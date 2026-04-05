# airflow-provider-remsvc

Apache Airflow provider for **RemSvc** — a gRPC-based remote command execution service.

## Requirements

- Apache Airflow >= 3.1.0
- Python >= 3.10
- `grpcio >= 1.51.0`
- `grpcio-status >= 1.51.0`
- `protobuf >= 4.0.0`
- RemSvc Python gRPC stubs (generated via `regen_proto.sh`)

## Installation

```bash
# From the prv/ directory
pip install .

# Generate proto stubs (required before use)
./regen_proto.sh
```

## Airflow Connection Setup

Create a connection of type `remsvc` in the Airflow UI or via environment variable:

```bash
# Insecure (development)
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={}'

# TLS with system trust store
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":true}'

# TLS with custom CA certificate
export AIRFLOW_CONN_REMSVC_DEFAULT='remsvc://remotehost:50051?extra={"use_ssl":true,"ca_cert_path":"/etc/ssl/remsvc-ca.pem"}'
```

Connection fields:

| Field | Description |
|-------|-------------|
| `host` | RemSvc server hostname or IP |
| `port` | RemSvc server port (default: 50051) |
| `extra.use_ssl` | `true` to enable TLS (default: `false`) |
| `extra.ca_cert_path` | Path to CA certificate PEM file (optional; uses system trust store if omitted) |

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

All commands are sent over a single `RemCmdStrm` bidirectional stream. Responses may
arrive in any order and are correlated back to their originating command via `tid`
(1-based index in the `commands` list). The operator fails the task if any command
returns a non-zero exit code or if any response is missing.

### Command dict fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | str | Yes | Command string to execute |
| `cmdtyp` | int | No | `0` = native shell (default), `1` = PowerShell |
| `cmdusr` | str | No | OS user to run the command as (Linux only) |
| `src` | str | No | Source identifier for server-side logging |

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
pip install -e ".[dev]"
./regen_proto.sh
pytest
```
