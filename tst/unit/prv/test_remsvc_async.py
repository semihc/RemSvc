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

    def __init__(self, responses: List[Any]) -> None:
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
        with patch.object(t, "_run_stream", new=AsyncMock(return_value=payload)):
            events = await _collect(t)
        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value

    @pytest.mark.asyncio
    async def test_run_yields_exactly_one_event_on_failure(self):
        t = _make_trigger()
        payload = {EVT_STATE: JobState.FAILED.value, EVT_RESULTS: {}, EVT_ERROR: "oops"}
        with patch.object(t, "_run_stream", new=AsyncMock(return_value=payload)):
            events = await _collect(t)
        assert len(events) == 1
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value

    @pytest.mark.asyncio
    async def test_run_fires_cancelled_on_timeout(self):
        t = _make_trigger(stream_timeout=0.001)
        # _run_stream sleeps long enough to trigger the wait_for timeout
        async def _slow():
            import asyncio as _a
            await _a.sleep(10)
        with patch.object(t, "_run_stream", new=_slow):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE]  == JobState.CANCELLED.value
        assert "timed out" in events[0].payload[EVT_ERROR].lower()

    @pytest.mark.asyncio
    async def test_run_catches_grpc_error(self):
        t = _make_trigger()
        rpc_error = grpc.aio.AioRpcError(
            code                 = grpc.StatusCode.UNAVAILABLE,
            initial_metadata     = grpc.aio.Metadata(),
            trailing_metadata    = grpc.aio.Metadata(),
            details              = "connection refused",
            debug_error_string   = "",
        )
        with patch.object(t, "_run_stream", new=AsyncMock(side_effect=rpc_error)):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert EVT_ERROR in events[0].payload

    @pytest.mark.asyncio
    async def test_run_catches_generic_exception(self):
        t = _make_trigger()
        with patch.object(t, "_run_stream", new=AsyncMock(side_effect=RuntimeError("boom"))):
            events = await _collect(t)
        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert "boom" in events[0].payload[EVT_ERROR]


# ---------------------------------------------------------------------------
# RemSvcTrigger — _run_stream: response correlation and state logic
# ---------------------------------------------------------------------------

class TestRemSvcTriggerRunStream:

    def _setup(self, trigger: RemSvcTrigger, responses: list) -> tuple:
        """Wire a mock channel+stub+stream onto the trigger.
        Returns (channel_mock, stub_mock, stream_mock)."""
        stream  = _AsyncStream(responses)
        stub    = MagicMock()
        stub.RemCmdStrm.return_value = stream
        channel = _mock_channel_ctx(stub)
        return channel, stub, stream

    @pytest.mark.asyncio
    async def test_single_command_success(self):
        t = _make_trigger(commands=[_CMD1])
        ch, stub, _ = self._setup(t, [_mock_response(tid=1, rc=0, out="hello")])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream()

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
            result = await t._run_stream()

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
            result = await t._run_stream()

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
            result = await t._run_stream()

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
            result = await t._run_stream()

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
            await t._run_stream()

        # write() called once per command
        assert stream.write.call_count == 2
        sent_msgs = [call.args[0] for call in stream.write.call_args_list]
        assert sent_msgs[0].tid == 1
        assert sent_msgs[1].tid == 2

    @pytest.mark.asyncio
    async def test_done_writing_called(self):
        t = _make_trigger(commands=[_CMD1])
        ch, stub, stream = self._setup(t, [_mock_response(tid=1, rc=0)])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            await t._run_stream()
        stream.done_writing.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_result_includes_original_cmd_string(self):
        t = _make_trigger(commands=[{"cmd": "echo hello"}])
        ch, stub, _ = self._setup(t, [_mock_response(tid=1, rc=0)])
        with patch.object(t, "_async_channel", return_value=ch), \
             patch.object(t, "_async_stub",    return_value=stub):
            result = await t._run_stream()
        assert result[EVT_RESULTS][1]["cmd"] == "echo hello"


# ---------------------------------------------------------------------------
# RemSvcOperator (deferrable)
# ---------------------------------------------------------------------------

class TestDeferrableRemSvcOperator:

    def test_execute_defers(self):
        from airflow.exceptions import TaskDeferred
        op  = _make_operator()
        ctx = _mock_context()

        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True):
            with pytest.raises(TaskDeferred) as exc_info:
                op.execute(ctx)

        deferred = exc_info.value
        assert deferred.trigger.__class__.__name__ == "RemSvcTrigger"
        assert deferred.method_name                == "execute_complete"

    def test_execute_defers_with_correct_commands(self):
        from airflow.exceptions import TaskDeferred
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

    def test_template_fields_declared(self):
        assert "commands" in RemSvcOperator.template_fields
        assert "metadata" in RemSvcOperator.template_fields
