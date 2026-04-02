"""
remsvc_provider/triggers/remsvc.py
===================================
Async trigger for the RemSvc gRPC service.

Runs inside the Airflow triggerer process (a single asyncio event loop shared
across all deferred tasks).  Polls RemSvc via grpc.aio, yields a TriggerEvent
when the job reaches a terminal state, and holds no worker slot during polling.

Repo layout context
-------------------
  RemSvc/
  ├── src/proto/          ← .proto source files
  └── prv/
      ├── remsvc_proto/   ← generated stubs (from src/proto/)
      ├── remsvc_provider/
      │   ├── operators/remsvc.py
      │   └── triggers/remsvc.py   ← this file
      └── tests/

Requirements
------------
  pip install apache-airflow>=2.2.0 grpcio grpcio-status
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any, AsyncIterator, Dict, Optional, Sequence

import grpc
import grpc.aio

from airflow.triggers.base import BaseTrigger, TriggerEvent

# Stubs are generated from RemSvc/src/proto/ into RemSvc/prv/remsvc_proto/
try:
    from remsvc_proto import remsvc_pb2 as pb2              # type: ignore
    from remsvc_proto import remsvc_pb2_grpc as pb2_grpc    # type: ignore
    _PROTO_AVAILABLE = True
except ImportError:
    pb2 = None          # type: ignore[assignment]
    pb2_grpc = None     # type: ignore[assignment]
    _PROTO_AVAILABLE = False

from remsvc_provider.operators.remsvc import JobState

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Trigger event payload keys — shared constants used by operator and trigger
# ---------------------------------------------------------------------------
EVT_JOB_ID = "job_id"
EVT_STATE  = "state"
EVT_ERROR  = "error"


class RemSvcTrigger(BaseTrigger):
    """Async trigger that polls RemSvc until a job reaches a terminal state.

    Serialised into the Airflow metadata DB when the operator defers; rebuilt
    in the triggerer process via ``serialize()`` / constructor.

    Parameters
    ----------
    job_id:
        The RemSvc job identifier returned by SubmitJob.
    grpc_conn_id:
        Airflow connection pointing at the RemSvc host.
    poll_interval:
        Seconds between GetStatus calls (default 10 s).
        ``asyncio.sleep`` is used — non-blocking inside the triggerer loop.
    poll_timeout:
        Maximum seconds before the trigger fires a CANCELLED event (default 3600 s).
    request_timeout:
        Per-RPC deadline in seconds (default 30 s).
    metadata:
        Optional gRPC call metadata (e.g. bearer tokens).
    """

    def __init__(
        self,
        *,
        job_id:          str,
        grpc_conn_id:    str   = "remsvc_default",
        poll_interval:   float = 10.0,
        poll_timeout:    float = 3600.0,
        request_timeout: float = 30.0,
        metadata:        Optional[Sequence[tuple[str, str]]] = None,
    ) -> None:
        super().__init__()
        self.job_id          = job_id
        self.grpc_conn_id    = grpc_conn_id
        self.poll_interval   = poll_interval
        self.poll_timeout    = poll_timeout
        self.request_timeout = request_timeout
        self.metadata        = list(metadata or [])

    # ------------------------------------------------------------------
    # BaseTrigger contract
    # ------------------------------------------------------------------

    def serialize(self) -> tuple[str, Dict[str, Any]]:
        """Return (classpath, kwargs) so Airflow can store and rebuild this
        trigger in the triggerer process after a restart."""
        return (
            "remsvc_provider.triggers.remsvc.RemSvcTrigger",
            {
                "job_id":          self.job_id,
                "grpc_conn_id":    self.grpc_conn_id,
                "poll_interval":   self.poll_interval,
                "poll_timeout":    self.poll_timeout,
                "request_timeout": self.request_timeout,
                "metadata":        self.metadata,
            },
        )

    async def run(self) -> AsyncIterator[TriggerEvent]:
        """Async generator: yield exactly one TriggerEvent when done."""
        log.info(
            "RemSvcTrigger started. job_id=%s  poll_interval=%ss  timeout=%ss",
            self.job_id, self.poll_interval, self.poll_timeout,
        )

        try:
            async with self._async_channel() as channel:
                stub    = self._async_stub(channel)
                elapsed = 0.0

                while True:
                    if elapsed >= self.poll_timeout:
                        log.warning(
                            "RemSvcTrigger timed out. job_id=%s elapsed=%.0fs",
                            self.job_id, elapsed,
                        )
                        yield TriggerEvent({
                            EVT_JOB_ID: self.job_id,
                            EVT_STATE:  JobState.CANCELLED.value,
                            EVT_ERROR:  f"Trigger timed out after {self.poll_timeout}s",
                        })
                        return

                    state = await self._get_status(stub)
                    log.debug(
                        "job_id=%s  state=%s  elapsed=%.0fs",
                        self.job_id, state.value, elapsed,
                    )

                    if state in JobState.terminal():
                        log.info(
                            "RemSvcTrigger finished. job_id=%s  state=%s",
                            self.job_id, state.value,
                        )
                        yield TriggerEvent({
                            EVT_JOB_ID: self.job_id,
                            EVT_STATE:  state.value,
                        })
                        return

                    await asyncio.sleep(self.poll_interval)
                    elapsed += self.poll_interval

        except grpc.aio.AioRpcError as exc:
            log.error(
                "RemSvcTrigger gRPC error. job_id=%s  code=%s  details=%s",
                self.job_id, exc.code(), exc.details(),
            )
            yield TriggerEvent({
                EVT_JOB_ID: self.job_id,
                EVT_STATE:  JobState.FAILED.value,
                EVT_ERROR:  f"gRPC error {exc.code()}: {exc.details()}",
            })

        except Exception as exc:  # noqa: BLE001
            log.exception("RemSvcTrigger unexpected error. job_id=%s", self.job_id)
            yield TriggerEvent({
                EVT_JOB_ID: self.job_id,
                EVT_STATE:  JobState.FAILED.value,
                EVT_ERROR:  repr(exc),
            })

    # ------------------------------------------------------------------
    # gRPC async helpers
    # ------------------------------------------------------------------

    def _async_channel(self) -> grpc.aio.Channel:
        """Build an async gRPC channel from the Airflow connection."""
        from airflow.hooks.base import BaseHook
        conn   = BaseHook.get_connection(self.grpc_conn_id)
        target = f"{conn.host}:{conn.port}"
        # For TLS: swap to grpc.aio.secure_channel if conn.extra_dejson.get("use_ssl")
        return grpc.aio.insecure_channel(target)

    @staticmethod
    def _async_stub(channel: grpc.aio.Channel) -> Any:
        return pb2_grpc.RemSvcStub(channel)  # type: ignore[attr-defined]

    async def _get_status(self, stub: Any) -> JobState:
        request  = pb2.GetStatusRequest(job_id=self.job_id)  # type: ignore[attr-defined]
        response = await stub.GetStatus(
            request,
            timeout  = self.request_timeout,
            metadata = self.metadata,
        )
        return JobState.from_proto(response.state)
