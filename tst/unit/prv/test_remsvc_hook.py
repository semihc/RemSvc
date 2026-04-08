"""
tst/unit/prv/test_remsvc_hook.py
=================================
Unit tests for RemSvcHook.

Run from RemSvc/prv/:
  pytest ../tst/unit/prv -v

Requires:
  pip install pytest apache-airflow>=3.1.0
"""

from __future__ import annotations

import os
from unittest.mock import MagicMock, mock_open, patch

import pytest

from airflow.exceptions import AirflowException

import grpc

from remsvc_provider.hooks.remsvc import (
    DEFAULT_PORT, RemSvcHook,
    _async_channel_pool, _channel_pool_key, _get_pooled_channel,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_conn(
    host: str | None = "remotehost",
    port: int | None = 50051,
    extra_dejson: dict | None = None,
) -> MagicMock:
    conn = MagicMock()
    conn.host        = host
    conn.port        = port
    conn.extra_dejson = extra_dejson or {}
    return conn


def _make_hook(conn: MagicMock) -> RemSvcHook:
    hook = RemSvcHook(remsvc_conn_id="remsvc_test")
    hook._conn = conn
    return hook


# ---------------------------------------------------------------------------
# _target()
# ---------------------------------------------------------------------------

class TestTarget:

    def test_returns_host_and_port(self):
        hook = _make_hook(_make_conn(host="myhost", port=9090))
        assert hook._target() == "myhost:9090"

    def test_uses_default_port_when_none(self):
        hook = _make_hook(_make_conn(host="myhost", port=None))
        assert hook._target() == f"myhost:{DEFAULT_PORT}"

    def test_raises_when_host_is_none(self):
        hook = _make_hook(_make_conn(host=None))
        with pytest.raises(AirflowException, match="no host configured"):
            hook._target()

    def test_raises_when_host_is_empty_string(self):
        hook = _make_hook(_make_conn(host=""))
        with pytest.raises(AirflowException, match="no host configured"):
            hook._target()


# ---------------------------------------------------------------------------
# _use_ssl()
# ---------------------------------------------------------------------------

class TestUseSsl:

    def test_false_by_default(self):
        hook = _make_hook(_make_conn())
        assert hook._use_ssl() is False

    def test_true_when_set(self):
        hook = _make_hook(_make_conn(extra_dejson={"use_ssl": True}))
        assert hook._use_ssl() is True

    def test_false_when_explicitly_false(self):
        hook = _make_hook(_make_conn(extra_dejson={"use_ssl": False}))
        assert hook._use_ssl() is False


# ---------------------------------------------------------------------------
# _ca_cert()
# ---------------------------------------------------------------------------

class TestCaCert:

    def test_returns_none_when_not_configured(self):
        hook = _make_hook(_make_conn())
        assert hook._ca_cert() is None

    def test_reads_file_when_path_configured(self, tmp_path):
        ca_file = tmp_path / "ca.pem"
        ca_file.write_bytes(b"CERT_DATA")
        hook = _make_hook(_make_conn(extra_dejson={"ca_cert_path": str(ca_file)}))
        assert hook._ca_cert() == b"CERT_DATA"

    def test_raises_airflow_exception_when_file_missing(self):
        hook = _make_hook(_make_conn(extra_dejson={"ca_cert_path": "/nonexistent/ca.pem"}))
        with pytest.raises(AirflowException, match="ca.pem"):
            hook._ca_cert()

    def test_raises_airflow_exception_includes_conn_id(self):
        hook = _make_hook(_make_conn(extra_dejson={"ca_cert_path": "/nonexistent/ca.pem"}))
        with pytest.raises(AirflowException, match="remsvc_test"):
            hook._ca_cert()

    def test_cert_bytes_cached_after_first_read(self, tmp_path):
        """Second call must not re-open the file."""
        ca_file = tmp_path / "ca.pem"
        ca_file.write_bytes(b"CERT_DATA")
        hook = _make_hook(_make_conn(extra_dejson={"ca_cert_path": str(ca_file)}))
        first  = hook._ca_cert()
        ca_file.unlink()           # delete the file — a second read would raise
        second = hook._ca_cert()   # must return cached value, not raise
        assert first == second == b"CERT_DATA"

    def test_none_cached_when_no_path_configured(self):
        hook = _make_hook(_make_conn())
        assert hook._ca_cert() is None
        assert hook._ca_cert() is None  # second call also returns None (cached)


# ---------------------------------------------------------------------------
# get_channel() — sync, insecure vs TLS
# ---------------------------------------------------------------------------

class TestGetChannel:

    def test_insecure_channel_when_no_tls(self):
        hook = _make_hook(_make_conn())
        with patch("grpc.insecure_channel") as mock_insecure, \
             patch("grpc.secure_channel") as mock_secure:
            hook.get_channel()
            mock_insecure.assert_called_once_with("remotehost:50051")
            mock_secure.assert_not_called()

    def test_secure_channel_when_tls_enabled(self):
        hook = _make_hook(_make_conn(extra_dejson={"use_ssl": True}))
        with patch("grpc.ssl_channel_credentials", return_value=MagicMock()) as mock_creds, \
             patch("grpc.secure_channel") as mock_secure, \
             patch("grpc.insecure_channel") as mock_insecure:
            hook.get_channel()
            mock_creds.assert_called_once_with(root_certificates=None)
            mock_secure.assert_called_once()
            mock_insecure.assert_not_called()

    def test_secure_channel_passes_ca_cert(self, tmp_path):
        ca_file = tmp_path / "ca.pem"
        ca_file.write_bytes(b"CERT")
        hook = _make_hook(_make_conn(
            extra_dejson={"use_ssl": True, "ca_cert_path": str(ca_file)}
        ))
        with patch("grpc.ssl_channel_credentials", return_value=MagicMock()) as mock_creds, \
             patch("grpc.secure_channel"):
            hook.get_channel()
            mock_creds.assert_called_once_with(root_certificates=b"CERT")


# ---------------------------------------------------------------------------
# get_async_channel() — async, insecure vs TLS
# ---------------------------------------------------------------------------

class TestGetAsyncChannel:

    def test_insecure_channel_when_no_tls(self):
        hook = _make_hook(_make_conn())
        with patch("grpc.aio.insecure_channel") as mock_insecure, \
             patch("grpc.aio.secure_channel") as mock_secure:
            hook.get_async_channel()
            mock_insecure.assert_called_once_with("remotehost:50051")
            mock_secure.assert_not_called()

    def test_secure_channel_when_tls_enabled(self):
        hook = _make_hook(_make_conn(extra_dejson={"use_ssl": True}))
        with patch("grpc.ssl_channel_credentials", return_value=MagicMock()) as mock_creds, \
             patch("grpc.aio.secure_channel") as mock_secure, \
             patch("grpc.aio.insecure_channel") as mock_insecure:
            hook.get_async_channel()
            mock_creds.assert_called_once_with(root_certificates=None)
            mock_secure.assert_called_once()
            mock_insecure.assert_not_called()

    def test_secure_channel_passes_ca_cert(self, tmp_path):
        ca_file = tmp_path / "ca.pem"
        ca_file.write_bytes(b"CERT")
        hook = _make_hook(_make_conn(
            extra_dejson={"use_ssl": True, "ca_cert_path": str(ca_file)}
        ))
        with patch("grpc.ssl_channel_credentials", return_value=MagicMock()) as mock_creds, \
             patch("grpc.aio.secure_channel"):
            hook.get_async_channel()
            mock_creds.assert_called_once_with(root_certificates=b"CERT")


# ---------------------------------------------------------------------------
# conn_type / hook metadata
# ---------------------------------------------------------------------------

class TestHookMetadata:

    def test_conn_type(self):
        assert RemSvcHook.conn_type == "remsvc"

    def test_conn_name_attr(self):
        assert RemSvcHook.conn_name_attr == "remsvc_conn_id"

    def test_hook_name(self):
        assert RemSvcHook.hook_name == "RemSvc gRPC"


# ---------------------------------------------------------------------------
# get_conn() — Airflow connection lookup failure
# ---------------------------------------------------------------------------

class TestGetConn:

    def test_raises_when_connection_not_found(self):
        """get_connection() raises AirflowNotFoundException for unknown conn_id.
        Verify the error propagates without being swallowed by the hook."""
        from airflow.exceptions import AirflowNotFoundException
        hook = RemSvcHook(remsvc_conn_id="nonexistent_conn")
        with patch.object(
            RemSvcHook,
            "get_connection",
            side_effect=AirflowNotFoundException("nonexistent_conn"),
        ):
            with pytest.raises(AirflowNotFoundException):
                hook.get_conn()

    def test_conn_is_cached_after_first_call(self):
        hook = RemSvcHook(remsvc_conn_id="remsvc_test")
        conn = _make_conn()
        with patch.object(RemSvcHook, "get_connection", return_value=conn) as mock_get:
            hook.get_conn()
            hook.get_conn()
        mock_get.assert_called_once()  # second call uses cached _conn


# ---------------------------------------------------------------------------
# get_call_metadata() — bearer token injection
# ---------------------------------------------------------------------------

class TestGetCallMetadata:

    def test_returns_bearer_header_when_token_configured(self):
        hook = _make_hook(_make_conn(extra_dejson={"bearer_token": "my-secret"}))
        assert hook.get_call_metadata() == [("authorization", "Bearer my-secret")]

    def test_returns_empty_list_when_no_token(self):
        hook = _make_hook(_make_conn())
        assert hook.get_call_metadata() == []

    def test_returns_empty_list_when_token_is_empty_string(self):
        hook = _make_hook(_make_conn(extra_dejson={"bearer_token": ""}))
        assert hook.get_call_metadata() == []

    def test_header_key_is_lowercase_authorization(self):
        hook = _make_hook(_make_conn(extra_dejson={"bearer_token": "tok123"}))
        key, _ = hook.get_call_metadata()[0]
        assert key == "authorization"

    def test_header_value_has_bearer_prefix(self):
        hook = _make_hook(_make_conn(extra_dejson={"bearer_token": "tok123"}))
        _, value = hook.get_call_metadata()[0]
        assert value == "Bearer tok123"

    def test_returns_exactly_one_entry(self):
        hook = _make_hook(_make_conn(extra_dejson={"bearer_token": "tok"}))
        assert len(hook.get_call_metadata()) == 1


# ---------------------------------------------------------------------------
# Async channel pool — _get_pooled_channel()
# ---------------------------------------------------------------------------

class TestAsyncChannelPool:
    """Tests for the module-level async channel pool in hooks/remsvc.py."""

    def setup_method(self):
        """Clear the pool before each test to avoid inter-test interference."""
        _async_channel_pool.clear()

    def test_same_params_returns_same_channel(self):
        ch = MagicMock()
        ch.get_state.return_value = grpc.ChannelConnectivity.READY
        with patch("grpc.aio.insecure_channel", return_value=ch) as mock_create:
            first  = _get_pooled_channel("host:50051", False, None)
            second = _get_pooled_channel("host:50051", False, None)
        assert first is second
        mock_create.assert_called_once()  # created only once

    def test_different_target_creates_new_channel(self):
        ch = MagicMock()
        ch.get_state.return_value = grpc.ChannelConnectivity.READY
        with patch("grpc.aio.insecure_channel", return_value=ch) as mock_create:
            _get_pooled_channel("host1:50051", False, None)
            _get_pooled_channel("host2:50051", False, None)
        assert mock_create.call_count == 2

    def test_shutdown_channel_is_evicted_and_replaced(self):
        stale = MagicMock()
        stale.get_state.return_value = grpc.ChannelConnectivity.SHUTDOWN
        fresh = MagicMock()
        fresh.get_state.return_value = grpc.ChannelConnectivity.READY

        with patch("grpc.aio.insecure_channel", side_effect=[stale, fresh]):
            first  = _get_pooled_channel("host:50051", False, None)
            # Manually put the stale channel into the pool as if it degraded.
            key = _channel_pool_key("host:50051", False, None)
            _async_channel_pool[key] = stale
            second = _get_pooled_channel("host:50051", False, None)

        assert first  is stale
        assert second is fresh

    def test_get_state_exception_evicts_and_replaces(self):
        broken = MagicMock()
        broken.get_state.side_effect = RuntimeError("channel gone")
        fresh = MagicMock()
        fresh.get_state.return_value = grpc.ChannelConnectivity.READY

        key = _channel_pool_key("host:50051", False, None)
        _async_channel_pool[key] = broken

        with patch("grpc.aio.insecure_channel", return_value=fresh):
            result = _get_pooled_channel("host:50051", False, None)

        assert result is fresh

    def test_ssl_and_non_ssl_use_separate_pool_entries(self):
        insecure_ch = MagicMock()
        insecure_ch.get_state.return_value = grpc.ChannelConnectivity.READY
        secure_ch = MagicMock()
        secure_ch.get_state.return_value = grpc.ChannelConnectivity.READY

        with patch("grpc.aio.insecure_channel", return_value=insecure_ch), \
             patch("grpc.ssl_channel_credentials", return_value=MagicMock()), \
             patch("grpc.aio.secure_channel", return_value=secure_ch):
            ch1 = _get_pooled_channel("host:50051", False, None)
            ch2 = _get_pooled_channel("host:50051", True,  None)

        assert ch1 is not ch2

    def test_different_ca_certs_use_separate_pool_entries(self):
        ch_a = MagicMock()
        ch_a.get_state.return_value = grpc.ChannelConnectivity.READY
        ch_b = MagicMock()
        ch_b.get_state.return_value = grpc.ChannelConnectivity.READY
        creds = MagicMock()

        with patch("grpc.ssl_channel_credentials", return_value=creds), \
             patch("grpc.aio.secure_channel", side_effect=[ch_a, ch_b]):
            r1 = _get_pooled_channel("host:50051", True, b"CERT_A")
            r2 = _get_pooled_channel("host:50051", True, b"CERT_B")

        assert r1 is not r2
