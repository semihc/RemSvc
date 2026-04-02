"""
tests/test_remsvc_async.py
==========================
Async tests for RemSvcTrigger and the deferrable RemSvcOperator.

Run from RemSvc/prv/:
  pytest tests/test_remsvc_async.py -v --asyncio-mode=auto

Requires:
  pip install pytest pytest-asyncio apache-airflow>=2.2.0
"""

from __future__ import annotations

from unittest.mock import AsyncMock, MagicMock, patch

import grpc
import pytest

from airflow.exceptions import AirflowException
from airflow.triggers.base import TriggerEvent

from remsvc_provider.operators.remsvc import JobState, RemSvcOperator
from remsvc_provider.triggers.remsvc import (
    EVT_ERROR, EVT_JOB_ID, EVT_STATE, RemSvcTrigger,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_trigger(**kwargs) -> RemSvcTrigger:
    defaults = dict(
        job_id        = "job-test-001",
        grpc_conn_id  = "remsvc_test",
        poll_interval = 0.0,
        poll_timeout  = 60.0,
    )
    defaults.update(kwargs)
    return RemSvcTrigger(**defaults)


def _make_operator(**kwargs) -> RemSvcOperator:
    defaults = dict(
        task_id      = "test_op",
        grpc_conn_id = "remsvc_test",
        job_payload  = {"script": "test.py"},
    )
    defaults.update(kwargs)
    return RemSvcOperator(**defaults)


def _mock_context() -> dict:
    return {"task_instance": MagicMock()}


def _mock_channel():
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
# RemSvcTrigger tests
# ---------------------------------------------------------------------------

class TestRemSvcTrigger:

    def test_serialize_roundtrip(self):
        t = _make_trigger(poll_interval=5.0, poll_timeout=600.0)
        classpath, kwargs = t.serialize()

        assert classpath == "remsvc_provider.triggers.remsvc.RemSvcTrigger"
        rebuilt = RemSvcTrigger(**kwargs)
        assert rebuilt.job_id        == t.job_id
        assert rebuilt.poll_interval == t.poll_interval
        assert rebuilt.poll_timeout  == t.poll_timeout

    @pytest.mark.asyncio
    async def test_run_yields_success_event(self):
        t = _make_trigger()
        with patch.object(t, "_async_channel", return_value=_mock_channel()), \
             patch.object(t, "_get_status", new=AsyncMock(return_value=JobState.SUCCESS)):
            events = await _collect(t)

        assert len(events) == 1
        assert events[0].payload[EVT_STATE]  == JobState.SUCCESS.value
        assert events[0].payload[EVT_JOB_ID] == "job-test-001"
        assert EVT_ERROR not in events[0].payload

    @pytest.mark.asyncio
    async def test_run_waits_through_running_then_succeeds(self):
        states = [JobState.PENDING, JobState.RUNNING, JobState.SUCCESS]
        t = _make_trigger()
        with patch.object(t, "_async_channel", return_value=_mock_channel()), \
             patch.object(t, "_get_status", new=AsyncMock(side_effect=states)):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.SUCCESS.value

    @pytest.mark.asyncio
    async def test_run_yields_failed_event(self):
        t = _make_trigger()
        with patch.object(t, "_async_channel", return_value=_mock_channel()), \
             patch.object(t, "_get_status", new=AsyncMock(return_value=JobState.FAILED)):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.FAILED.value

    @pytest.mark.asyncio
    async def test_run_times_out(self):
        t = _make_trigger(poll_timeout=0.0)
        with patch.object(t, "_async_channel", return_value=_mock_channel()), \
             patch.object(t, "_get_status", new=AsyncMock(return_value=JobState.RUNNING)):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.CANCELLED.value
        assert "timed out" in events[0].payload[EVT_ERROR].lower()

    @pytest.mark.asyncio
    async def test_run_catches_grpc_error(self):
        t = _make_trigger()
        rpc_error = grpc.aio.AioRpcError(
            code=grpc.StatusCode.UNAVAILABLE,
            initial_metadata=grpc.aio.Metadata(),
            trailing_metadata=grpc.aio.Metadata(),
            details="connection refused",
            debug_error_string="",
        )
        with patch.object(t, "_async_channel", return_value=_mock_channel()), \
             patch.object(t, "_get_status", new=AsyncMock(side_effect=rpc_error)):
            events = await _collect(t)

        assert events[0].payload[EVT_STATE] == JobState.FAILED.value
        assert EVT_ERROR in events[0].payload

    @pytest.mark.asyncio
    async def test_run_yields_exactly_one_event(self):
        for final_state in (JobState.SUCCESS, JobState.FAILED, JobState.CANCELLED):
            t = _make_trigger()
            with patch.object(t, "_async_channel", return_value=_mock_channel()), \
                 patch.object(t, "_get_status", new=AsyncMock(return_value=final_state)):
                events = await _collect(t)
            assert len(events) == 1, \
                f"Expected 1 event for state {final_state}, got {len(events)}"


# ---------------------------------------------------------------------------
# RemSvcOperator (deferrable) tests
# ---------------------------------------------------------------------------

class TestDeferrableRemSvcOperator:

    def test_execute_defers(self):
        from airflow.exceptions import TaskDeferred
        op  = _make_operator()
        ctx = _mock_context()

        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True), \
             patch.object(op, "_submit_job", return_value="job-defer-test"):
            with pytest.raises(TaskDeferred) as exc_info:
                op.execute(ctx)

        deferred = exc_info.value
        assert deferred.trigger.__class__.__name__ == "RemSvcTrigger"
        assert deferred.trigger.job_id == "job-defer-test"
        assert deferred.method_name    == "execute_complete"

    def test_execute_pushes_job_id_xcom_before_defer(self):
        from airflow.exceptions import TaskDeferred
        op  = _make_operator()
        ctx = _mock_context()

        with patch("remsvc_provider.operators.remsvc._PROTO_AVAILABLE", True), \
             patch.object(op, "_submit_job", return_value="job-xcom-test"):
            with pytest.raises(TaskDeferred):
                op.execute(ctx)

        ctx["task_instance"].xcom_push.assert_called_once_with(
            key="job_id", value="job-xcom-test"
        )

    def test_execute_complete_success(self):
        op    = _make_operator()
        ctx   = _mock_context()
        event = {EVT_JOB_ID: "job-ok", EVT_STATE: JobState.SUCCESS.value}

        with patch.object(op, "_fetch_result", return_value={"out": "s3://result"}):
            result = op.execute_complete(ctx, event)

        assert result["state"]  == "SUCCESS"
        assert result["job_id"] == "job-ok"

    def test_execute_complete_failure_raises(self):
        op    = _make_operator()
        ctx   = _mock_context()
        event = {EVT_JOB_ID: "job-bad", EVT_STATE: JobState.FAILED.value}

        with pytest.raises(AirflowException, match="FAILED"):
            op.execute_complete(ctx, event)

    def test_execute_complete_timeout_raises(self):
        op    = _make_operator()
        ctx   = _mock_context()
        event = {
            EVT_JOB_ID: "job-timeout",
            EVT_STATE:  JobState.CANCELLED.value,
            EVT_ERROR:  "Trigger timed out after 3600s",
        }
        with pytest.raises(AirflowException, match="CANCELLED"):
            op.execute_complete(ctx, event)

    def test_execute_complete_includes_error_in_message(self):
        op    = _make_operator()
        ctx   = _mock_context()
        event = {
            EVT_JOB_ID: "job-err",
            EVT_STATE:  JobState.FAILED.value,
            EVT_ERROR:  "gRPC error UNAVAILABLE: connection refused",
        }
        with pytest.raises(AirflowException, match="connection refused"):
            op.execute_complete(ctx, event)

    def test_custom_result_handler_applied(self):
        handler = MagicMock(return_value={"transformed": True})
        op  = _make_operator(result_handler=handler)
        ctx = _mock_context()
        event = {EVT_JOB_ID: "job-h", EVT_STATE: JobState.SUCCESS.value}
        raw = object()

        with patch.object(op, "_fetch_result", return_value=raw):
            result = op.execute_complete(ctx, event)

        handler.assert_called_once_with(raw)
        assert result["output"] == {"transformed": True}

    def test_template_fields_declared(self):
        assert "job_payload" in RemSvcOperator.template_fields
        assert "metadata"    in RemSvcOperator.template_fields
