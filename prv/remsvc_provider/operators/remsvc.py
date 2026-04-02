"""
remsvc_provider/operators/remsvc.py
=====================================
Deferrable RemSvcOperator for Apache Airflow.

Execution flow
--------------
1. execute()          — submit the job synchronously (fast), then defer.
                        Worker slot is released immediately after TaskDeferred.
2. [triggerer]        — RemSvcTrigger polls asynchronously until terminal state.
3. execute_complete() — worker resumes, fetches result, pushes to XCom.

The worker holds its slot only during step 1 (a single SubmitJob RPC) and
step 3 (a single GetResult RPC).  All waiting happens in the triggerer.

Repo layout context
-------------------
  RemSvc/
  ├── src/proto/          ← .proto source files
  └── prv/
      ├── remsvc_proto/   ← generated stubs (from src/proto/)
      ├── remsvc_provider/
      │   ├── operators/remsvc.py   ← this file
      │   └── triggers/remsvc.py
      └── tests/

Requirements
------------
  Apache Airflow >= 2.2  (TaskDeferred / deferrable operators)
  grpcio, grpcio-status
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, Optional, Sequence

import grpc
from google.protobuf.message import Message

from airflow.exceptions import AirflowException
from airflow.models import BaseOperator
from airflow.providers.grpc.hooks.grpc import GrpcHook
from airflow.utils.context import Context

# Stubs are generated from RemSvc/src/proto/ into RemSvc/prv/remsvc_proto/
try:
    from remsvc_proto import remsvc_pb2 as pb2              # type: ignore
    from remsvc_proto import remsvc_pb2_grpc as pb2_grpc    # type: ignore
    _PROTO_AVAILABLE = True
except ImportError:
    pb2 = None          # type: ignore[assignment]
    pb2_grpc = None     # type: ignore[assignment]
    _PROTO_AVAILABLE = False

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Shared types — imported by triggers/remsvc.py as well
# ---------------------------------------------------------------------------

class JobState(str, Enum):
    PENDING   = "PENDING"
    RUNNING   = "RUNNING"
    SUCCESS   = "SUCCESS"
    FAILED    = "FAILED"
    CANCELLED = "CANCELLED"

    @classmethod
    def terminal(cls) -> frozenset["JobState"]:
        return frozenset({cls.SUCCESS, cls.FAILED, cls.CANCELLED})

    @classmethod
    def from_proto(cls, proto_state: int) -> "JobState":
        """Map protobuf enum int to JobState."""
        mapping = {
            0: cls.PENDING,
            1: cls.RUNNING,
            2: cls.SUCCESS,
            3: cls.FAILED,
            4: cls.CANCELLED,
        }
        return mapping.get(proto_state, cls.PENDING)


@dataclass
class RemSvcResult:
    """Structured result pushed to XCom."""
    job_id:    str
    state:     str
    output:    Any                = None
    metadata:  Dict[str, str]     = field(default_factory=dict)
    error_msg: Optional[str]      = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "job_id":    self.job_id,
            "state":     self.state,
            "output":    self.output,
            "metadata":  self.metadata,
            "error_msg": self.error_msg,
        }


# ---------------------------------------------------------------------------
# Operator
# ---------------------------------------------------------------------------

class RemSvcOperator(BaseOperator):
    """Deferrable operator that submits a job to RemSvc and waits via the
    Airflow triggerer — no worker slot held during polling.

    Parameters
    ----------
    grpc_conn_id:
        Airflow connection ID of type ``grpc``.
    job_payload:
        Dict forwarded to ``SubmitJobRequest``.  Jinja-templated.
    poll_interval:
        Seconds between async ``GetStatus`` polls in the triggerer (default 10 s).
    poll_timeout:
        Maximum seconds the trigger will wait before firing a timeout event
        (default 3600 s).
    request_timeout:
        Per-RPC deadline in seconds (default 30 s).
    result_handler:
        Optional ``(GetResultResponse) -> Any`` transformer applied before XCom.
    xcom_key:
        XCom key (default ``"return_value"``).
    metadata:
        Optional gRPC call metadata sequence.

    Examples
    --------
    >>> run = RemSvcOperator(
    ...     task_id="run_remote",
    ...     grpc_conn_id="remsvc_prod",
    ...     job_payload={"script": "process.py", "args": ["--date", "{{ ds }}"]},
    ...     dag=dag,
    ... )
    """

    template_fields: Sequence[str] = ("job_payload", "metadata")
    template_fields_renderers = {"job_payload": "json"}
    ui_color = "#8f70d4"

    def __init__(
        self,
        *,
        grpc_conn_id:    str = "remsvc_default",
        job_payload:     Dict[str, Any],
        poll_interval:   float = 10.0,
        poll_timeout:    float = 3600.0,
        request_timeout: float = 30.0,
        result_handler:  Optional[Callable[[Any], Any]] = None,
        xcom_key:        str = "return_value",
        metadata:        Optional[Sequence[tuple[str, str]]] = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self.grpc_conn_id    = grpc_conn_id
        self.job_payload     = job_payload
        self.poll_interval   = poll_interval
        self.poll_timeout    = poll_timeout
        self.request_timeout = request_timeout
        self.result_handler  = result_handler or self._default_result_handler
        self.xcom_key        = xcom_key
        self.metadata        = list(metadata or [])

    # ------------------------------------------------------------------
    # Phase 1: submit → defer  (runs on a worker, briefly)
    # ------------------------------------------------------------------

    def execute(self, context: Context) -> None:
        if not _PROTO_AVAILABLE:
            raise ImportError(
                "RemSvc proto stubs not found. "
                "Run regen_proto.sh against RemSvc/src/proto/ and ensure "
                "the output is at RemSvc/prv/remsvc_proto/."
            )

        job_id = self._submit_job()
        log.info("RemSvc job submitted. job_id=%s  Deferring to triggerer.", job_id)

        # Push job_id immediately — visible in the Airflow UI while DEFERRED
        context["task_instance"].xcom_push(key="job_id", value=job_id)

        # Deferred import to avoid circular dependency at module load time
        from remsvc_provider.triggers.remsvc import RemSvcTrigger

        # Hand off to the triggerer; worker slot is released here.
        self.defer(
            trigger=RemSvcTrigger(
                job_id          = job_id,
                grpc_conn_id    = self.grpc_conn_id,
                poll_interval   = self.poll_interval,
                poll_timeout    = self.poll_timeout,
                request_timeout = self.request_timeout,
                metadata        = self.metadata,
            ),
            method_name="execute_complete",
        )
        # Unreachable — self.defer() raises TaskDeferred internally.

    # ------------------------------------------------------------------
    # Phase 3: resume → fetch → XCom  (runs on a worker, briefly)
    # ------------------------------------------------------------------

    def execute_complete(
        self,
        context: Context,
        event:   Dict[str, Any],
    ) -> Dict[str, Any]:
        """Called by Airflow when the trigger fires a TriggerEvent.

        ``event`` is the dict yielded by ``RemSvcTrigger.run()``.
        """
        from remsvc_provider.triggers.remsvc import EVT_ERROR, EVT_JOB_ID, EVT_STATE

        job_id = event.get(EVT_JOB_ID, "<unknown>")
        state  = JobState(event.get(EVT_STATE, JobState.FAILED.value))
        error  = event.get(EVT_ERROR)

        if state != JobState.SUCCESS:
            msg = f"RemSvc job {job_id!r} ended with state {state.value}."
            if error:
                msg += f"  Trigger error: {error}"
            raise AirflowException(msg)

        log.info("RemSvc job succeeded. job_id=%s  Fetching result.", job_id)
        raw_result = self._fetch_result(job_id)
        result = RemSvcResult(
            job_id = job_id,
            state  = state.value,
            output = self.result_handler(raw_result),
        )

        log.info("RemSvc result fetched. job_id=%s", job_id)
        return result.to_dict()

    # ------------------------------------------------------------------
    # Synchronous gRPC helpers
    # ------------------------------------------------------------------

    def _get_hook(self) -> GrpcHook:
        return GrpcHook(grpc_conn_id=self.grpc_conn_id)

    def _stub(self, channel: grpc.Channel) -> Any:
        return pb2_grpc.RemSvcStub(channel)  # type: ignore[attr-defined]

    def _submit_job(self) -> str:
        with self._get_hook().get_conn() as channel:
            stub     = self._stub(channel)
            request  = pb2.SubmitJobRequest(**self.job_payload)   # type: ignore[attr-defined]
            response = stub.SubmitJob(
                request,
                timeout  = self.request_timeout,
                metadata = self.metadata,
            )
            return response.job_id

    def _fetch_result(self, job_id: str) -> Any:
        with self._get_hook().get_conn() as channel:
            stub    = self._stub(channel)
            request = pb2.GetResultRequest(job_id=job_id)         # type: ignore[attr-defined]
            return stub.GetResult(
                request,
                timeout  = self.request_timeout,
                metadata = self.metadata,
            )

    # ------------------------------------------------------------------
    # Result transformation
    # ------------------------------------------------------------------

    @staticmethod
    def _default_result_handler(response: Any) -> Any:
        if isinstance(response, Message):
            from google.protobuf.json_format import MessageToDict
            return MessageToDict(response, preserving_proto_field_name=True)
        return response
