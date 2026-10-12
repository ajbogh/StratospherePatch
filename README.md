# Stratosphere: Conquest of the Skies - Linux / Proton patch

Makes the 1998 game *Stratosphere: Conquest of the Skies* run under **Steam + Proton 10.0**: sound, music, intro videos, menus, Fortress Builder and missions.
This repository contains only the patch files. **You need your own copy of the game.**

## You need
- Steam with **Proton 10.0**
- [gamescope](https://github.com/ValveSoftware/gamescope) (`sudo apt install gamescope` or your distro's equivalent)
- Your copy of the game installer (`Stratosphere.exe`)
- From this repository: `dinput.dll` and `launch.sh`

## Install
1. **Install the game.** In Steam choose *Games -> Add a Non-Steam Game* and add the installer `Stratosphere.exe`.
   Open its *Properties -> Compatibility*, tick *Force the use of a specific Steam Play compatibility tool* and pick **Proton 10.0**.
   Click *Play*, run the installer and pick the **Full** install. Remember the folder it installs to (the one that contains `strat.exe`).
2. **Point Steam at the game.** Open the same entry's *Properties* and set *Target* to `strat.exe` in that folder and *Start In* to the folder.
3. **Add the patch files.** Copy `dinput.dll` and `launch.sh` from this repository into the game folder, next to `strat.exe`, then run `chmod +x launch.sh`.
   If the folder contains `ddraw.dll` or `ddraw.log`, delete them (they are for Windows and break the game under Wine).
4. **Set the launch options** (*Properties -> Launch Options*). Use the full path to your game folder, and set `-W` / `-H` to your monitor's resolution:
   ```
   WINEDLLOVERRIDES="dinput=n,b" gamescope -w 640 -h 480 -W 1920 -H 1080 -f -S fit -- "/full/path/to/game folder/launch.sh" %command%
   ```
5. **Play.** The first launch takes a little longer and shows a blue background once; after that the background is black.

## Notes
- Tested with Proton 10.0 on Ubuntu with an AMD GPU. Multiplayer is untested.
- The game is from 1998 and a bit glitchy even on Windows.
- Something not working? See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for what the patch does, and [open an issue](../../issues).

## License
The code and documentation in this repository are released under the **MIT License** (see `LICENSE`).
This repository contains **no files from the game or from anyone else's software**; the game and its files belong to their owners and are not covered by this license.
If other files are ever added, their own licenses apply to them.
"Wine", "Proton", "Steam" and "gamescope" are other projects; this one is not affiliated with them.
SHA-256 of `dinput.dll`: see `SHA256SUMS`.
