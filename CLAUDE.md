# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**RemSvc** is a Remote Execution Service built on gRPC. It has two main components:
1. **C++ gRPC server/client** (`src/`) — executes shell commands on the host machine
2. **Apache Airflow provider** (`prv/`) — deferrable operator/trigger for distributed job orchestration

The gRPC service definition (`src/proto/RemSvc.proto`) is the contract between all components.

## C++ Build (CMake + VCPKG)

```bash
# Configure (requires VCPKG_ROOT environment variable set)
cmake --preset=bld_vc

# Build all targets
cmake --build bld_vc

# Run tests
ctest --test-dir bld_vc
```

Build outputs go to `bld_vc/`. Key binaries:
- `bld_vc/server/RemSvc_server.exe` — gRPC server
- `bld_vc/client/RemSvc_client.exe` — CLI client
- `bld_vc/server/Rest_server.exe` — Qt HTTP/REST wrapper

The preset `bld_vc` targets Visual C++ with Ninja. CMake 3.18.4+ required, C++20 standard.

## Python Airflow Provider

```bash
cd prv/

# Install with dev dependencies
pip install -e ".[dev]"

# Run all tests (testpaths in pyproject.toml points to tst/unit/prv/)
pytest

# Run all tests from repo root (no cd required)
PYTHONPATH=prv pytest tst/unit/prv

# Run a single test (from prv/)
pytest ../tst/unit/prv/test_remsvc_async.py::TestRemSvcTrigger::test_run_success -v

# Regenerate Python gRPC stubs from proto
./regen_proto.sh
```

Tests live in `tst/unit/prv/`. `tst/unit/prv/conftest.py` adds `prv/` to `sys.path` automatically.
Tests use `pytest-asyncio` with `asyncio_mode = "auto"` (configured in `pyproject.toml`).

## Architecture

### gRPC Service (`src/proto/RemSvc.proto`)
Four RPC methods:
- `Ping` — health check
- `GetStatus` — service status
- `RemCmd(RemCmdMsg) → RemResMsg` — single command execution
- `RemCmdStrm` — bidirectional streaming variant

`RemCmdMsg` fields: `cmd` (command string), `cmdtyp` (0=shell, 1=PowerShell), `cmdusr`, `tid` (transaction ID), `src`, `hsh`.

### C++ Server (`src/server/`)
- `RemSvcServiceImpl` implements the gRPC service; runs in a `QThread` (`GrpcServerThread`)
- Command execution uses `QProcess`: Windows routes through `cmd.exe /C`, Linux through `/bin/sh -c`
- Command filtering: denylist is checked first (any match → `PERMISSION_DENIED`), then allowlist (must match at least one pattern, or allowlist is empty). A bad regex in either list activates deny-all mode at startup.
- `BearerTokenAuthProcessor` enforces bearer-token auth; requires TLS — server refuses to start if `[auth]` tokens are configured without TLS enabled.
- `RestServer.cc` is a separate Qt HTTP server wrapping the gRPC interface
- Signal handling (`SignalHandler.cc`) supports graceful shutdown on both Unix and Windows

### Python Airflow Integration (`prv/remsvc_provider/`)
Deferrable operator pattern — frees Airflow worker slots during job polling:
1. `RemSvcOperator.execute()` submits the job via `SubmitJob` RPC, then calls `raise TaskDeferred(trigger=RemSvcTrigger(...))`
2. `RemSvcTrigger` runs in the Airflow triggerer process using `grpc.aio` (async); polls until terminal state
3. `RemSvcOperator.execute_complete()` resumes, fetches result, pushes to XCom

The `remsvc_proto/` package contains auto-generated Python gRPC stubs — regenerate with `regen_proto.sh` when the `.proto` changes, not manually.

### Common C++ Utilities (`src/common/`)
- **Log.hh** — spdlog wrapper with debug levels (debug0–debug9) and `std::source_location` support
- **CLI.hh** — CLI11 wrapper; global vars `CliLogFile`, `CliLogLevel`, `CliDbgLevel` configure logging
- **Assert.hh** — GSL contract checking (`Expects`, `Ensures`)

## Key Dependencies

| Component | Dependency | Notes |
|-----------|-----------|-------|
| C++ build | VCPKG | Set `VCPKG_ROOT` env var |
| gRPC/protobuf | vcpkg / Linux stow `/data/local/stow/` | Platform-specific paths in CMakeLists.txt |
| Qt6 | Qt6 Core, HttpServer | Used for QProcess and REST server |
| Logging | spdlog | |
| CLI | CLI11 2.4.1 | |
| Python gRPC | grpcio >= 1.51.0 | |
| Airflow | Apache Airflow >= 2.2.0 | Deferrable operators require 2.2+ |
