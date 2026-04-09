#!/usr/bin/env bash
# regen_proto.sh
# Regenerates gRPC stubs from RemSvc/src/proto/ into RemSvc/prv/remsvc_proto/.
#
# Run from anywhere inside the repo:
#   chmod +x prv/regen_proto.sh
#   ./prv/regen_proto.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_SRC="${REPO_ROOT}/src/proto"
STUB_OUT="${REPO_ROOT}/prv/remsvc_proto"

echo "Proto source : ${PROTO_SRC}"
echo "Stub output  : ${STUB_OUT}"

mkdir -p "${STUB_OUT}"
touch "${STUB_OUT}/__init__.py"

python3 -m grpc_tools.protoc \
    -I "${PROTO_SRC}" \
    --python_out="${STUB_OUT}" \
    --grpc_python_out="${STUB_OUT}" \
    "${PROTO_SRC}"/*.proto

# Fix grpc_tools codegen bug: bare `import RemSvc_pb2` fails when installed as
# a package; rewrite to a package-qualified import.
sed -i 's/^import RemSvc_pb2 as RemSvc__pb2$/from remsvc_proto import RemSvc_pb2 as RemSvc__pb2/' \
    "${STUB_OUT}/RemSvc_pb2_grpc.py"

echo "Done. Stubs written to ${STUB_OUT}/"
