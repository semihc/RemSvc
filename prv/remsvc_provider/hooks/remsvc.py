"""
remsvc_provider/hooks/remsvc.py
================================
Airflow hook for the RemSvc gRPC service.

Wraps connection management for both synchronous (operator) and async
(trigger) gRPC channels.  Connection parameters are stored in the Airflow
connection with the following conventions:

  conn_type : "remsvc"
  host      : RemSvc server hostname or IP
  port      : RemSvc server port  (default 50051)
  extra     : JSON object with optional fields:
                "use_ssl"       : bool   — enable TLS (default false)
                "ca_cert_path"  : str    — path to CA certificate PEM file;
                                          if omitted with use_ssl=true, uses
                                          system default trust store

Example Airflow connection (environment variable form):
  AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":false}'
"""

from __future__ import annotations

import logging

import grpc
import grpc.aio

from airflow.exceptions import AirflowException
from airflow.hooks.base import BaseHook
from airflow.models import Connection

log = logging.getLogger(__name__)

DEFAULT_PORT = 50051


class RemSvcHook(BaseHook):
    """Hook that provides gRPC channels to the RemSvc server.

    Parameters
    ----------
    remsvc_conn_id:
        Airflow connection ID configured with the RemSvc host, port, and
        optional TLS settings in the ``extra`` JSON field.
    """

    conn_name_attr  = "remsvc_conn_id"
    conn_type       = "remsvc"
    hook_name       = "RemSvc gRPC"

    def __init__(self, remsvc_conn_id: str = "remsvc_default") -> None:
        super().__init__()
        self.remsvc_conn_id = remsvc_conn_id
        self._conn: Connection | None = None

    # ------------------------------------------------------------------
    # Connection helpers
    # ------------------------------------------------------------------

    def get_conn(self) -> Connection:
        """Return the raw Airflow Connection object (cached)."""
        if self._conn is None:
            self._conn = self.get_connection(self.remsvc_conn_id)
        return self._conn

    def _target(self) -> str:
        conn = self.get_conn()
        if not conn.host:
            raise AirflowException(
                f"RemSvcHook: connection '{self.remsvc_conn_id}' has no host configured."
            )
        port = conn.port or DEFAULT_PORT
        return f"{conn.host}:{port}"

    def _use_ssl(self) -> bool:
        conn = self.get_conn()
        return bool((conn.extra_dejson or {}).get("use_ssl", False))

    def _ca_cert(self) -> bytes | None:
        """Return CA certificate bytes if a path is configured, else None."""
        conn    = self.get_conn()
        ca_path = (conn.extra_dejson or {}).get("ca_cert_path")
        if ca_path:
            try:
                with open(ca_path, "rb") as fh:
                    return fh.read()
            except OSError as exc:
                raise AirflowException(
                    f"RemSvcHook: cannot read CA certificate for connection "
                    f"'{self.remsvc_conn_id}' from '{ca_path}': {exc}"
                ) from exc
        return None

    # ------------------------------------------------------------------
    # Synchronous channel  (for use in operator workers)
    # ------------------------------------------------------------------

    def get_channel(self) -> grpc.Channel:
        """Return a synchronous gRPC channel.

        Reserved for future use by operator-side unary RPCs (e.g. Ping).
        Currently all gRPC work runs in the triggerer via ``get_async_channel()``.

        The caller is responsible for closing the channel when done,
        or using it as a context manager (``with hook.get_channel() as ch``).
        """
        target = self._target()
        if self._use_ssl():
            credentials = grpc.ssl_channel_credentials(
                root_certificates=self._ca_cert()
            )
            log.debug("RemSvcHook: opening secure sync channel to %s", target)
            return grpc.secure_channel(target, credentials)
        log.debug("RemSvcHook: opening insecure sync channel to %s", target)
        return grpc.insecure_channel(target)

    # ------------------------------------------------------------------
    # Async channel  (for use in triggerer coroutines)
    # ------------------------------------------------------------------

    def get_async_channel(self) -> grpc.aio.Channel:
        """Return an async gRPC channel (``grpc.aio``).

        Must be used as an async context manager::

            async with hook.get_async_channel() as channel:
                stub = RemSvcStub(channel)
                ...
        """
        target = self._target()
        if self._use_ssl():
            credentials = grpc.ssl_channel_credentials(
                root_certificates=self._ca_cert()
            )
            log.debug("RemSvcHook: opening secure async channel to %s", target)
            return grpc.aio.secure_channel(target, credentials)
        log.debug("RemSvcHook: opening insecure async channel to %s", target)
        return grpc.aio.insecure_channel(target)
