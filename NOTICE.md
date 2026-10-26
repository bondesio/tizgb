# Third-party notices

TizGB's own code (`app/`, `launcher/`, `input-watcher/`, `harness/`, `tools/`)
is Pablo's project code. The frontend and watcher are derived from TizNES and
its root watcher in this workspace.

## Walnut-CGB (MIT)
`vendor/Walnut-CGB/walnut_cgb.h`, `extras/cgb.h`, `test/*`, version 1.2.4,
upstream https://github.com/Mr-PauI/Walnut-CGB commit
a42c186917516cc9c80515ceae1121600a071628. Copyright (c) 2025-2026 Mr. Paul
(Walnut-CGB additions); Copyright (c) 2018-2023 Mahyar Koshkouei (Peanut-GB);
parts taken from SameBoy, Copyright (c) 2015-2019 Lior Halphon (MIT). Licence
text: `vendor/Walnut-CGB/LICENSE` and the header of `walnut_cgb.h`.

## minigb_apu (MIT)
`vendor/Walnut-CGB/minigb_apu/`, Copyright (c) 2017 Alex Baines, Copyright
(c) 2019 Mahyar Koshkouei; based on MiniGBS. Licence:
`vendor/Walnut-CGB/minigb_apu/LICENSE`. Modified by TizGB, see
`vendor/Walnut-CGB/PATCHES.md`.

## Palette data
The per-title GBC-style palette table in `app/platform.c` comes from the
Peanut-GB SDL example (MIT, Mahyar Koshkouei). The 12 GBC boot palettes come
from Walnut-CGB `extras/cgb.h` (MIT).

## Test ROMs (host harness only, never packaged)
- Blargg's `cpu_instrs` and `instr_timing` as vendored by Walnut-CGB (freely
  distributable test ROMs).
- dmg-acid2 and cgb-acid2 by Matt Currie (MIT), fetched by
  `harness/fetch_test_roms.sh` and pinned by SHA-256.
- `tools/make_test_rom.py` assembles original APU and save test ROMs.

No commercial game ROM, boot ROM or other copyrighted Nintendo code or data is
included, downloaded or used.
