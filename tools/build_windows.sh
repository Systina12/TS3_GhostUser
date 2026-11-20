#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${ZIG:?Set ZIG to a Zig 0.13.0 executable}"
: "${TS3_SDK_DIR:?Set TS3_SDK_DIR to the official API 26 plugin SDK checkout}"
command -v objdump >/dev/null || { printf 'Install binutils (objdump) to verify Windows DLL exports.\n' >&2; exit 1; }
mkdir -p dist build/windows
for source in plugin controller udp_gate; do
  "$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -fms-extensions -O2 -c \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE \
    -Isrc -I"$TS3_SDK_DIR/include" "src/$source.cpp" -o "build/windows/$source.obj"
done
"$ZIG" c++ -target x86_64-windows-gnu -shared \
  build/windows/plugin.obj build/windows/controller.obj build/windows/udp_gate.obj \
  -lws2_32 -lcomctl32 -luser32 -lgdi32 -o dist/ghostuser.dll
bash tools/check_windows_exports.sh dist/ghostuser.dll
python3 tools/package.py
