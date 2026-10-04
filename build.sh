#!/usr/bin/env bash
# Compile the core (core/*.c) to web/core/qflip.wasm.
# Only needed after changing something in core/: the built file is part of the repository.
# Needs clang and lld with the WebAssembly target (sudo apt install clang lld), or else Zig's
# bundled clang (pip install ziglang), which is used automatically when clang is missing.
set -euo pipefail
cd "$(dirname "$0")"
TARGET=wasm32
if [ -n "${CC:-}" ]; then CC_CMD=($CC)
elif command -v clang >/dev/null; then CC_CMD=(clang)
elif python3 -c 'import ziglang' 2>/dev/null; then CC_CMD=(python3 -m ziglang cc); TARGET=wasm32-freestanding
else echo "clang is not installed (sudo apt install clang lld, or pip install ziglang)"; exit 1; fi
mkdir -p web/core
"${CC_CMD[@]}" --target=$TARGET -std=c99 -O2 -nostdlib -ffreestanding -fvisibility=hidden -Wall -Wextra \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all -Wl,-z,stack-size=131072 \
  -o web/core/qflip.wasm core/qflip.c core/wasm_api.c
ls -l web/core/qflip.wasm
