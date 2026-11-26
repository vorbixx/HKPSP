# HKPSP

A native port of the Steam version of **Hollow Knight** to the **Sony PSP**
(PSP-1000/2000/3000), written in C++17 with pspdev/PSPSDK and `sceGu`.

**The port is a work in progress.** It does not include the Black Egg, the
final encounter or Ending A yet; they will come in later releases.

No original game files are in this repository or in the releases. You build
the game data yourself from your own Steam copy of Hollow Knight.

## What works

- Native 480×272 rendering at a fixed 30 FPS on PSP-2000/3000. The PSP-1000
  has less memory and streams its textures, so its frame rate is lower.
- Movement, jumping, nail combat, spells, damage, Soul and healing.
- Room loading, transitions, collision, cameras, doors, switches, lifts,
  breakable floors, walls and ceilings.
- Enemies, bosses up to the current route, NPC dialogue, shops, benches, four
  save slots, death and the Shade.
- Map and inventory, layered scenery, lighting, effects, music and sounds.
- PSP-1000 memory mode with streamed CLUT8 texture pages.

Checked in PPSSPP in normal and PSP-1000 modes. Some rooms and objects may
still look or behave differently from the original.

## Install from a release

You need:

- your own Steam copy of Hollow Knight, version 1.5.12620 (Steam build
  22529139, the current public branch);
- Windows 10/11 (64-bit) or Linux (x86-64);
- about 35 GB of free disk space and a few hours for the first build.

Steps:

1. Download the release archive for your system from the
   [Releases](../../releases) page and unpack it to a folder with a short path
   (for example `C:\HKPSP` or `~/HKPSP`).
2. Run the builder:
   - Windows: double-click **`Build HKPSP.bat`**;
   - Linux: run `./build-hkpsp.sh`.

   It finds Hollow Knight in your Steam libraries. If it does not, pass the
   game folder: `"Build HKPSP.bat" --game "D:\SteamLibrary\steamapps\common\Hollow Knight"`
   (Linux: `./build-hkpsp.sh --game "<folder>"`). `--check` only runs the checks.
   You can stop the build and run it again later: finished steps are kept.
3. When it says **Done**, copy the folder `build/psp/package-game/PSP/GAME/HKPSP`
   to `PSP/GAME/` on your memory stick, then start **HKPSP** from the Game menu.

If the build fails, it writes `hkpsp-support.zip` with its logs; attach that
file when you report the problem.

### Update

Back up the `.sav` and `.sav.bak` files in `PSP/GAME/HKPSP/`, then replace
`EBOOT.PBP` and the `data/` folder. Your four save slots and `system.sav`
settings stay. If a new EBOOT needs newer data, it says so at start-up; run
the builder of the new release again and copy the new `data/` folder.

Do not share the generated `data/` folder or the package: it is converted from
the original game.

## Controls

| Input | Action |
| --- | --- |
| Analog nub / D-Pad | Move and aim |
| Cross | Jump |
| Square | Nail attack |
| Circle | Tap to cast; hold to Focus |
| Triangle | Dream Nail |
| R | Dash |
| L | Crystal Heart / Super Dash |
| Select (tap) | Inventory |
| Select (hold) | Quick Map |
| Start | Pause |

In menus Circle confirms and Cross cancels.

## Build from source

The release builder above uses a prebuilt EBOOT. To build the EBOOT yourself:

1. Install WSL2 with Ubuntu 24.04 and the
   [pspdev](https://github.com/pspdev/pspdev) toolchain (tested with PSP GCC
   15.2.0) in `~/pspdev`.
2. From the repository root run:

   ```powershell
   wsl -d Ubuntu-24.04 -- bash tools/psp/wsl-make.sh psp
   ```

   This writes `psp/EBOOT.PBP`.

The game data is converted by `tools/psp/build-steam.ps1` (PowerShell 7,
Python 3.12 with NumPy, Pillow, UnityPy and soundfile, AssetRipper, ILSpy,
FreeType, ffmpeg and devkitPro `tex3ds`). The release archive bundles all of
these tools; `tools/psp/make_builder_bundle.py` assembles such an archive.

## Layout

| Path | Purpose |
| --- | --- |
| `src/`, `include/` | Platform-independent game and simulation code |
| `psp/` | PSP frontend: renderer, audio, input, build definition |
| `tools/` | Converters that turn the original game files into port data |
| `tools/psp/` | PSP conversion, staging, packaging and the release builder |

## Reporting issues

Include the release version, PSP model or PPSSPP version, the room, steps to
reproduce and what you expected. Attach screenshots or video for rendering
problems and the files from `PSP/GAME/HKPSP/out/` if there are any. Do not
attach original game files, converted data or saves with personal information.

## License

The HKPSP code and tools are licensed under the [MIT License](LICENSE).
Third-party tools in the release archives keep their own licenses, listed in
the archive. Hollow Knight and its assets are not covered by this license.

## Credits

Hollow Knight and its original assets belong to Team Cherry. This is an
independent fan project, not affiliated with or endorsed by Team Cherry.
