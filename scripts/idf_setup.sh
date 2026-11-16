#!/bin/bash
# idf_setup.sh — recreate the ESP-IDF v6.1 build environment after a sandbox
# recycle (the tools live under my-project/.idf, which is gitignored).
set -euo pipefail

IDF_ROOT=/home/z/my-project/.idf
IDF_PATH=$IDF_ROOT/v6.1/esp-idf
export IDF_TOOLS_PATH=$IDF_ROOT/tools

echo "=== idf_setup: python/cmake/ninja ==="
python3 --version
python3 -m pip install --quiet cmake ninja 2>&1 | tail -1 || true
cmake --version | head -1
ninja --version

if [ ! -d "$IDF_PATH" ]; then
  echo "=== idf_setup: cloning esp-idf v6.1 (shallow) ==="
  mkdir -p "$IDF_ROOT/v6.1"
  git clone --depth 1 --branch v6.1 https://github.com/espressif/esp-idf.git "$IDF_PATH" 2>&1 | tail -2
fi

echo "=== idf_setup: install.sh esp32s3 ==="
cd "$IDF_PATH"
./install.sh esp32s3 2>&1 | tail -5

echo "=== idf_setup: DONE ==="
