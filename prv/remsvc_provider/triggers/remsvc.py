"""
remsvc_provider/triggers/remsvc.py
===================================
Async trigger for the RemSvc gRPC service.

Runs inside the Airflow triggerer process (a single asyncio event loop shared
across all deferred tasks).  Opens one ``RemCmdStrm`` bidirectional stream,
concurrently sends all commands and reads all responses, then correlates
each response back to its originating command via the ``tid`` field.

Correlation model
-----------------
Each command is assigned a ``tid`` equal to its 1-based position in the
``commands`` list.  Responses may arrive in any order; they are placed into
a ``{tid: result_dict}`` mapping.  When the writer and reader both finish,
the trigger checks for missing responses and any non-zero return codes, then
yields exactly one TriggerEvent carrying the full results map.

This means the single-command case (N=1) works identically — one message is
sent with ``tid=1`` and one response is expected with ``tid=1``.

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
  pip install apache-airflow>=3.1.0 grpcio grpcio-status
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import AsyncIterator, Sequence
from typing import Any

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

from remsvc_provider.operators.remsvc import JobState, crc32hex

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Trigger event payload keys — shared constants used by operator and trigger
# ---------------------------------------------------------------------------
EVT_STATE   = "state"
EVT_RESULTS = "results"   # dict[int, dict[str, Any]]  keyed by tid
EVT_ERROR   = "error"


class RemSvcTrigger(BaseTrigger):
    """Async trigger that executes commands via RemCmdStrm and correlates
    responses by ``tid``.

    Serialised into the Airflow metadata DB when the operator defers; rebuilt
    in the triggerer process via ``serialize()`` / constructor.

    Parameters
    ----------
    commands:
        List of command dicts forwarded to ``RemCmdMsg``.  ``tid`` is
        assigned automatically (1-based index in this list).
    grpc_conn_id:
        Airflow connection pointing at the RemSvc host.
    stream_timeout:
        Maximum seconds for the entire stream (default 3600 s).
        Enforced at two levels:
        - Python: ``asyncio.wait_for`` cancels the coroutine if exceeded.
        - gRPC:   passed as the deadline when opening ``RemCmdStrm`` so the
                  server also receives ``DEADLINE_EXCEEDED`` and can clean up.
    metadata:
        Optional gRPC call metadata (e.g. bearer tokens).
    """

    def __init__(
        self,
        *,
        commands:       list[dict[str, Any]],
        grpc_conn_id:   str   = "remsvc_default",
        stream_timeout: float = 3600.0,
        metadata:       Sequence[tuple[str, str]] | None = None,
    ) -> None:
        super().__init__()
        self.commands       = commands
        self.grpc_conn_id   = grpc_conn_id
        self.stream_timeout = stream_timeout
        self.metadata       = list(metadata or [])

    # ------------------------------------------------------------------
    # BaseTrigger contract
    # ------------------------------------------------------------------

    def serialize(self) -> tuple[str, dict[str, Any]]:
        """Return (classpath, kwargs) so Airflow can store and rebuild this
        trigger in the triggerer process after a restart."""
        return (
            "remsvc_provider.triggers.remsvc.RemSvcTrigger",
            {
                "commands":       self.commands,
                "grpc_conn_id":   self.grpc_conn_id,
                "stream_timeout": self.stream_timeout,
                "metadata":       self.metadata,
            },
        )

    async def run(self) -> AsyncIterator[TriggerEvent]:
        """Async generator: yield exactly one TriggerEvent when done."""
        if not _PROTO_AVAILABLE:
            yield TriggerEvent({
                EVT_STATE:   JobState.FAILED.value,
                EVT_RESULTS: {},
                EVT_ERROR:   (
                    "RemSvc proto stubs not found. "
                    "Run regen_proto.sh against RemSvc/src/proto/ and ensure "
                    "the output is at RemSvc/prv/remsvc_proto/."
                ),
            })
            return

        log.info(
            "RemSvcTrigger started: %d command(s)  timeout=%.0fs",
            len(self.commands), self.stream_timeout,
        )
        try:
            event = await asyncio.wait_for(
                self._run_stream(),
                timeout=self.stream_timeout,
            )
            yield TriggerEvent(event)

        except asyncio.TimeoutError:
            log.warning(
                "RemSvcTrigger timed out after %.0fs", self.stream_timeout,
            )
            yield TriggerEvent({
                EVT_STATE:   JobState.CANCELLED.value,
                EVT_RESULTS: {},
                EVT_ERROR:   f"Stream timed out after {self.stream_timeout}s",
            })

        except grpc.aio.AioRpcError as exc:
            log.error(
                "RemSvcTrigger gRPC error: code=%s details=%s",
                exc.code(), exc.details(),
            )
            yield TriggerEvent({
                EVT_STATE:   JobState.FAILED.value,
                EVT_RESULTS: {},
                EVT_ERROR:   f"gRPC error {exc.code()}: {exc.details()}",
            })

        except Exception as exc:  # noqa: BLE001
            log.exception("RemSvcTrigger unexpected error")
            yield TriggerEvent({
                EVT_STATE:   JobState.FAILED.value,
                EVT_RESULTS: {},
                EVT_ERROR:   repr(exc),
            })

    # ------------------------------------------------------------------
    # Stream execution
    # ------------------------------------------------------------------

    async def _run_stream(self) -> dict[str, Any]:
        """Open RemCmdStrm, concurrently write all commands and read all
        responses, then return a completed event payload dict."""
        async with self._async_channel() as channel:
            stub   = self._async_stub(channel)
            stream = stub.RemCmdStrm(
                timeout  = self.stream_timeout,
                metadata = self.metadata,
            )

            # Assign tid = 1-based index; keep a local copy for the closure.
            indexed: list[tuple[int, dict[str, Any]]] = [
                (i + 1, cmd) for i, cmd in enumerate(self.commands)
            ]
            results: dict[int, dict[str, Any]] = {}

            async def _write() -> None:
                for tid, cmd_dict in indexed:
                    cmd_str = cmd_dict.get("cmd", "")
                    msg = pb2.RemCmdMsg(                         # type: ignore[attr-defined]
                        cmd    = cmd_str,
                        cmdtyp = cmd_dict.get("cmdtyp", 0),
                        cmdusr = cmd_dict.get("cmdusr", ""),
                        src    = cmd_dict.get("src", ""),
                        tid    = tid,
                        hsh    = crc32hex(cmd_str),
                    )
                    await stream.write(msg)
                    log.debug("Sent    tid=%d  cmd=%r", tid, cmd_str)
                await stream.done_writing()
                log.debug("Writer finished (%d command(s) sent).", len(indexed))

            async def _read() -> None:
                async for response in stream:
                    tid = response.tid
                    if tid in results:
                        log.warning(
                            "Duplicate response for tid=%d — overwriting previous entry",
                            tid,
                        )
                    # Look up the original command string for reference.
                    cmd_str = (
                        indexed[tid - 1][1].get("cmd", "")
                        if 0 < tid <= len(indexed) else ""
                    )
                    results[tid] = {
                        "tid": tid,
                        "rc":  response.rc,
                        "out": response.out,
                        "err": response.err,
                        "hsh": response.hsh,
                        "cmd": cmd_str,
                    }
                    log.debug("Received tid=%d  rc=%d", tid, response.rc)
                log.debug("Reader finished (%d response(s) received).", len(results))

            # gather() uses return_exceptions=False (default): if _write() raises
            # (e.g. server closes the stream mid-write) it cancels _read() and
            # re-raises, discarding any partial results already in `results`.
            # The outer exception handlers in run() will catch this and yield FAILED.
            await asyncio.gather(_write(), _read())

        # ---- post-stream analysis ----------------------------------------

        sent_tids    = {tid for tid, _ in indexed}
        missing_tids = sent_tids - results.keys()
        if missing_tids:
            return {
                EVT_STATE:   JobState.FAILED.value,
                EVT_RESULTS: results,
                EVT_ERROR:   f"No response for tid(s): {sorted(missing_tids)}",
            }

        failed_tids = sorted(tid for tid, r in results.items() if r["rc"] != 0)
        if failed_tids:
            log.warning(
                "Commands with non-zero rc: tids=%s", failed_tids,
            )
            return {
                EVT_STATE:   JobState.FAILED.value,
                EVT_RESULTS: results,
                EVT_ERROR:   f"Non-zero rc for tid(s): {failed_tids}",
            }

        return {
            EVT_STATE:   JobState.SUCCESS.value,
            EVT_RESULTS: results,
        }

    # ------------------------------------------------------------------
    # gRPC async helpers
    # ------------------------------------------------------------------

    def _async_channel(self) -> grpc.aio.Channel:
        """Build an async gRPC channel via RemSvcHook (respects TLS config)."""
        from remsvc_provider.hooks.remsvc import RemSvcHook
        return RemSvcHook(self.grpc_conn_id).get_async_channel()

    def _async_stub(self, channel: grpc.aio.Channel) -> Any:
        return pb2_grpc.RemSvcStub(channel)  # type: ignore[attr-defined]
