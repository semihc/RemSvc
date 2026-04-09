"""
dag_remsvc_e2e_linux.py
=======================
End-to-end test DAG for RemSvc on a **Linux** remote host.

Tests covered
-------------
1. Health check   — echo round-trip (native shell, cmdtyp=0)
2. Host identity  — hostname -f (FQDN)
3. OS information — uname -a
4. Working user   — whoami
5. Process list   — ps -eo pid,comm --no-headers | head -5
7. Validate       — downstream @task reads XCom, asserts rc=0 for all commands

Connection setup (run once before activating the DAG)
-----------------------------------------------------
Insecure / no auth (development only):

    export AIRFLOW_CONN_REMSVC_LINUX='remsvc://linux-host.example.com:50051?extra={}'

TLS + bearer token (production):

    export AIRFLOW_CONN_REMSVC_LINUX='remsvc://linux-host.example.com:50051?extra={"use_ssl":true,"ca_cert_path":"/etc/ssl/remsvc-ca.pem","bearer_token":"secret-prod-token"}'

Or via Airflow UI:
    Connection Type : remsvc
    Host            : linux-host.example.com
    Port            : 50051
    Extra (JSON)    : {"use_ssl": true, "ca_cert_path": "/etc/ssl/remsvc-ca.pem", "bearer_token": "secret-prod-token"}

Server-side INI (minimum allowlist to permit these commands):

    [allowlist]
    1=^echo\b
    2=^hostname\b
    3=^uname\b
    4=^whoami$
    5=^ps\b
"""

from __future__ import annotations

import json
import logging

from datetime import datetime

from airflow.sdk import dag, task, Param

from remsvc_provider.operators.remsvc import RemSvcOperator

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Commands — native shell (cmdtyp=0) unless noted
# ---------------------------------------------------------------------------
COMMANDS = [
    # 1. Echo round-trip — verifies the channel is alive and output is returned
    {
        "cmd":    "echo RemSvc E2E test: {{ ds }}",
        "cmdtyp": 0,
        "src":    "e2e-linux-dag",
    },
    # 2. Fully-qualified hostname — confirms which machine executed the command
    {
        "cmd":    "hostname -f",
        "cmdtyp": 0,
        "src":    "e2e-linux-dag",
    },
    # 3. OS / kernel info — verifies uname is accessible
    {
        "cmd":    "uname -a",
        "cmdtyp": 0,
        "src":    "e2e-linux-dag",
    },
    # 4. Effective user — verifies the OS user the server process runs as
    {
        "cmd":    "whoami",
        "cmdtyp": 0,
        "src":    "e2e-linux-dag",
    },
    # 5. Short process list — exercises a command producing tabular output
    {
        "cmd":    "ps -eo pid,comm --no-headers | head -5",
        "cmdtyp": 0,
        "src":    "e2e-linux-dag",
    },
]


@dag(
    dag_id="remsvc_e2e_linux",
    description="End-to-end test: RemSvc on a Linux remote host",
    schedule=None,           # manual trigger only — this is a test DAG
    start_date=datetime(2025, 1, 1),
    catchup=False,
    tags=["remsvc", "e2e", "linux"],
    params={
        "grpc_conn_id": Param(
            "remsvc_linux",
            type="string",
            description="Airflow connection ID for the RemSvc target host. "
                        "Override at trigger time to point at a different server.",
        ),
    },
)
def remsvc_e2e_linux():

    # -----------------------------------------------------------------------
    # Preflight: verify the connection exists before deferring to the triggerer.
    # Fails immediately with a clear message rather than an obscure gRPC error.
    # -----------------------------------------------------------------------
    @task
    def check_connection(params=None):
        from airflow.exceptions import AirflowNotFoundException
        try:
            from airflow.sdk.bases.hook import BaseHook
        except ImportError:
            from airflow.hooks.base import BaseHook
        conn_id = params["grpc_conn_id"]
        try:
            BaseHook.get_connection(conn_id)
        except AirflowNotFoundException:
            raise AirflowNotFoundException(
                f"Airflow connection '{conn_id}' not found. "
                f"Create it with: airflow connections add {conn_id} "
                f"--conn-type remsvc --conn-host <host> --conn-port 50051"
            )
        log.info("Connection '%s' found — proceeding.", conn_id)

    # -----------------------------------------------------------------------
    # Phase 1 + 2: submit commands → defer → triggerer executes → resume
    # The worker slot is released while the triggerer runs the gRPC stream.
    # grpc_conn_id is Jinja-templated so --conf '{"grpc_conn_id":"..."}' overrides it.
    # -----------------------------------------------------------------------
    run_commands = RemSvcOperator(
        task_id        = "run_commands",
        grpc_conn_id   = "{{ params.grpc_conn_id }}",
        commands       = COMMANDS,
        stream_timeout = 120,
        retries        = 1,
    )

    # -----------------------------------------------------------------------
    # Phase 3 (validation): read XCom, assert every command exited rc=0
    # -----------------------------------------------------------------------
    @task
    def validate_results(ti=None):
        xcom = ti.xcom_pull(task_ids="run_commands")
        if xcom is None:
            raise ValueError("No XCom result returned by run_commands")

        state   = xcom.get("state")
        results = xcom.get("results", [])

        log.info("RemSvc state: %s  (%d result(s))", state, len(results))

        assert state == "SUCCESS", f"Expected state=SUCCESS, got {state!r}"
        assert len(results) == len(COMMANDS), (
            f"Expected {len(COMMANDS)} results, got {len(results)}"
        )

        for r in results:
            tid = r["tid"]
            rc  = r["rc"]
            out = r["out"].strip()
            err = r["err"].strip()
            cmd = r["cmd"]

            log.info(
                "tid=%d  rc=%d  cmd=%r\n  stdout: %s\n  stderr: %s",
                tid, rc, cmd, out or "(empty)", err or "(empty)",
            )

            assert rc == 0, (
                f"tid={tid} exited with rc={rc} (cmd={cmd!r})\n"
                f"  stdout: {out!r}\n  stderr: {err!r}"
            )

        # Spot-check: hostname should be non-empty (tid=2)
        hostname_out = results[1]["out"].strip()
        assert hostname_out, "hostname returned empty output"
        log.info("Remote hostname: %s", hostname_out)

        # Spot-check: uname output must contain "Linux" (tid=3)
        uname_out = results[2]["out"].strip()
        assert "Linux" in uname_out, (
            f"uname -a did not contain 'Linux': {uname_out!r}"
        )
        log.info("Remote OS: %s", uname_out)

        # Spot-check: whoami must return a non-empty username (tid=4)
        user_out = results[3]["out"].strip()
        assert user_out, "whoami returned empty output"
        log.info("Remote user: %s", user_out)

        log.info("All %d commands passed validation.", len(results))
        return json.dumps(xcom)   # pass full result downstream if needed

    check_connection() >> run_commands >> validate_results()


remsvc_e2e_linux()
