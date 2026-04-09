"""
hatch_build.py
==============
Custom Hatchling build hook for airflow-provider-remsvc.

Automatically generates Python gRPC stubs from RemSvc/src/proto/RemSvc.proto
into RemSvc/prv/remsvc_proto/ before the wheel is assembled.

This makes ``pip install .`` (or ``pip install -e .``) fully self-contained —
no manual ``./regen_proto.sh`` step is needed after installation.

The hook requires ``grpcio-tools`` to be available in the build environment.
It is declared as a build-system dependency in pyproject.toml so that pip
installs it automatically in the isolated build environment.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    """Generate remsvc_proto stubs before the wheel is built."""

    PLUGIN_NAME = "custom"

    def initialize(self, version: str, build_data: dict) -> None:
        prv_dir   = Path(__file__).parent.resolve()
        repo_root = prv_dir.parent
        proto_src = repo_root / "src" / "proto"
        stub_out  = prv_dir / "remsvc_proto"

        proto_file = proto_src / "RemSvc.proto"
        if not proto_file.exists():
            raise FileNotFoundError(
                f"hatch_build: proto source not found: {proto_file}\n"
                "Ensure the repository is fully checked out."
            )

        stub_out.mkdir(parents=True, exist_ok=True)
        (stub_out / "__init__.py").touch(exist_ok=True)

        # Idempotency: skip regeneration if stubs exist and are newer than proto.
        pb2_file = stub_out / "RemSvc_pb2.py"
        if pb2_file.exists():
            if pb2_file.stat().st_mtime >= proto_file.stat().st_mtime:
                self.app.display_info(
                    "hatch_build: stubs are up to date, skipping regeneration."
                )
                return

        self.app.display_info(
            f"hatch_build: generating gRPC stubs from {proto_file} → {stub_out}"
        )

        # Build the protoc command.  The --mypy_out flag requires the
        # mypy-protobuf plugin (grpc_tools.protoc_mypy_plugin); if it is not
        # installed we skip .pyi generation rather than failing the build — the
        # stubs are optional (IDE / mypy support only; runtime behaviour is
        # unchanged without them).
        cmd = [
            sys.executable, "-m", "grpc_tools.protoc",
            f"-I{proto_src}",
            f"--python_out={stub_out}",
            f"--grpc_python_out={stub_out}",
        ]

        # Probe for mypy-protobuf.
        mypy_probe = subprocess.run(
            [sys.executable, "-c", "import grpc_tools.protoc_mypy_plugin"],
            capture_output=True,
        )
        if mypy_probe.returncode == 0:
            cmd.append(f"--mypy_out={stub_out}")
            self.app.display_info("hatch_build: mypy-protobuf available — generating .pyi stubs")
        else:
            self.app.display_info(
                "hatch_build: mypy-protobuf not installed — skipping .pyi stub generation "
                "(install mypy-protobuf in dev extras for IDE/mypy support)"
            )

        cmd.append(str(proto_file))

        result = subprocess.run(
            cmd,
            cwd=str(prv_dir),
            capture_output=True,
            text=True,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"hatch_build: protoc failed (exit {result.returncode}).\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

        # Fix grpc_tools codegen bug: the generated *_pb2_grpc.py uses a bare
        # `import RemSvc_pb2` which works in-directory but fails when the stubs
        # are installed as a package.  Rewrite to a package-qualified import.
        grpc_stub = stub_out / "RemSvc_pb2_grpc.py"
        if grpc_stub.exists():
            text = grpc_stub.read_text()
            fixed = text.replace(
                "import RemSvc_pb2 as RemSvc__pb2",
                "from remsvc_proto import RemSvc_pb2 as RemSvc__pb2",
            )
            if fixed != text:
                grpc_stub.write_text(fixed)
                self.app.display_info("hatch_build: patched RemSvc_pb2_grpc.py import.")

        self.app.display_info("hatch_build: stub generation complete.")
