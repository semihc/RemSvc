"""
remsvc_provider/operators/remsvc.py
=====================================
Deferrable RemSvcOperator for Apache Airflow.

Execution flow
--------------
1. execute()          — validate inputs, defer to RemSvcTrigger.
                        Worker slot is released immediately after TaskDeferred.
2. [triggerer]        — RemSvcTrigger opens a RemCmdStrm bidirectional stream,
                        concurrently sends all commands and reads all responses,
                        correlating each response to its command via ``tid``.
3. execute_complete() — worker resumes, receives correlated results, pushes to XCom.

The worker holds its slot only during steps 1 and 3 (brief validation + XCom push).
All command execution and waiting happens in the triggerer process.

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
  Apache Airflow >= 3.1  (Python 3.10+)
  grpcio >= 1.67.0
"""

from __future__ import annotations

import logging
import zlib
from dataclasses import dataclass, field
from enum import Enum
from collections.abc import Callable, Sequence
from typing import Any

from airflow.exceptions import AirflowException
from airflow.models import BaseOperator
from airflow.utils.context import Context

# Check that stubs have been generated — actual proto types are used in the trigger.
try:
    import remsvc_proto.remsvc_pb2  # type: ignore  # noqa: F401
    _PROTO_AVAILABLE = True
except ImportError:
    _PROTO_AVAILABLE = False

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Shared types — imported by triggers/remsvc.py as well
# ---------------------------------------------------------------------------

class JobState(str, Enum):
    SUCCESS   = "SUCCESS"
    FAILED    = "FAILED"
    CANCELLED = "CANCELLED"


def crc32hex(data: str) -> str:
    return format(zlib.crc32(data.encode("utf-8")) & 0xFFFFFFFF, "08x")


@dataclass
class RemSvcResult:
    """Structured result pushed to XCom."""
    state:     str
    results:   list[dict[str, Any]] = field(default_factory=list)
    error_msg: str | None        = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "state":     self.state,
            "results":   self.results,
            "error_msg": self.error_msg,
        }


# ---------------------------------------------------------------------------
# Operator
# ---------------------------------------------------------------------------

class RemSvcOperator(BaseOperator):
    """Deferrable operator that executes one or more remote commands via the
    RemSvc bidirectional streaming RPC (``RemCmdStrm``).

    All commands are sent over a single stream; responses are correlated back
    to their originating command via the ``tid`` field (1-based index in the
    ``commands`` list).  Responses may arrive in any order — the trigger
    collects them all before firing the completion event.

    Parameters
    ----------
    commands:
        List of command dicts forwarded to ``RemCmdMsg``.  Each dict must
        contain at least ``cmd`` (str).  Optional fields: ``cmdtyp`` (int,
        0=native shell, 1=PowerShell), ``cmdusr`` (str), ``src`` (str).
        Jinja-templated.
    grpc_conn_id:
        Airflow connection ID of type ``remsvc``.
    stream_timeout:
        Maximum seconds for the entire stream, enforced at both the Python
        asyncio level and as the gRPC deadline on the stream (default 3600 s).
        If the gRPC channel drops mid-stream (e.g. a network blip), the trigger
        raises ``AioRpcError(UNAVAILABLE)`` and the task fails immediately —
        there is no automatic reconnect.  For resilience, set ``retries`` on
        the operator or wrap in an Airflow retry policy.
    result_handler:
        Optional callable applied to the raw ``{tid: result_dict}`` mapping
        before it is stored in XCom.  Defaults to a sorted list by tid.
    metadata:
        Optional gRPC call metadata sequence (e.g. bearer tokens).
        Jinja-templated.

    Examples
    --------
    >>> op = RemSvcOperator(
    ...     task_id="run_remote",
    ...     grpc_conn_id="remsvc_prod",
    ...     commands=[
    ...         {"cmd": "echo {{ ds }}", "cmdtyp": 0},
    ...         {"cmd": "hostname",      "cmdtyp": 0},
    ...     ],
    ...     dag=dag,
    ... )
    """

    template_fields: Sequence[str] = ("commands", "metadata")
    template_fields_renderers = {"commands": "json"}
    ui_color = "#8f70d4"

    def __init__(
        self,
        *,
        grpc_conn_id:   str = "remsvc_default",
        commands:       list[dict[str, Any]],
        stream_timeout: float = 3600.0,
        result_handler: Callable[[Any], Any] | None = None,
        metadata:       Sequence[tuple[str, str]] | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self.grpc_conn_id   = grpc_conn_id
        self.commands       = commands
        self.stream_timeout = stream_timeout
        self.result_handler = result_handler or self._default_result_handler
        self.metadata       = list(metadata or [])

    # ------------------------------------------------------------------
    # Phase 1: validate → defer  (runs on a worker, briefly)
    # ------------------------------------------------------------------

    def execute(self, context: Context) -> None:
        if not _PROTO_AVAILABLE:
            raise ImportError(
                "RemSvc proto stubs not found. "
                "Run regen_proto.sh against RemSvc/src/proto/ and ensure "
                "the output is at RemSvc/prv/remsvc_proto/."
            )
        if not self.commands:
            raise AirflowException("RemSvcOperator: 'commands' must not be empty.")

        empty_idxs = [i for i, c in enumerate(self.commands) if not c.get("cmd", "").strip()]
        if empty_idxs:
            raise AirflowException(
                f"RemSvcOperator: 'cmd' is empty or whitespace-only at "
                f"index(es) {empty_idxs} in 'commands'."
            )

        # Deferred import to avoid circular dependency at module load time
        from remsvc_provider.triggers.remsvc import RemSvcTrigger

        log.info(
            "RemSvcOperator: deferring %d command(s) to triggerer.",
            len(self.commands),
        )
        self.defer(
            trigger=RemSvcTrigger(
                commands       = self.commands,
                grpc_conn_id   = self.grpc_conn_id,
                stream_timeout = self.stream_timeout,
                metadata       = self.metadata,
            ),
            method_name="execute_complete",
        )
        # Unreachable — self.defer() raises TaskDeferred internally.

    # ------------------------------------------------------------------
    # Phase 3: resume → XCom  (runs on a worker, briefly)
    # ------------------------------------------------------------------

    def execute_complete(
        self,
        context: Context,
        event:   dict[str, Any],
    ) -> dict[str, Any]:
        """Called by Airflow when the trigger fires a TriggerEvent.

        ``event`` is the dict yielded by ``RemSvcTrigger.run()``.
        """
        from remsvc_provider.triggers.remsvc import EVT_ERROR, EVT_RESULTS, EVT_STATE

        state   = JobState(event.get(EVT_STATE, JobState.FAILED.value))
        error   = event.get(EVT_ERROR)
        raw     = event.get(EVT_RESULTS, {})

        if state != JobState.SUCCESS:
            msg = f"RemSvc commands ended with state {state.value}."
            if error:
                msg += f"  Detail: {error}"
            raise AirflowException(msg)

        log.info("RemSvc commands completed successfully (%d result(s)).", len(raw))
        result = RemSvcResult(
            state   = state.value,
            results = self.result_handler(raw),
        )
        return result.to_dict()

    # ------------------------------------------------------------------
    # Result transformation
    # ------------------------------------------------------------------

    @staticmethod
    def _default_result_handler(
        results: dict[int, dict[str, Any]],
    ) -> list[dict[str, Any]]:
        """Convert ``{tid: result_dict}`` to a list sorted ascending by tid."""
        return [v for _, v in sorted(results.items())]
