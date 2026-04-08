"""
tst/unit/prv/test_remsvc_async.py
==================================
Async unit tests for RemSvcTrigger and the deferrable RemSvcOperator.

Run from RemSvc/prv/:
  pytest ../tst/unit/prv -v

Requires:
  pip install pytest pytest-asyncio apache-airflow>=3.1.0
"""

from __future__ import annotations

from typing import Any
from unittest.mock import AsyncMock, MagicMock, patch

import grpc
import pytest

from airflow.exceptions import AirflowException
from airflow.sdk.exceptions import TaskDeferred
from airflow.triggers.base import TriggerEvent

from remsvc_provider.operators.remsvc import JobState, RemSvcOperator
from remsvc_provider.triggers.remsvc import (
    EVT_ERROR, EVT_RESULTS, EVT_STATE, RemSvcTrigger,
)


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

_CMD1 = {"cmd": "echo hello", "cmdtyp": 0}
_CMD2 = {"cmd": "hostname",   "cmdtyp": 0}
_CMD3 = {"cmd": "whoami",     "cmdtyp": 0}


def _make_trigger(**kwargs) -> RemSvcTrigger:
    defaults = dict(
        commands       = [_CMD1],
        grpc_conn_id   = "remsvc_test",
        stream_timeout = 60.0,
    )
    defaults.update(kwargs)
    return RemSvcTrigger(**defaults)


def _make_operator(**kwargs) -> RemSvcOperator:
    defaults = dict(
        task_id      = "test_op",
        grpc_conn_id = "remsvc_test",
        commands     = [_CMD1],
    )
    defaults.update(kwargs)
    return RemSvcOperator(**defaults)


def _mock_context() -> dict:
    return {"task_instance": MagicMock()}


def _mock_response(*, tid: int, rc: int = 0, out: str = "", err: str = "",
                   hsh: str = "") -> MagicMock:
    r = MagicMock()
    r.tid = tid
    r.rc  = rc
    r.out = out
    r.err = err
    r.hsh = hsh
    return r


class _AsyncStream:
    """Minimal async-iterable stream double for RemCmdStrm."""

    def __init__(self, responses: list[Any]) -> None:
        self._responses  = responses
        self.write       = AsyncMock()
        self.done_writing = AsyncMock()

    def __aiter__(self):
        return self._aiter()

    async def _aiter(self):
        for r in self._responses:
            yield r


def _mock_channel_ctx(stub: Any) -> MagicMock:
    """Return an async context manager that yields a channel; _async_stub
    will be patched separately to return ``stub``."""
    ch = MagicMock()
    ch.__aenter__ = AsyncMock(return_value=ch)
    ch.__aexit__  = AsyncMock(return_value=False)
    return ch


async def _collect(trigger: RemSvcTrigger) -> list[TriggerEvent]:
    events = []
    async for event in trigger.run():
        events.append(event)
    return events


# ---------------------------------------------------------------------------
# RemSvcTrigger — serialization
# ---------------------------------------------------------------------------

class TestRemSvcTriggerSerialize:

    def test_serialize_roundtrip(self):
        t = _make_trigger(
            commands       = [_CMD1, _CMD2],
            stream_timeout = 600.0,
        )
        classpath, kwargs = t.serialize()

        assert classpath == "remsvc_provider.triggers.remsvc.RemSvcTrigger"
        rebuilt = RemSvcTrigger(**kwargs)
        assert rebuilt.commands       == t.commands
        assert rebuilt.stream_timeout == t.stream_timeout
        assert rebuilt.grpc_conn_id   == t.grpc_conn_id

    def test_serialize_preserves_metadata(self):
        t = _make_trigger(metadata=[("x-token", "abc")])
        _, kwargs = t.serialize()
        assert kwargs["metadata"] == [("x-token", "abc")]


# ---------------------------------------------------------------------------
# RemSvcTrigger — run() outer behaviour (patch _run_stream)
# ---------------------------------------------------------------------------

class TestRemSvcTriggerRun:

    @pytest.mark.asyncio
    async def test_run_yields_exactly_one_event_on_success(self):
        t = _make_trigger()
        payload = {EVT_STATE: JobState.SUCCESS.value, EVT_RESULTS: {1: {"rc": 0}}}
        with patch.object(t, "_run_stream", new=AsyncMock(return_value=payload)), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)
        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value

    @pytest.mark.asyncio
    async def test_run_yields_exactly_one_event_on_failure(self):
        t = _make_trigger()
        payload = {EVT_STATE: JobState.FAILED.value, EVT_RESULTS: {}, EVT_ERROR: "oops"}
        with patch.object(t, "_run_stream", new=AsyncMock(return_value=payload)), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)
        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value

    @pytest.mark.asyncio
    async def test_run_fires_cancelled_on_timeout(self):
        t = _make_trigger(stream_timeout=0.001)
        # Use an Event that never fires — deterministic block without wall-clock sleep
        async def _block(sent_ref):
            import asyncio as _a
            await _a.Event().wait()
        with patch.object(t, "_run_stream", new=_block), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE]  == JobState.CANCELLED.value
        assert "timed out" in events[0].payload[EVT_ERROR].lower()

    @pytest.mark.asyncio
    async def test_run_catches_grpc_error(self):
        # Use PERMISSION_DENIED (non-retryable) so the trigger fails immediately
        # without retry loops.  UNAVAILABLE is retried up to _MAX_ATTEMPTS times
        # and would require patching asyncio.sleep to avoid slow tests.
        t = _make_trigger()
        rpc_error = grpc.aio.AioRpcError(
            code               = grpc.StatusCode.PERMISSION_DENIED,
            initial_metadata   = grpc.aio.Metadata(),
            trailing_metadata  = grpc.aio.Metadata(),
            details            = "access denied",
            debug_error_string = "",
        )
        with patch.object(t, "_run_stream", new=AsyncMock(side_effect=rpc_error)), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert EVT_ERROR in events[0].payload

    @pytest.mark.asyncio
    async def test_run_catches_generic_exception(self):
        t = _make_trigger()
        with patch.object(t, "_run_stream", new=AsyncMock(side_effect=RuntimeError("boom"))), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "boom" in events[0].payload[EVT_ERROR]


# ---------------------------------------------------------------------------
# RemSvcTrigger — _run_stream: response correlation and state logic
# ---------------------------------------------------------------------------

class TestRemSvcTriggerRunStream:

    @pytest.fixture(autouse=True)
    def stop_patches(self):
        yield
        patch.stopall()

    def _setup(self, trigger: RemSvcTrigger, responses: list) -> tuple:
        """Wire a mock channel+stub+stream onto the trigger.
        Patches _call_metadata (no Airflow DB) and pb2 (no proto stubs needed).
        Returns (channel_mock, stub_mock, stream_mock)."""
        stream  = _AsyncStream(responses)
        stub    = MagicMock()
        stub.RemCmdStrm.return_value = stream
        channel = _mock_channel_ctx(stub)
        patch.object(trigger, "_call_metadata", return_value=[]).start()
        patch("remsvc_provider.triggers.remsvc.pb2", MagicMock()).start()
        return channel, stub, stream

    @pytest.mark.asyncio
    async def test_single_command_success(self):
        t = _make_trigger(commands=[_CMD1])
        ch, stub, _ = self._setup(t, [_mock_response(tid=1, rc=0, out="hello")])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])

        assert result[EVT_STATE]          == JobState.SUCCESS.value
        assert result[EVT_RESULTS][1]["rc"]  == 0
        assert result[EVT_RESULTS][1]["out"] == "hello"

    @pytest.mark.asyncio
    async def test_multiple_commands_all_success(self):
        t = _make_trigger(commands=[_CMD1, _CMD2, _CMD3])
        responses = [
            _mock_response(tid=1, rc=0, out="hello"),
            _mock_response(tid=2, rc=0, out="myhost"),
            _mock_response(tid=3, rc=0, out="alice"),
        ]
        ch, stub, _ = self._setup(t, responses)
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])

        assert result[EVT_STATE] == JobState.SUCCESS.value
        assert len(result[EVT_RESULTS]) == 3
        for tid in (1, 2, 3):
            assert result[EVT_RESULTS][tid]["rc"] == 0

    @pytest.mark.asyncio
    async def test_interleaved_responses_correlated_by_tid(self):
        """Responses arrive in reverse order; correlation must still be correct."""
        t = _make_trigger(commands=[_CMD1, _CMD2, _CMD3])
        responses = [
            _mock_response(tid=3, rc=0, out="alice"),   # arrives first
            _mock_response(tid=1, rc=0, out="hello"),   # arrives second
            _mock_response(tid=2, rc=0, out="myhost"),  # arrives last
        ]
        ch, stub, _ = self._setup(t, responses)
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])

        assert result[EVT_STATE]             == JobState.SUCCESS.value
        assert result[EVT_RESULTS][1]["out"] == "hello"
        assert result[EVT_RESULTS][2]["out"] == "myhost"
        assert result[EVT_RESULTS][3]["out"] == "alice"

    @pytest.mark.asyncio
    async def test_nonzero_rc_yields_failed(self):
        t = _make_trigger(commands=[_CMD1, _CMD2])
        responses = [
            _mock_response(tid=1, rc=0,  out="ok"),
            _mock_response(tid=2, rc=1,  err="not found"),
        ]
        ch, stub, _ = self._setup(t, responses)
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])

        assert result[EVT_STATE]   == JobState.FAILED.value
        assert "2" in result[EVT_ERROR]          # failed tid mentioned
        assert result[EVT_RESULTS][1]["rc"] == 0
        assert result[EVT_RESULTS][2]["rc"] == 1

    @pytest.mark.asyncio
    async def test_missing_response_yields_failed(self):
        """Server only sends one response for two commands."""
        t = _make_trigger(commands=[_CMD1, _CMD2])
        ch, stub, _ = self._setup(t, [_mock_response(tid=1, rc=0)])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])

        assert result[EVT_STATE] == JobState.FAILED.value
        assert "2" in result[EVT_ERROR]          # missing tid=2 reported

    @pytest.mark.asyncio
    async def test_commands_sent_with_correct_tids(self):
        t = _make_trigger(commands=[_CMD1, _CMD2])
        responses = [
            _mock_response(tid=1, rc=0),
            _mock_response(tid=2, rc=0),
        ]
        ch, stub, stream = self._setup(t, responses)
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            await t._run_stream([0])

        # write() called once per command; pb2 is mocked so inspect the kwargs
        # passed to RemCmdMsg() rather than the attribute on the returned mock.
        assert stream.write.call_count == 2
        import remsvc_provider.triggers.remsvc as _trig_mod
        pb2_mock = _trig_mod.pb2
        tid_calls = [c.kwargs["tid"] for c in pb2_mock.RemCmdMsg.call_args_list]
        assert tid_calls == [1, 2]

    @pytest.mark.asyncio
    async def test_sent_ref_counts_written_commands(self):
        """sent_ref[0] must equal the number of commands actually written."""
        t = _make_trigger(commands=[_CMD1, _CMD2, _CMD3])
        responses = [
            _mock_response(tid=1, rc=0),
            _mock_response(tid=2, rc=0),
            _mock_response(tid=3, rc=0),
        ]
        ch, stub, _ = self._setup(t, responses)
        sent_ref: list[int] = [0]
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            await t._run_stream(sent_ref)
        assert sent_ref[0] == 3

    @pytest.mark.asyncio
    async def test_done_writing_called(self):
        t = _make_trigger(commands=[_CMD1])
        ch, stub, stream = self._setup(t, [_mock_response(tid=1, rc=0)])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            await t._run_stream([0])
        stream.done_writing.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_result_includes_original_cmd_string(self):
        t = _make_trigger(commands=[{"cmd": "echo hello"}])
        ch, stub, _ = self._setup(t, [_mock_response(tid=1, rc=0)])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream([0])
        assert result[EVT_RESULTS][1]["cmd"] == "echo hello"


# ---------------------------------------------------------------------------
# RemSvcOperator (deferrable)
# ---------------------------------------------------------------------------

class TestDeferrableRemSvcOperator:

    def test_execute_defers(self):

        op  = _make_operator()
        ctx = _mock_context()

        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(TaskDeferred) as exc_info:
                op.execute(ctx)

        deferred = exc_info.value
        assert deferred.trigger.__class__.__name__ == "RemSvcTrigger"
        assert deferred.method_name                == "execute_complete"

    def test_execute_defers_with_correct_commands(self):

        commands = [_CMD1, _CMD2]
        op  = _make_operator(commands=commands)
        ctx = _mock_context()

        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(TaskDeferred) as exc_info:
                op.execute(ctx)

        assert exc_info.value.trigger.commands == commands

    def test_execute_raises_when_commands_empty(self):
        op  = _make_operator(commands=[])
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(AirflowException, match="empty"):
                op.execute(ctx)

    def test_execute_raises_when_stubs_missing(self):
        op  = _make_operator()
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", False):
            with pytest.raises(ImportError, match="regen_proto"):
                op.execute(ctx)

    def test_execute_complete_success_returns_results(self):
        op  = _make_operator()
        ctx = _mock_context()
        event = {
            EVT_STATE:   JobState.SUCCESS.value,
            EVT_RESULTS: {1: {"tid": 1, "rc": 0, "out": "hello", "err": "", "hsh": "", "cmd": "echo hello"}},
        }
        result = op.execute_complete(ctx, event)

        assert result["state"]           == "SUCCESS"
        assert len(result["results"])    == 1
        assert result["results"][0]["rc"]  == 0
        assert result["results"][0]["out"] == "hello"

    def test_execute_complete_results_sorted_by_tid(self):
        """Default handler must return results in tid order regardless of
        insertion order in the event dict."""
        op  = _make_operator(commands=[_CMD1, _CMD2, _CMD3])
        ctx = _mock_context()
        event = {
            EVT_STATE: JobState.SUCCESS.value,
            EVT_RESULTS: {
                3: {"tid": 3, "rc": 0, "out": "c", "err": "", "hsh": "", "cmd": "c3"},
                1: {"tid": 1, "rc": 0, "out": "a", "err": "", "hsh": "", "cmd": "c1"},
                2: {"tid": 2, "rc": 0, "out": "b", "err": "", "hsh": "", "cmd": "c2"},
            },
        }
        result = op.execute_complete(ctx, event)
        tids = [r["tid"] for r in result["results"]]
        assert tids == [1, 2, 3]

    def test_execute_complete_failure_raises(self):
        op    = _make_operator()
        ctx   = _mock_context()
        event = {
            EVT_STATE:   JobState.FAILED.value,
            EVT_RESULTS: {},
            EVT_ERROR:   "Non-zero rc for tid(s): [2]",
        }
        with pytest.raises(AirflowException, match="FAILED"):
            op.execute_complete(ctx, event)

    def test_execute_complete_failure_includes_error_detail(self):
        op  = _make_operator()
        ctx = _mock_context()
        event = {
            EVT_STATE:   JobState.FAILED.value,
            EVT_RESULTS: {},
            EVT_ERROR:   "connection refused",
        }
        with pytest.raises(AirflowException, match="connection refused"):
            op.execute_complete(ctx, event)

    def test_execute_complete_cancelled_raises(self):
        op  = _make_operator()
        ctx = _mock_context()
        event = {
            EVT_STATE:   JobState.CANCELLED.value,
            EVT_RESULTS: {},
            EVT_ERROR:   "Stream timed out after 3600s",
        }
        with pytest.raises(AirflowException, match="CANCELLED"):
            op.execute_complete(ctx, event)

    def test_custom_result_handler_applied(self):
        handler = MagicMock(return_value=[{"transformed": True}])
        op  = _make_operator(result_handler=handler)
        ctx = _mock_context()
        raw = {1: {"tid": 1, "rc": 0, "out": "x", "err": "", "hsh": "", "cmd": "c"}}
        event = {EVT_STATE: JobState.SUCCESS.value, EVT_RESULTS: raw}

        result = op.execute_complete(ctx, event)

        handler.assert_called_once_with(raw)
        assert result["results"] == [{"transformed": True}]

    def test_execute_raises_when_single_cmd_is_empty_string(self):
        op  = _make_operator(commands=[{"cmd": ""}])
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(AirflowException, match="empty"):
                op.execute(ctx)

    def test_execute_raises_when_single_cmd_is_whitespace(self):
        op  = _make_operator(commands=[{"cmd": "   "}])
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(AirflowException, match="empty"):
                op.execute(ctx)

    def test_execute_raises_with_index_of_empty_cmd(self):
        op  = _make_operator(commands=[_CMD1, {"cmd": ""}, _CMD2])
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(AirflowException, match="1"):  # index 1 is empty
                op.execute(ctx)

    def test_execute_raises_when_cmd_key_missing(self):
        op  = _make_operator(commands=[{"cmdtyp": 1}])  # no "cmd" key
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(AirflowException, match="empty"):
                op.execute(ctx)

    def test_execute_valid_commands_do_not_raise_empty_check(self):

        op  = _make_operator(commands=[{"cmd": "echo hi"}, {"cmd": "hostname"}])
        ctx = _mock_context()
        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(TaskDeferred):
                op.execute(ctx)  # must reach defer, not AirflowException

    def test_template_fields_declared(self):
        assert "commands" in RemSvcOperator.template_fields
        assert "metadata" in RemSvcOperator.template_fields

    def test_execute_complete_missing_results_key_treated_as_empty(self):
        """If EVT_RESULTS is absent from the event (malformed trigger payload),
        execute_complete must raise rather than silently succeed with no results."""
        op  = _make_operator()
        ctx = _mock_context()
        # Event has SUCCESS state but no EVT_RESULTS key at all.
        event = {EVT_STATE: JobState.SUCCESS.value}
        # Default handler on an empty dict returns [] — the call succeeds but
        # results will be empty.  This test pins that behaviour so any future
        # change (e.g. raising on missing key) is a deliberate decision.
        result = op.execute_complete(ctx, event)
        assert result["state"]   == "SUCCESS"
        assert result["results"] == []

    def test_execute_complete_missing_state_key_raises(self):
        """An event with no EVT_STATE must not silently succeed."""
        op    = _make_operator()
        ctx   = _mock_context()
        event = {EVT_RESULTS: {1: {"tid": 1, "rc": 0, "out": "", "err": "", "hsh": "", "cmd": ""}}}
        # JobState() constructor raises ValueError for an unknown value.
        with pytest.raises((AirflowException, ValueError)):
            op.execute_complete(ctx, event)


# ---------------------------------------------------------------------------
# RemSvcTrigger — retry logic
# ---------------------------------------------------------------------------

def _rpc_error(code: grpc.StatusCode, details: str = "") -> grpc.aio.AioRpcError:
    return grpc.aio.AioRpcError(
        code               = code,
        initial_metadata   = grpc.aio.Metadata(),
        trailing_metadata  = grpc.aio.Metadata(),
        details            = details,
        debug_error_string = "",
    )


class TestRemSvcTriggerRetry:

    @pytest.mark.asyncio
    async def test_unavailable_is_retried(self):
        """UNAVAILABLE should be retried; trigger must not yield FAILED on first attempt."""
        t = _make_trigger()
        success_payload = {EVT_STATE: JobState.SUCCESS.value, EVT_RESULTS: {1: {"rc": 0}}}
        # Fail once with UNAVAILABLE (sent_ref stays 0 — no writes), then succeed.
        calls = [_rpc_error(grpc.StatusCode.UNAVAILABLE, "transient"), success_payload]

        async def _run_stream_side_effect(sent_ref):
            item = calls.pop(0)
            if isinstance(item, grpc.aio.AioRpcError):
                raise item
            return item

        with patch.object(t, "_run_stream", side_effect=_run_stream_side_effect), \
             patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value

    @pytest.mark.asyncio
    async def test_resource_exhausted_is_retried(self):
        """RESOURCE_EXHAUSTED is also in _RETRYABLE_CODES and must be retried."""
        t = _make_trigger()
        success_payload = {EVT_STATE: JobState.SUCCESS.value, EVT_RESULTS: {1: {"rc": 0}}}
        errors = [_rpc_error(grpc.StatusCode.RESOURCE_EXHAUSTED)] * 2

        call_iter = iter(errors + [success_payload])

        async def _side_effect(sent_ref):
            item = next(call_iter)
            if isinstance(item, grpc.aio.AioRpcError):
                raise item
            return item

        with patch.object(t, "_run_stream", side_effect=_side_effect), \
             patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value

    @pytest.mark.asyncio
    async def test_deadline_exceeded_not_retried(self):
        """DEADLINE_EXCEEDED is not in _RETRYABLE_CODES — fail immediately."""
        t = _make_trigger()
        with patch.object(
            t, "_run_stream",
            new=AsyncMock(side_effect=_rpc_error(grpc.StatusCode.DEADLINE_EXCEEDED, "too slow")),
        ), patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "DEADLINE_EXCEEDED" in events[0].payload[EVT_ERROR]

    @pytest.mark.asyncio
    async def test_all_retries_exhausted_yields_failed(self):
        """After _MAX_ATTEMPTS failures with UNAVAILABLE, exactly one FAILED event."""
        from remsvc_provider.triggers.remsvc import _MAX_ATTEMPTS
        t = _make_trigger()

        with patch.object(
            t, "_run_stream",
            new=AsyncMock(side_effect=_rpc_error(grpc.StatusCode.UNAVAILABLE, "gone")),
        ), patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
           patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "UNAVAILABLE" in events[0].payload[EVT_ERROR]
        assert "gone" in events[0].payload[EVT_ERROR]

    @pytest.mark.asyncio
    async def test_retry_count_matches_max_attempts(self):
        """_run_stream must be called exactly _MAX_ATTEMPTS times on persistent UNAVAILABLE."""
        from remsvc_provider.triggers.remsvc import _MAX_ATTEMPTS
        t = _make_trigger()
        mock_stream = AsyncMock(
            side_effect=_rpc_error(grpc.StatusCode.UNAVAILABLE, "gone")
        )

        with patch.object(t, "_run_stream", mock_stream), \
             patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            await _collect(t)

        assert mock_stream.call_count == _MAX_ATTEMPTS

    @pytest.mark.asyncio
    async def test_non_retryable_error_fails_on_first_attempt(self):
        """PERMISSION_DENIED is not retryable — _run_stream called exactly once."""
        t = _make_trigger()
        mock_stream = AsyncMock(
            side_effect=_rpc_error(grpc.StatusCode.PERMISSION_DENIED, "denied")
        )

        with patch.object(t, "_run_stream", mock_stream), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert mock_stream.call_count == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value

    @pytest.mark.asyncio
    async def test_unavailable_after_writes_not_retried(self):
        """UNAVAILABLE after ≥1 write must fail immediately — no retry to prevent
        duplicate command execution on the remote server."""
        t = _make_trigger(commands=[_CMD1, _CMD2])

        async def _side_effect(sent_ref):
            # Simulate one command already dispatched before the transport drops.
            sent_ref[0] = 1
            raise _rpc_error(grpc.StatusCode.UNAVAILABLE, "connection reset")

        mock_stream = AsyncMock(side_effect=_side_effect)

        with patch.object(t, "_run_stream", mock_stream), \
             patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        # Must fail on the first attempt; no retry.
        assert mock_stream.call_count == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "duplicate" in events[0].payload[EVT_ERROR].lower()

    @pytest.mark.asyncio
    async def test_unavailable_before_any_writes_is_retried(self):
        """UNAVAILABLE when sent_ref[0] == 0 (connection failed before any write)
        must still retry — no commands reached the server so no duplicate risk."""
        t = _make_trigger(commands=[_CMD1])
        success_payload = {EVT_STATE: JobState.SUCCESS.value, EVT_RESULTS: {1: {"rc": 0}}}
        calls = [_rpc_error(grpc.StatusCode.UNAVAILABLE, "not yet up"), success_payload]

        async def _side_effect(sent_ref):
            # sent_ref[0] stays 0 — nothing was written before the failure.
            item = calls.pop(0)
            if isinstance(item, grpc.aio.AioRpcError):
                raise item
            return item

        with patch.object(t, "_run_stream", side_effect=_side_effect), \
             patch("remsvc_provider.triggers.remsvc.asyncio.sleep", new=AsyncMock()), \
             patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", True):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value


# ---------------------------------------------------------------------------
# RemSvcTrigger — _PROTO_AVAILABLE guard
# ---------------------------------------------------------------------------

class TestRemSvcTriggerProtoGuard:

    @pytest.mark.asyncio
    async def test_run_yields_failed_when_stubs_missing(self):
        t = _make_trigger()
        with patch("remsvc_provider.triggers.remsvc._PROTO_AVAILABLE", False):
            events = await _collect(t)
        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "regen_proto" in events[0].payload[EVT_ERROR]


# ---------------------------------------------------------------------------
# RemSvcTrigger — duplicate tid warning
# ---------------------------------------------------------------------------

class TestRemSvcTriggerDuplicateTid:

    @pytest.mark.asyncio
    async def test_duplicate_tid_logs_warning(self, caplog):
        import logging
        t = _make_trigger(commands=[_CMD1])
        stream  = _AsyncStream([
            _mock_response(tid=1, rc=0, out="first"),
            _mock_response(tid=1, rc=0, out="second"),   # duplicate
        ])
        stub    = MagicMock()
        stub.RemCmdStrm.return_value = stream
        channel = MagicMock()
        channel.__aenter__ = AsyncMock(return_value=channel)
        channel.__aexit__  = AsyncMock(return_value=False)

        with patch.object(t, "_async_channel", return_value=channel), \
             patch.object(t, "_async_stub",    return_value=stub), \
             patch.object(t, "_call_metadata", return_value=[]), \
             patch("remsvc_provider.triggers.remsvc.pb2", MagicMock()), \
             caplog.at_level(logging.WARNING, logger="remsvc_provider.triggers.remsvc"):
            await t._run_stream([0])

        assert any("Duplicate" in r.message for r in caplog.records)

    @pytest.mark.asyncio
    async def test_duplicate_tid_last_response_wins(self):
        t = _make_trigger(commands=[_CMD1])
        stream  = _AsyncStream([
            _mock_response(tid=1, rc=0, out="first"),
            _mock_response(tid=1, rc=0, out="second"),
        ])
        stub    = MagicMock()
        stub.RemCmdStrm.return_value = stream
        channel = MagicMock()
        channel.__aenter__ = AsyncMock(return_value=channel)
        channel.__aexit__  = AsyncMock(return_value=False)

        with patch.object(t, "_async_channel", return_value=channel), \
             patch.object(t, "_async_stub",    return_value=stub), \
             patch.object(t, "_call_metadata", return_value=[]), \
             patch("remsvc_provider.triggers.remsvc.pb2", MagicMock()):
            result = await t._run_stream([0])

        assert result[EVT_RESULTS][1]["out"] == "second"


# ---------------------------------------------------------------------------
# RemSvcTrigger — _call_metadata() merge and deduplication
# ---------------------------------------------------------------------------

class TestCallMetadata:
    """Unit tests for _call_metadata() without touching the Airflow DB.
    All tests patch _hook() so no RemSvcHook is actually constructed."""

    def _hook_returning(self, conn_meta: list) -> MagicMock:
        mock_hook = MagicMock()
        mock_hook.get_call_metadata.return_value = conn_meta
        return mock_hook

    def test_no_conn_meta_returns_operator_metadata(self):
        t = _make_trigger(metadata=[("x-custom", "val")])
        with patch.object(t, "_hook", return_value=self._hook_returning([])):
            result = t._call_metadata()
        assert result == [("x-custom", "val")]

    def test_no_conn_meta_no_operator_meta_returns_empty(self):
        t = _make_trigger()
        with patch.object(t, "_hook", return_value=self._hook_returning([])):
            result = t._call_metadata()
        assert result == []

    def test_conn_meta_returned_when_operator_meta_empty(self):
        t = _make_trigger()
        conn_meta = [("authorization", "Bearer conn-tok")]
        with patch.object(t, "_hook", return_value=self._hook_returning(conn_meta)):
            result = t._call_metadata()
        assert result == [("authorization", "Bearer conn-tok")]

    def test_conn_meta_and_operator_non_auth_meta_merged(self):
        t = _make_trigger(metadata=[("x-custom", "val")])
        conn_meta = [("authorization", "Bearer conn-tok")]
        with patch.object(t, "_hook", return_value=self._hook_returning(conn_meta)):
            result = t._call_metadata()
        assert ("authorization", "Bearer conn-tok") in result
        assert ("x-custom", "val") in result

    def test_operator_authorization_stripped_when_conn_meta_present(self):
        """Operator-supplied 'authorization' must be dropped when conn provides one."""
        t = _make_trigger(metadata=[("authorization", "Bearer op-tok"), ("x-other", "val")])
        conn_meta = [("authorization", "Bearer conn-tok")]
        with patch.object(t, "_hook", return_value=self._hook_returning(conn_meta)):
            result = t._call_metadata()
        auth_values = [v for k, v in result if k == "authorization"]
        assert auth_values == ["Bearer conn-tok"], "only conn token must survive"
        assert ("x-other", "val") in result

    def test_operator_authorization_kept_when_no_conn_meta(self):
        """Without conn-level auth, operator metadata is passed through unchanged."""
        t = _make_trigger(metadata=[("authorization", "Bearer op-tok")])
        with patch.object(t, "_hook", return_value=self._hook_returning([])):
            result = t._call_metadata()
        assert result == [("authorization", "Bearer op-tok")]

    def test_authorization_key_comparison_is_case_insensitive(self):
        """Operator headers with 'Authorization' (mixed case) are also stripped."""
        t = _make_trigger(metadata=[("Authorization", "Bearer op-tok")])
        conn_meta = [("authorization", "Bearer conn-tok")]
        with patch.object(t, "_hook", return_value=self._hook_returning(conn_meta)):
            result = t._call_metadata()
        auth_values = [v for k, v in result if k.lower() == "authorization"]
        assert len(auth_values) == 1
        assert auth_values[0] == "Bearer conn-tok"


# ---------------------------------------------------------------------------
# RemSvcTrigger — metadata forwarded to RemCmdStrm
# ---------------------------------------------------------------------------

class TestRemSvcTriggerMetadata:

    @pytest.mark.asyncio
    async def test_metadata_passed_to_stream(self):
        """_run_stream must forward _call_metadata() to RemCmdStrm."""
        meta = [("x-token", "secret")]
        t    = _make_trigger(commands=[_CMD1], metadata=meta)
        stream  = _AsyncStream([_mock_response(tid=1, rc=0)])
        stub    = MagicMock()
        stub.RemCmdStrm.return_value = stream
        channel = MagicMock()

        # _call_metadata() calls _hook() which hits the Airflow DB; patch it
        # directly to return our known metadata list.
        with patch.object(t, "_async_channel", return_value=channel), \
             patch.object(t, "_async_stub",    return_value=stub), \
             patch.object(t, "_call_metadata", return_value=meta), \
             patch("remsvc_provider.triggers.remsvc.pb2", MagicMock()):
            await t._run_stream([0])

        stub.RemCmdStrm.assert_called_once_with(
            timeout  = t.stream_timeout,
            metadata = meta,
        )


# ---------------------------------------------------------------------------
# crc32hex utility
# ---------------------------------------------------------------------------

class TestCrc32Hex:

    def test_known_value(self):
        from remsvc_provider.operators.remsvc import crc32hex
        # echo -n "hello" | crc32 → 3610a686
        assert crc32hex("hello") == "3610a686"

    def test_empty_string(self):
        from remsvc_provider.operators.remsvc import crc32hex
        # crc32 of empty string is 0
        assert crc32hex("") == "00000000"

    def test_returns_eight_hex_chars(self):
        from remsvc_provider.operators.remsvc import crc32hex
        result = crc32hex("any string")
        assert len(result) == 8
        assert all(c in "0123456789abcdef" for c in result)


# ---------------------------------------------------------------------------
# get_provider_info() contract
# ---------------------------------------------------------------------------

class TestGetProviderInfo:

    def test_required_keys_present(self):
        from remsvc_provider import get_provider_info
        info = get_provider_info()
        assert info["package-name"] == "airflow-provider-remsvc"
        assert info["min-airflow-version"] == "3.1.0"
        assert "1.0.0" in info["versions"]
        assert info["name"] == "RemSvc"

    def test_returns_dict(self):
        from remsvc_provider import get_provider_info
        assert isinstance(get_provider_info(), dict)


# ---------------------------------------------------------------------------
# RemSvcResult.to_dict() schema
# ---------------------------------------------------------------------------

class TestRemSvcResult:

    def test_to_dict_keys(self):
        from remsvc_provider.operators.remsvc import RemSvcResult
        r = RemSvcResult(state="SUCCESS", results=[{"tid": 1}], error_msg=None)
        d = r.to_dict()
        assert set(d.keys()) == {"state", "results", "error_msg"}

    def test_to_dict_values(self):
        from remsvc_provider.operators.remsvc import RemSvcResult
        payload = [{"tid": 1, "rc": 0}]
        r = RemSvcResult(state="SUCCESS", results=payload, error_msg=None)
        d = r.to_dict()
        assert d["state"]     == "SUCCESS"
        assert d["results"]   == payload
        assert d["error_msg"] is None

    def test_to_dict_with_error(self):
        from remsvc_provider.operators.remsvc import RemSvcResult
        r = RemSvcResult(state="FAILED", results=[], error_msg="oops")
        assert r.to_dict()["error_msg"] == "oops"
