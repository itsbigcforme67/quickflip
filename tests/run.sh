#!/usr/bin/env bash
# Tests that run on the PC: the C core (under the address and undefined-behaviour sanitizers),
# then the server's Python reader of the same files.
set -euo pipefail
cd "$(dirname "$0")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
gcc -std=c99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
  -o "$OUT/test_core" test_core.c ../core/qflip.c
"$OUT/test_core" "$OUT/sample.qflip"
python3 test_server.py "$OUT/sample.qflip"
