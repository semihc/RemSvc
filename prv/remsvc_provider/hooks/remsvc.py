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
                "bearer_token"  : str    — bearer token sent as
                                          "Authorization: Bearer <token>"
                                          on every gRPC call; must match an
                                          entry in the server's [auth] config

Example Airflow connection (environment variable form):
  # Insecure, no auth:
  AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":false}'

  # TLS + bearer token:
  AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":true,"ca_cert_path":"/etc/ssl/ca.pem","bearer_token":"secret-prod-token"}'
"""

from __future__ import annotations

import hashlib
import logging
from typing import Any

import grpc
import grpc.aio

from airflow.exceptions import AirflowException
from airflow.hooks.base import BaseHook
from airflow.models import Connection

log = logging.getLogger(__name__)

DEFAULT_PORT = 50051


# ---------------------------------------------------------------------------
# Async channel pool
# ---------------------------------------------------------------------------
# The Airflow triggerer runs many RemSvcTrigger coroutines inside a single
# asyncio event loop.  Without pooling, each coroutine would open and close
# its own grpc.aio.Channel, paying TCP + TLS handshake overhead on every
# deferred task.
#
# This module-level pool caches channels keyed by (target, ssl, ca_digest).
# grpc.aio.Channel is designed to be shared — it manages its own connection
# pool internally and is safe to use concurrently from many coroutines in the
# same event loop.
#
# The pool is never explicitly flushed during normal operation; channels
# survive for the lifetime of the triggerer process.  If a channel enters a
# terminal error state (SHUTDOWN), the next call to _get_pooled_channel()
# detects this and replaces it transparently.
#
# Thread safety: the triggerer process uses a single OS thread with one asyncio
# event loop, so plain dict operations are safe without locking.

_async_channel_pool: dict[tuple[Any, ...], grpc.aio.Channel] = {}

_CHANNEL_SHUTDOWN_STATES = frozenset({
    grpc.ChannelConnectivity.SHUTDOWN,
})


def _channel_pool_key(target: str, use_ssl: bool, ca_cert: bytes | None) -> tuple[Any, ...]:
    """Derive a hashable pool key from channel parameters."""
    ca_digest = hashlib.sha256(ca_cert).hexdigest() if ca_cert else ""
    return (target, use_ssl, ca_digest)


def _get_pooled_channel(
    target: str,
    use_ssl: bool,
    ca_cert: bytes | None,
) -> grpc.aio.Channel:
    """Return a cached grpc.aio.Channel, creating one if necessary.

    If the cached channel has reached SHUTDOWN state (the server closed it or
    the process recycled it), it is evicted and a fresh channel is created.
    """
    key = _channel_pool_key(target, use_ssl, ca_cert)
    existing = _async_channel_pool.get(key)

    if existing is not None:
        try:
            state = existing.get_state(try_to_connect=False)
            if state not in _CHANNEL_SHUTDOWN_STATES:
                log.debug(
                    "RemSvcHook channel pool: reusing channel to %s (state=%s)",
                    target, state,
                )
                return existing
        except Exception:  # noqa: BLE001
            pass  # defensive: if get_state() fails, fall through and replace
        log.debug("RemSvcHook channel pool: evicting stale channel to %s", target)
        _async_channel_pool.pop(key, None)

    # Create a fresh channel and cache it.
    if use_ssl:
        credentials = grpc.ssl_channel_credentials(root_certificates=ca_cert)
        log.debug("RemSvcHook channel pool: creating secure channel to %s", target)
        channel = grpc.aio.secure_channel(target, credentials)
    else:
        log.debug("RemSvcHook channel pool: creating insecure channel to %s", target)
        channel = grpc.aio.insecure_channel(target)

    _async_channel_pool[key] = channel
    return channel


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
        self._ca_cert_cache: bytes | None | bool = False  # False = not yet loaded

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

    def _bearer_token(self) -> str | None:
        """Return the configured bearer token, or None if not set."""
        conn = self.get_conn()
        return (conn.extra_dejson or {}).get("bearer_token") or None

    def get_call_metadata(self) -> list[tuple[str, str]]:
        """Return gRPC call metadata to attach to every RPC.

        Currently injects ``Authorization: Bearer <token>`` when a
        ``bearer_token`` is configured in the Airflow connection extra.
        Returns an empty list when no auth is configured.

        Usage in trigger::

            metadata = hook.get_call_metadata()
            stream = stub.RemCmdStrm(metadata=metadata, ...)
        """
        token = self._bearer_token()
        if token:
            return [("authorization", f"Bearer {token}")]
        return []

    def _ca_cert(self) -> bytes | None:
        """Return CA certificate bytes if a path is configured, else None.

        Result is cached after the first successful read so that concurrent
        triggers sharing a hook instance do not each hit the file system.
        """
        if self._ca_cert_cache is not False:
            return self._ca_cert_cache  # type: ignore[return-value]
        conn    = self.get_conn()
        ca_path = (conn.extra_dejson or {}).get("ca_cert_path")
        if ca_path:
            try:
                with open(ca_path, "rb") as fh:
                    data = fh.read()
            except OSError as exc:
                raise AirflowException(
                    f"RemSvcHook: cannot read CA certificate for connection "
                    f"'{self.remsvc_conn_id}' from '{ca_path}': {exc}"
                ) from exc
            self._ca_cert_cache = data
            return data
        self._ca_cert_cache = None
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
        """Return an async gRPC channel (``grpc.aio``) from the module-level pool.

        Channels are shared across all ``RemSvcTrigger`` coroutines that target
        the same host/port/TLS configuration within the same triggerer process.
        This avoids re-establishing the TCP (+ TLS) handshake for every deferred
        task.

        The returned channel is valid for the lifetime of the triggerer process
        and must NOT be closed by the caller.  Use it directly (not as a context
        manager) so the pooled instance is not inadvertently shut down::

            channel = hook.get_async_channel()
            stub = RemSvcStub(channel)
            stream = stub.RemCmdStrm(...)
        """
        target  = self._target()
        use_ssl = self._use_ssl()
        ca_cert = self._ca_cert() if use_ssl else None
        return _get_pooled_channel(target, use_ssl, ca_cert)
