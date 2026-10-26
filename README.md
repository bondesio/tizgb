# TizGB: Game Boy and Game Boy Color emulator for the Samsung Q60A

> **This is a native Tizen app, not a web app.** TizGB is a 32-bit ARM ELF written in C/C++ against
> EFL/Evas and `capi-media-audio-io`, packaged as a signed **TPK**. It is *not* an HTML5/WGT web application and
> no web engine is involved in emulation. The only WGT in this repository (`launcher/`) is a Home-rail tile that
> starts the native app.
>
> It runs on **one rooted Samsung QE75Q60AAUXXC** (Tizen 6.0, ARMv7) and depends on the research that made that possible:
>
> - [Samsung QE75Q60AAUXXC root research](https://github.com/bondesio/qe75q60aa-root-research): how the TV was rooted, how native packages are installed and how the root helper services work.
> - [Native Tizen 6 apps and UEP](https://github.com/bondesio/tizen6-native-app-uep-research): why a native process starts under User Execution Prevention but only renders its UI once UEP is disabled.
>
> This is a research record and source release, not an installer for stock TVs. No game ROMs, BIOS images, saves or
> Samsung binaries are included; link libraries are copied from your own TV at build time (see *Build*).
> Nothing here has been published as a ready-to-install package.

**Sibling projects**

- [Tizen emulator starter kit](https://github.com/bondesio/tizen-emulator-framework): Shared conventions, snippets and host-test fakes for the native Tizen TV emulators
- [TizNES](https://github.com/bondesio/tiznes): Native Tizen 6 NES emulator (InfoNES) for a rooted Samsung Q60A
- [TizGBA](https://github.com/bondesio/tizgba): Native Tizen 6 Game Boy Advance emulator (mGBA) for a rooted Samsung Q60A
- [TizNeo](https://github.com/bondesio/tizneo): Native Tizen 6 Neo Geo AES/MVS emulator (Geolith) for a rooted Samsung Q60A
- [TizPSX](https://github.com/bondesio/tizpsx): Native Tizen 6 PlayStation emulator (PCSX ReARMed) for a rooted Samsung Q60A

A native Tizen 6.0 TV app (signed TPK, 32-bit ARM ELF) that plays Game Boy
(DMG) and Game Boy Color (CGB) games on Pablo's rooted Samsung
QE75Q60AAUXXC. It is the sibling of [TizNES](https://github.com/bondesio/tiznes) and reuses
its architecture and every lesson from its debugging passes. A separate
signed WGT provides the Home-rail tile.

Emulation core: **Walnut-CGB 1.2.4** (MIT; a Game Boy Color capable
descendant of Peanut-GB) with **minigb_apu** (MIT) for sound, vendored under
`vendor/Walnut-CGB/`. `walnut_cgb.h` is unmodified; the APU carries the fixes
listed in `vendor/Walnut-CGB/PATCHES.md`. See `NOTICE.md`.

No game ROMs are included or downloaded. Put ROM files you own on a USB
pendrive (any folder layout, up to 8 folders deep) or copy them into the
app's data folder (see "Adding games").

## Status

Version 1.2.0 (2026-10-26) adds the framework's library layout (first row **Exit TizGB**, games,
Settings; the first game is selected at start) and Settings → System → **Performance overlay**
(Off / FPS / Detailed, persisted as `perf_overlay`). Settings rows are 37 px apart so the 15 rows
fit above the footer. Host harness: `make -C harness ui-tests` (98 checks). Installed on the TV
2026-10-26 (TPK `dab2d2e3…`, not launched); rollback `backups/2026-10-26-before-1.2.0-deploy/`.

Version 1.1.0 (2026-10-23): USB ROM library explorer (deep, bounded pendrive
scan; games play straight from the drive; saves stay internal, keyed by ROM
content; hot-plug). The host harnesses pass, a read-only probe of the real
ARM scanner ran on the TV against the attached pendrive, and the TPK is
installed and hash-verified. Running the app on the TV (launch, real
controller, sound by ear, how the picture looks, a real pendrive end to end)
has **not** been done yet: the TV was in use both times. Run
`tools/deferred-tv-checks.sh` when it is free.

## Layout

| Path | What |
|---|---|
| `app/gb_main.c` | EFL frontend: library screen, sectioned Settings, pacing, controller input (port of `nes_main.c` 1.0.26). |
| `app/library.c`, `app/library.h` | ROM library: Storage API drive discovery, bounded recursive scan, mount-table hot-plug check. |
| `app/platform.c` | Core port: ROM, battery saves, RTC, palettes, sub-frame-synced S16 stereo audio. |
| `app/include/` | Freestanding libc shims for the `-nostdinc` build (types + hand declarations). |
| `app/Makefile`, `app/tizen-manifest.xml`, `app/start.S` | Native TPK build (same toolchain and recipe as TizNES). |
| `launcher/` | Home-tile WGT `GBLAUNCH01` that launches `org.pablo.tizgb`. |
| `input-watcher/` | Root watcher module `tizgb-input` (`tizgb-input-access.service`) + fakeroot test. |
| `harness/` | Host test harnesses (core accuracy, audio, UI/pacing/input). |
| `tools/` | Icon generator, original APU test-ROM assembler, ROM copy and WGT-install helpers, deferred on-TV checks, read-only on-TV library probe (`library-probe.c`) and grab probe. |
| `vendor/Walnut-CGB/` | Core, APU, CGB palette table, upstream licence and test-ROM arrays. |
| `backups/` | Pre-deployment snapshots with `MANIFEST.sha256`. |

## Package / app IDs

| Package | Type | Package ID | App ID | Exec | Version |
|---|---|---|---|---|---|
| Emulator | TPK (native `capp`) | `org.pablo.tizgb` | `org.pablo.tizgb` | `tizgb` | 1.1.0 |
| Home tile | WGT | `GBLAUNCH01` | `GBLAUNCH01.Launcher` | none | 1.0.0 |
| Root module | selfroot module | `tizgb-input` | `tizgb-input-access.service` | `watch.sh` | 1.0.0 |

The WGT launches the exact app ID `org.pablo.tizgb`. Tizen web package IDs
are exactly 10 alphanumeric characters (`GBLAUNCH01`), like `NESLAUNCH1`.

## What carries over from TizNES

- **Frame pacing by the monotonic clock** at the Game Boy's 59.7275 Hz
  (70224 cycles at 4.194304 MHz), never one frame per compositor tick. The
  `pace` log line repeats every minute with `emu_fps_x100`.
- **Clean 16-bit audio**: S16LE through capi-media-audio-io (stereo, because
  Game Boy games pan their channels; mono S16 fallback), with silence
  prequeued at the target fill. Each frame yields its share of 44.1 kHz,
  corrected by at most 2 samples from the ring fill, and is DC-blocked. The
  APU is rendered up to every register access inside the frame.
- **Exclusive controller capture** with a cloned root watcher. Both the app
  and the watcher ignore joystick nodes without key capabilities (DualShock 4
  motion sensors). It has its own config, state and unit.
- **Sectioned Settings** from the start: Controller, Audio, Video, System.
- **ROM library**: `.gb`, `.gbc` (and `.cgb`) in the data folder and on
  every mounted USB drive, sorted by name. Each entry is tagged `[GB]`,
  `[GB+GBC]` (colour-enhanced, header `0x143` = `0x80`) or `[GBC]` (colour
  only, `0xC0`). CGB-flagged games run in colour mode. See "USB library".
- **Home launcher** that exits on `visibilitychange`, never on a timer.

## Settings

| Section | Row | Notes |
|---|---|---|
| Controller | Controller | Pick the USB pad (identity vid:pid:serial:port). |
| | Exclusive capture | On: the watcher grants the pad's event node and TizGB grabs it. |
| | Map controller buttons | D-pad, A, B, Select, Start; saved per controller model (`tizgb-map-<vid>-<pid>.bin`). |
| | Guided gamepad input test | Logs raw codes to `tizgb-gamepad-test.log`. |
| Audio | Volume, Mute | Saved. |
| | Audio channel test | Leave out Pulse 1, Pulse 2, Wave or Noise to compare sounds (not saved). |
| Video | Screen size | Pixel-perfect 7x (1120x1008, default), fit height (1200x1080), stretch 4:3. |
| | Scaling filter | Sharp (nearest, default) or smooth. |
| | Original Game Boy palette | For monochrome games: classic green, Pocket grey, auto colour by title (GBC-style, default), or any of the 12 GBC boot palettes. |
| | Game Boy Color colours | GBC screen (colour-corrected, default) or vivid (raw). |
| System | Rescan ROMs / Back to library / Exit TizGB | Rescan re-runs the full scan (data folder + every drive the Storage API reports now). Exit saves, releases the pad and quits (with an `_exit` fallback). |

All rows except the audio channel test are saved in `tizgb-settings.conf`.

## USB library

- **Drives**: whatever the Tizen Storage API
  (`storage_foreach_device_supported`) reports as external and mounted
  (read-write or read-only), at the mount path it reports. On this TV that
  was `/opt/media/USBDriveA1` (storage id 11, vfat) in the 2026-10-23 probe;
  nothing in the code assumes that path. Only if the API call fails are the
  folders inside `/opt/media` and `/run/media/owner` used (logged).
- **Depth and limits**: up to 8 folders below a drive (or the data folder).
  Typical collections nest 4-6 deep; deeper trees on a pendrive are almost
  always copied system trees, backups or filesystem loops. Symlinks are
  never followed, every folder's (device, inode) is visited once (a drive
  reported twice is scanned once), and each scan stops at 4096 folders,
  200000 directory entries, 2048 games or 10 s, keeping what it found and
  saying which limit was hit. Hidden entries (macOS `._*` files,
  `.Spotlight-V100`, `.Trashes`), `System Volume Information`,
  `$RECYCLE.BIN`, `RECYCLER`, `LOST.DIR` and the app's own `saves` folder
  are skipped.
- **Screen**: USB rows show the drive name, e.g. `[GBC]  Game.gbc  (USBDriveA1)`;
  the footer shows the selected game's folder. Left/Right (or Channel
  Up/Down) page by 14 rows.
- **Playing from the drive**: the game file is read completely into memory
  from its path on the drive; nothing is copied to the TV. Pulling the
  drive during play does not affect the running game; its save is still
  written to internal storage.
- **Hot-plug**: the mount table (`/proc/self/mounts`) is checked every ~2 s;
  when it changes and the Storage API's set of drives (or the drive on a
  mount point) differs, the library rescans as soon as it is on screen
  (deferred during a game or Settings), keeping the cursor on the same game.
  Starting a game whose drive has gone shows "The USB drive with this game
  is no longer connected." and the library rescans when you return.
- Scans start from the animator after "Scanning..." is drawn, including the
  first one at start-up.

## Saves

Battery-backed cartridge RAM goes in `data/saves/<key>.sav` and MBC3 clocks
in `<key>.rtc`, always on the TV's internal storage, never on the drive. The
key is the ROM header title plus the 64-bit FNV-1a hash of the whole ROM
file, e.g. `POKEMON_YELLOW-0123456789abcdef`. So two different games that
share a file name (`game.gbc` on two pendrives) or even a header title get
separate saves, while the same ROM file keeps its save wherever it is found
(another pendrive, renamed, a different USB port and therefore mount path).
A file-name key would collide; a path key would lose saves whenever a drive
mounts under another name. A patched or re-dumped ROM is different content
and starts a new save. If no internal save exists yet, a `<rom name>.sav`
beside the ROM (as other emulators leave them) is imported read-only and
copied to internal storage on the first automatic save. Saves are written
atomically about 2 s after the game stops writing, when you leave the game
with Back, when the app goes to the background and on exit. MBC3 clocks
advance by the time spent away. Save states are not implemented.

## Adding games

```sh
tools/add-rom.sh "/path/to/My Game (USA).gbc"   # copies, sets owner/SMACK label, checks SHA-256
```

Files go in `/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data/`
(`owner:system_share 0644`, SMACK `User::Pkg::org.pablo.tizgb`), in any
subfolders up to 8 deep. Or simply plug in a pendrive with the games (see
"USB library"). Use Settings > Rescan ROMs after copying with `add-rom.sh`
while the app is open.

## Build

```sh
make -C app            # native TPK (fetches TV link libraries on first build)
make -C launcher       # Home-tile WGT
python3 tools/make_icon.py app/shared/res/icon.png launcher/icon.png
```

Outputs: `app/out/org.pablo.tizgb-1.2.0-arm.tpk`,
`launcher/out/GBLAUNCH01-1.0.0.wgt`. After a TV firmware change, run
`make -C app refresh-tv-libs` and rebuild.

## Tests (host, x86-64)

```sh
make -C harness core-tests   # Blargg cpu_instrs + instr_timing, dmg-acid2, cgb-acid2 (both dispatch paths)
make -C harness apu-tests    # envelope fade, sweep, wave level, noise, mid-frame timing
make -C harness ui-tests     # 85 checks: library, settings, pacing, audio ring, DS4/Afterglow input, saves, exit,
                             # and the USB library (fake Storage API + mount table: depth, symlink loops, dedupe,
                             # hot-plug, removal during play / before launch, save-key collisions, budgets, fallback)
fakeroot sh input-watcher/test-watch.sh   # 16 watcher scenarios
```

On the TV, read-only (no launch, nothing on screen), the real ARM scanner:

```sh
make -C app library-probe    # app/bin/library-probe: library.c with a /tmp test tree as its internal root
```

The harnesses compile the real `app/*.c` freestanding against `app/include/`
exactly as for the TV. Only the system edges are replaced: EFL/audio stubs,
a fake clock, and fake input nodes. Test ROMs are the free acid2 ROMs (MIT,
fetched and pinned by SHA-256), the Blargg tests that Walnut-CGB vendors, and
original ROMs from `tools/make_test_rom.py`.

## Install (TV)

Back up first (`backups/<date>-before-<version>-deploy/` with
`MANIFEST.sha256`). Stream the files, because scp is unavailable:

```sh
ssh q60a-tv-root 'cat > /tmp/tizgb.tpk' < app/out/org.pablo.tizgb-1.2.0-arm.tpk
ssh q60a-tv-root 'sha256sum /tmp/tizgb.tpk && pkgcmd -i -t tpk -p /tmp/tizgb.tpk && pkginfo --app org.pablo.tizgb'
# launcher WGT: install launcher/out/GBLAUNCH01-1.0.0.wgt with Tizen Studio's `tizen install` over SDB (see the root research)
# root module: copy input-watcher/{watch.sh,boot.sh,module.json} to
# /opt/usr/share/selfroot/modules/tizgb-input/, then run boot.sh the runner's way:
ssh q60a-tv-root 'cd /opt/usr/share/selfroot/modules/tizgb-input && cat boot.sh | MODULE_DIR=$PWD bash'
```

The TizGB watcher serializes permission and state changes with the shared
`/run/tizen-input-access.lock` flock used by TizNES and TizNeo. TizGB and TizNeo
yield to TizNES; either TizGB or TizNeo retains a grant held by the other.
When multiple emulator settings select the same controller, one emulator
should be active at a time.

The Home tile appears under **Apps > Config.** as **TizGB**. Choose
**Añadir a la página de inicio** to pin it.

## Rollback / uninstall

```sh
ssh q60a-tv-root 'pkgcmd -u -n org.pablo.tizgb; pkgcmd -u -n GBLAUNCH01'
ssh q60a-tv-root 'touch /opt/usr/share/selfroot/modules/tizgb-input/disabled; systemctl stop tizgb-input-access.service'
# (stopping the watcher restores any node it granted; delete the module directory to remove it)
```

## License

The code written for this project is released under the Unlicense (see `LICENSE`). Vendored third-party code keeps its own license; see `NOTICE.md` and `vendor/`.
