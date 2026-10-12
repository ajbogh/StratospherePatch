#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the 32-bit proxy dinput.dll (needs: sudo apt install gcc-mingw-w64-i686)
set -euo pipefail
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -shared -O2 -Wall -o dinput.dll src/dinput.c \
    -Wl,--kill-at -Wl,--enable-stdcall-fixup -static-libgcc
echo "built dinput.dll ($(stat -c %s dinput.dll) bytes)"
