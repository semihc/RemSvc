# airflow-provider-remsvc

Apache Airflow provider for **RemSvc** — a gRPC-based remote command execution service.

## Requirements

- Apache Airflow >= 3.1.0
- Python >= 3.10
- `grpcio >= 1.67.0`
- `protobuf >= 4.0.0`

## Installation

```bash
# From the prv/ directory — stubs are generated automatically at install time
pip install .
```

The package is self-contained. `pip install` runs `hatch_build.py` which invokes
`grpc_tools.protoc` in an isolated build environment to generate the `remsvc_proto`
stubs and bundle them into the wheel. No manual proto generation step is needed.

## Airflow Connection Setup

Create a connection of type `remsvc` in the Airflow UI or via environment variable:

```bash
# Insecure, no authentication (development only)
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={}'

# Insecure channel + bearer token
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"bearer_token":"secret-prod-token"}'

# TLS with system trust store + bearer token (recommended for production)
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":true,"bearer_token":"secret-prod-token"}'

# TLS with custom CA certificate + bearer token
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":true,"ca_cert_path":"/etc/ssl/remsvc-ca.pem","bearer_token":"secret-prod-token"}'
```

Connection `extra` fields:

| Field | Type | Description |
|-------|------|-------------|
| `use_ssl` | bool | Enable TLS (default: `false`) |
| `ca_cert_path` | str | Path to CA certificate PEM file; uses system trust store if omitted |
| `bearer_token` | str | Bearer token sent as `Authorization: Bearer <token>` on every gRPC call. Must match an identity entry in the server's `[auth]` config section. Omit if the server has no `[auth]` section configured. |

### Authentication overview

The server enforces per-call bearer-token authentication when its INI config
contains an `[auth]` section.  Each token is tied to a named identity:

```ini
# server.ini
[auth]
airflow-prod    = secret-prod-token
airflow-staging = secret-staging-token
dev-semih       = secret-dev-token
```

The token is read from the Airflow connection's `bearer_token` extra field and
injected automatically as `Authorization: Bearer <token>` metadata on every
gRPC call.  Service handlers never see the raw token — it is consumed by the
server-side auth processor before the RPC runs.  The verified identity
(`airflow-prod`, etc.) is logged on the server for each authenticated call.

If the server's `[auth]` section is absent or empty, all callers are permitted
and `bearer_token` in the connection extra is silently ignored.

> **Security note:** bearer tokens provide caller identity but not
> confidentiality.  Always pair with TLS (`use_ssl: true`) in production so
> tokens are not transmitted in plaintext.

## Usage

### Single command

```python
from remsvc_provider.operators.remsvc import RemSvcOperator

run = RemSvcOperator(
    task_id="run_remote",
    grpc_conn_id="remsvc_default",
    commands=[{"cmd": "echo hello", "cmdtyp": 0}],
    dag=dag,
)
```

### Multiple commands (streamed concurrently)

```python
run = RemSvcOperator(
    task_id="run_remote",
    grpc_conn_id="remsvc_default",
    commands=[
        {"cmd": "echo {{ ds }}",  "cmdtyp": 0},
        {"cmd": "hostname",       "cmdtyp": 0},
        {"cmd": "whoami",         "cmdtyp": 0},
    ],
    stream_timeout=120.0,
    dag=dag,
)
```

All commands are sent over a single `RemCmdStrm` bidirectional stream.  The server
processes commands sequentially (one child process at a time) and echoes back the
`tid` in each response.  The trigger correlates responses to commands by `tid`
(1-based index in the `commands` list) and collects all results before resuming
the worker.  The operator fails the task if any command returns a non-zero exit
code or if any response is missing.

### Command execution model

Each command the server receives is run in a dedicated OS child process
(`cmd.exe /C` on Windows or `/bin/sh -c` on Linux for `cmdtyp=0`;
`powershell.exe` on Windows or `pwsh` on Linux for `cmdtyp=1`).
Every command gets its own address space, PID, and stdio pipes; when it exits
the process is reaped and all resources are released.

The server processes messages from a single stream sequentially — one child
process at a time.  The Airflow trigger sends all commands and reads all
responses concurrently (`asyncio.gather`), so from the operator's perspective
commands appear to run in parallel, but on the server they are dispatched one
after another in arrival order.  If two Airflow tasks target the same server
concurrently their messages interleave at the gRPC level, but each command
still runs in its own isolated child process — they share no process state.

### Operator parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `commands` | `list[dict]` | required | List of command dicts (see table below). Jinja-templated. |
| `grpc_conn_id` | str | `"remsvc_default"` | Airflow connection ID of type `remsvc` |
| `stream_timeout` | float | `3600.0` | Maximum seconds for the entire stream, enforced at both the Python asyncio level and as the gRPC deadline. If exceeded the task is cancelled (not retried). |
| `metadata` | `list[tuple[str,str]]` | `[]` | Additional gRPC call metadata. Connection-level `authorization` takes precedence — duplicate keys from this list are stripped. Jinja-templated. |
| `result_handler` | `Callable` | built-in | Applied to the raw `{tid: result_dict}` mapping before XCom push. Defaults to a list sorted ascending by `tid`. Supply a custom callable for filtering or reshaping results. |

### Command dict fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | str | Yes | Command string to execute. Must be non-empty and non-whitespace — the operator raises `AirflowException` at validation time if any command is blank. |
| `cmdtyp` | int | No | `0` = native shell (default): `cmd.exe /C` on Windows, `/bin/sh -c` on Linux. `1` = PowerShell: `powershell.exe` on Windows, `pwsh` on Linux. |
| `cmdusr` | str | No | OS user to run the command as (Linux only; silently ignored on Windows). |
| `src` | str | No | Source identifier included in server-side log entries for this command. |

### XCom result

`execute_complete` returns a dict pushed to XCom under `return_value`:

```python
{
    "state": "SUCCESS",
    "results": [
        {"tid": 1, "rc": 0, "out": "2024-01-01\n", "err": "", "hsh": "...", "cmd": "echo {{ ds }}"},
        {"tid": 2, "rc": 0, "out": "myhost\n",      "err": "", "hsh": "...", "cmd": "hostname"},
    ],
    "error_msg": None,
}
```

Results are sorted by `tid` (i.e. original command order).

## Development

```bash
cd prv/

# Install in editable mode — stubs are generated automatically by the build hook
pip install -e ".[dev]"

# Run all tests (path is configured in pyproject.toml)
pytest

# Run a specific test file
pytest ../tst/unit/prv/test_remsvc_async.py -v
pytest ../tst/unit/prv/test_remsvc_hook.py -v
```

> **Regenerating stubs manually** — only needed if `src/proto/RemSvc.proto` changes
> and you are working in an editable install without rebuilding:
>
> ```bash
> ./regen_proto.sh
> ```
