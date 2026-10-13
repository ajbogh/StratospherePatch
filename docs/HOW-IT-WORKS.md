# How the patch works (technical notes)

A small drop-in `dinput.dll` that fixes the two Wine bugs which make this game unplayable under Proton,
plus the Steam settings that make the display work. Tested with **Proton 10.0**.

## Symptoms this fixes
| Symptom under stock Proton | Cause | Fixed by |
|---|---|---|
| Spinner/black screen at startup, error dialog "Could not open ./sound/dn_aro.wav" (hidden behind the fullscreen window), crash when dismissed; later "Error reading extra bytes" | Wine's `mmioOpen` ignores the game's replacement of the default `"DOS "` I/O procedure (the game serves its sounds out of `.zpr` archives that way), and `mmioSeek` reports position 0 for the game's handler | the shim (always on) |
| Mouse cursor ghosting: stale copies of the target cursor smear across the background in menus and missions | The game's software-cursor erase assumes `Flip` leaves the back buffer intact for system-memory flip chains; Wine swaps the textures instead | the shim (on by default) |
| Menus: the highlighted item is not the one under the cursor (clicks still hit the right item) | In menus the game acquires the DirectInput mouse exclusively and tracks its own position; Windows then sends it no mouse-move messages, but Wine still does, from the OS pointer (parked in the corner and clamped), and the highlight code follows those | the shim (on by default): drops the mouse messages at the game's `WH_MOUSE` hook while the game is polling the DirectInput mouse (menus). In missions it does not poll, takes the pointer from the messages, and they are left alone |
| Menus: dropdowns open and instantly close when clicked | Every click arrives twice: as a DirectInput button event and, ~16 ms later, as a Windows button message (Windows sends none while the DI mouse is exclusive). The second one re-triggers the click | the shim (on by default): same fix as above, it drops all mouse messages (moves and buttons) in menu mode |
| Fortress Builder: "Cannot SafeLock surface", crash | The game briefly leaves exclusive mode; Wine then reports the whole desktop (e.g. 6144x3456) as the display mode and flags the game's 640x480 surfaces lost | run inside a 640x480 display: a Wine virtual desktop (set up by `launch.sh`) |
| Intro/promo videos are a black screen (audio plays) | Wine's fullscreen-exclusive presentation of the 8-bit palettized screen shows black (the frames are correct in memory; seen on Proton 10.0 and Experimental). In a window they display fine | Wine 640x480 virtual desktop (set up by `launch.sh`), scaled to fullscreen by gamescope |

## Which copy of the game this was tested with
A 290 MB InstallShield-style self-extracting `Stratosphere.exe` (a modern-Windows repack of the 1998 game; its payload contains `strat.exe`, `data/*.zpr`,
`fmv/*.fmv`, `smackw32.dll` and a `ddraw.dll`). **Delete that `ddraw.dll` (and `ddraw.log`)** - it is DDrawCompat, a Windows-only wrapper, and does not work under Wine/Proton.
Use the 1998 `smackw32.dll` that ships in the payload (95,232 bytes); a 2002 build is not needed. The installer is not run under Linux: extract it
(`7z`/Python's `zipfile` can read the payload that follows the CAB stub) and copy the files into the game folder.

## Install
1. **You need a complete game install.** All of these must exist or the game fails/loses music:
   `data/data0000.zpr` ... `data/data0010.zpr`, `data/patch000.zpr` **and** `patch000.zpr` in the game's root folder
   (the game scans `data\patch*.zpr` but then opens `patch000.zpr` from the root; missing it crashes at startup).
   `data0000` and `data0009` hold the music.
2. Copy `dinput.dll` from this folder next to `strat.exe`.
3. Copy `launch.sh` next to `strat.exe` too (`chmod +x launch.sh`). It makes sure the game's Wine prefix uses a 640x480 virtual desktop with a black backdrop
   (three `reg add` calls through Proton, only when missing), then starts the game. The very first launch shows a blue Wine backdrop once; after that it is black.
4. In Steam: *Add a Non-Steam Game* -> `strat.exe`. Properties -> Compatibility -> force **Proton 10.0**.
5. Launch options (adjust `-W`/`-H` to your monitor and use the full path to `launch.sh`):
   ```
   WINEDLLOVERRIDES="dinput=n,b" gamescope -w 640 -h 480 -W 1920 -H 1080 -f -S fit -- /path/to/game/launch.sh %command%
   ```
   (`apt install gamescope` or your distro's equivalent.) Tested with `-W 6144 -H 3456 -f -S fit` on a 6K monitor. The game is 4:3, so `-S fit` pillarboxes it.
   The wrapper needs `STEAM_COMPAT_DATA_PATH` (Steam sets it); if it is missing the wrapper just starts the game without touching the registry.

## Options (environment variables for the shim)
- `MMIOFIX_EXP=none` turn off the Flip fix (the mmio fix cannot be turned off). Other values (`blt`, `nosrc`, `touch`, `sync*`, `trace`) are diagnostic experiments.
- `MMIOFIX_MOUSE=off` turn off the menu mouse fixes (highlight offset, double clicks).
- `MMIOFIX_LOG=1` write `mmiofix.log` in the game folder (also logs the mouse calls and every menu/mission mouse-mode switch). `MMIOFIX_AUTODUMP=8` dump front/back buffers as BMP every 8 s (F11 or a file named `dump.now` dumps once).

## Status / not covered
- Tested: startup, main menu, options, Fortress Builder, unit library, single-mission setup, playing a mission, quitting. Sound and music work.
- Intro/promo videos (`fmv/*.fmv`, Smacker) play with the original 1998 `smackw32.dll` (the installer's). A 2002 `smackw32.dll` is not needed. Without the virtual desktop the video screen stays black.
- Multiplayer (DirectPlay) untested.
- Without the 640x480 virtual desktop, Fortress Builder crashes and videos are black (see table).
- The game itself is a 1998 title and a bit glitchy even on Windows.
- Tested on: Ubuntu (kernel 7.0), AMD RX 7900 XT (Mesa 26.0), Proton 10.0 (`proton-10.0-4b`), gamescope 3.16.20, Wayland session.

## How it works (short)
`strat.exe` imports `DirectInputCreateA` from `dinput.dll`, so a proxy `dinput.dll` gets loaded early. It forwards that call to Wine's real
`dinput.dll`, then
1. patches `strat.exe`'s import table so `mmioInstallIOProcA` records the game's `"DOS "` handler and `mmioOpenA` passes it explicitly (and wraps it so `MMIOM_SEEK` returns `lDiskOffset`);
2. patches the shared vtable of Wine's `IDirectDrawSurface2` so `Flip` on a **system-memory** chain becomes a back-to-front `Blt` (video-memory chains keep the real flip).
Full analysis and traces: see the two `wine-bug-*.md` files.

## Build
`sudo apt install gcc-mingw-w64-i686 && ./build.sh`

## License
The code and documentation written for this project (`src/dinput.c`, `build.sh`, `launch.sh`, the README and the Wine bug drafts) are released under the **MIT License** (see `LICENSE`).
`dinput.dll` in this repository is built from `src/dinput.c` with `build.sh`.

This repository contains **no files from the game or from anyone else's software**. The game, its installer, data and videos belong to their respective owners (Kodiak Interactive / Ripcord Games and other rights holders)
and are not included or covered by this license. If other files are ever added to this repository, the license for those files is whatever their own authors specified; the MIT license above applies only to the code written for this project.
"Wine", "Proton", "Steam" and "gamescope" are names of other projects used here; this project is not affiliated with them.

SHA-256 of the included `dinput.dll`: `b370e12bd5c6a903e272f2d0a66fdf6c0eb408726e4f7e77b6cd2a46e87d1dc3` (also in `SHA256SUMS`). The build is not byte-for-byte reproducible (the linker stamps a timestamp), so compare behaviour, or build it yourself from source.
