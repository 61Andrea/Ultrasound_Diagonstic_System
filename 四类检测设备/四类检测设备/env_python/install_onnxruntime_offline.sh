#!/bin/sh

set -e

echo "========================================"
echo " Offline install: pip + numpy + onnxruntime"
echo "========================================"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

echo "Current directory: $SCRIPT_DIR"

echo "Checking python3 and pip3..."
python3 --version
pip3 --version

echo "----------------------------------------"
echo "1. Install pip 24.0"
echo "----------------------------------------"
pip3 install --no-index pip-24.0-py3-none-any.whl

echo "----------------------------------------"
echo "2. Install numpy 1.19.5"
echo "----------------------------------------"
pip3 install --no-index numpy-1.19.5-cp37-cp37m-manylinux2014_aarch64.whl

echo "----------------------------------------"
echo "3. Install protobuf 3.20.3"
echo "----------------------------------------"
pip3 install --no-index protobuf-3.20.3-py2.py3-none-any.whl

echo "----------------------------------------"
echo "4. Install flatbuffers 23.5.26"
echo "----------------------------------------"
pip3 install --no-index flatbuffers-23.5.26-py2.py3-none-any.whl

echo "----------------------------------------"
echo "5. Install onnxruntime 1.10.0"
echo "----------------------------------------"
pip3 install --no-index onnxruntime-1.10.0-cp37-cp37m-manylinux_2_17_aarch64.manylinux2014_aarch64.whl

echo "----------------------------------------"
echo "6. Verify"
echo "----------------------------------------"
python3 - <<'EOF'
import numpy
import onnxruntime as ort

print("numpy version:", numpy.__version__)
print("onnxruntime version:", ort.__version__)
print("providers:", ort.get_available_providers())
EOF

echo "========================================"
echo " Install finished successfully"
echo "========================================"