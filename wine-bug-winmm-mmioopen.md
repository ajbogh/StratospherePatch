# winmm: mmioOpen() ignores an application-installed replacement of the default "DOS " I/O procedure

*Draft for bugs.winehq.org - verify against current master before filing.*

**Component:** winmm  **Seen with:** Proton 10.0 (Wine 10.0 based), also reproduced on Proton 5.13 (`proton-5.13-6d`)
**Application:** Stratosphere: Conquest of the Skies (1998, 32-bit, DirectX 5 era)

## Summary
The application replaces the default file I/O procedure and then opens files with no explicit procedure:

```c
mmioInstallIOProcA(mmioFOURCC('D','O','S',' '), game_proc, MMIO_INSTALLPROC);
h = mmioOpenA(".\\sound\\dn_aro.wav", NULL, MMIO_READ|...);   /* flags observed: 0x20 */
```
`game_proc` serves the file out of the game's own `.zpr` archive. With Wine, `mmioOpen` uses the built-in `mmioDosIOProc`
instead, looks for a loose file, fails, and returns NULL. The game then passes the NULL handle on to `mmioDescend()` and
shows "Could not open ./sound/dn_aro.wav" and crashes.

## Trace (`WINEDEBUG=+mmio,+file`)
```
MMIO_InstallIOProc (20534f44, 0051EAC0, 00010000, ansi)       <- 'DOS ', application proc, MMIO_INSTALLPROC
MMIO_Open (".\\sound\\dn_aro.wav", 00000000, 00000020, ansi);
MMIO_ParseExtA (".\\sound\\dn_aro.wav")
MMIO_SetBuffer (00A62508 00000000 0 0)
mmioDosIOProc (00A62508, 3, 0x3942428, 0x0);                   <- builtin handler, MMIOM_OPEN
mmioDescend (00000000, ...)                                    <- handle is NULL: open failed
```

## Cause (as I read dlls/winmm/mmio.c - please double-check)
In `MMIO_Open`, when no `fccIOProc`/`pIOProc` is passed and `MMIO_ParseExtA()` finds no `FOURCC+` prefix:
```c
wm->info.fccIOProc = FOURCC_DOS;
wm->ioProc = &defaultProcs[0];          /* hard-wired builtin */
```
`MMIO_InstallIOProc()` correctly prepends the application's node (so `MMIO_FindProcNode(FOURCC_DOS)` would return it), but
`MMIO_Open` never looks it up.

## Suggested fix (untested - I could not rebuild Wine)
```c
wm->info.fccIOProc = FOURCC_DOS;
if (!(wm->ioProc = MMIO_FindProcNode(FOURCC_DOS))) wm->ioProc = &defaultProcs[0];
```

## Expected behaviour
The game works on Windows (it was run there with DDrawCompat), so the installed `"DOS "` procedure must be used by `mmioOpen`.
I have not verified this with a standalone test program on native Windows; a conformance test would be a good addition.

## Possible second issue in the same flow: MMIOM_SEEK return value
After the open works, the game's procedure implements `MMIOM_SEEK` as: seek the underlying stream, store the new position
in `lpmmioinfo->lDiskOffset`, **return 0**. Wine's `mmioSeek()` (unbuffered handle) returns the procedure's return value, so
`mmioSeek(h, 0, SEEK_CUR)` reports 0 and `mmioDescend()` re-reads from the wrong offset ("seek inside parent at 8"), which gives
a garbage `cbSize` and the game's "Error reading extra bytes". The game works on Windows, so Windows evidently reports
`lDiskOffset`, but I have not confirmed Windows' exact rule. My workaround wraps the game's procedure and returns
`lDiskOffset` when `MMIOM_SEEK` returns 0.

## Workaround
Proxy `dinput.dll` that hooks the game's `mmioInstallIOProcA`/`mmioOpenA` imports (source in this repo).
