#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Stratosphere launch wrapper for Steam/Proton.
# Makes sure this game's Wine prefix runs in a 640x480 virtual desktop with a black backdrop
# (needed so the 8-bit intro videos are visible), then starts the game.
#
# Steam launch options (keep gamescope in front; run from the game folder):
#   WINEDLLOVERRIDES="dinput=n,b" gamescope -w 640 -h 480 -W 1920 -H 1080 -f -S fit -- ./launch.sh %command%
set -u
PROTON=""
for a in "$@"; do case "$a" in */proton) PROTON="$a"; break;; esac; done
PFX="${STEAM_COMPAT_DATA_PATH:-}/pfx"

if [ -n "$PROTON" ] && [ -n "${STEAM_COMPAT_DATA_PATH:-}" ]; then
  # only touch the prefix if the setting is missing
  if ! grep -qs '^"Stratosphere"="640x480"' "$PFX/user.reg"; then
    add() { "$PROTON" run reg add "$@" /f >/dev/null 2>&1; }
    add 'HKCU\Software\Wine\Explorer' /v Desktop /d Stratosphere
    add 'HKCU\Software\Wine\Explorer\Desktops' /v Stratosphere /d 640x480
    add 'HKCU\Control Panel\Colors' /v Background /t REG_SZ /d '0 0 0'
  fi
fi
exec "$@"
