#!/bin/bash
set -x

# Activate Python venv
source $HOME/Python311_venv/bin/activate


# Project Directory
PRJ=/data/airflow/RemSvc
# Proto Directory
PD=$PRJ/pro
# Proto File
PF=$PD/RemSvc.proto

# The "protoc" Compiler/Generator
PC=/data/local/stow/grpc-1.67.0/bin/protoc

# The output directory
OD=$PRJ/src/pyclient


#1
#$PC --python_out=$OD --grpc_python_out=$OD --mypy_out=$OD --mypy_grpc_out=$OD --plugin=protoc-gen-grpc_python=/home/scemiloglu@internal.bupa.com.au/Python311_venv/bin/protoc-gen-grpclib_python -I $PD RemSvc.proto

#2
python -m grpc_tools.protoc -I $PD --python_out=$OD --grpc_python_out=$OD $PF
