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

from remsvc_provider.hooks.remsvc import DEFAULT_PORT, RemSvcHook


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
