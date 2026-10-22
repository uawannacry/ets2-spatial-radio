#!/usr/bin/env bash
set -euo pipefail

echo "==> Building ETS2 Cabin Radio for Windows x64..."

ZIG="${ZIG:-zig}"
if ! command -v "$ZIG" &>/dev/null; then
  for candidate in \
    "$HOME/.local/bin/zig" \
    "/opt/homebrew/bin/zig" \
    "/usr/local/bin/zig" \
    "$HOME/bin/zig"
  do
    if [ -x "$candidate" ]; then
      ZIG="$candidate"
      break
    fi
  done
fi

if ! command -v "$ZIG" &>/dev/null && [ ! -x "$ZIG" ]; then
  echo "Error: zig compiler not found." >&2
  echo "Please install Zig (https://ziglang.org/download/) or specify ZIG=/path/to/zig" >&2
  exit 1
fi

# 1. Compile 64-bit live DSP DLL
"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -Wall -Wextra \
  -o ETS2CabinDSP.dll LiveCabinDSP.c upnp.c -lws2_32

# 2. Compile 64-bit Windows GUI launcher
"$ZIG" cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Wl,--subsystem,windows \
  -o "ETS2 Cabin Radio.exe" windows_main.c app.res \
  -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -ladvapi32

echo "==> Windows build complete: 'ETS2 Cabin Radio.exe' and 'ETS2CabinDSP.dll'"
