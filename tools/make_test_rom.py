#!/usr/bin/env python3
"""Assemble tiny original Game Boy test ROMs (no third-party code or data)
that program the APU directly, for the host audio harness.

Each ROM: header with a valid checksum, entry at 0x100 jumping to 0x150, a
sequence of APU register writes, then an endless loop. 'wait' steps burn
time with a counted loop so later writes land mid-way through playback.

usage: make_test_rom.py <out-dir>
"""
import sys, pathlib

def ldh(reg, val):            # LD A,val ; LDH (reg),A
    return bytes([0x3E, val, 0xE0, reg])

def wait_frames(n):           # burn ~n frames of CPU time
    # LD BC,nn ; loop: DEC BC ; LD A,B ; OR C ; JR NZ,loop -> 28 cycles/iter
    # (+12 setup), split into blocks of at most 0xFFFF iterations.
    total = int(n * 70224 / 28)
    code = b""
    while total > 0:
        count = min(0xFFFF, total)
        code += bytes([0x01, count & 0xFF, count >> 8, 0x0B, 0x78, 0xB1, 0x20, 0xFB])
        total -= count
    return code

def rom(title, body, cgb=0x00, cart=0x00, ram=0x00):
    r = bytearray(0x8000)
    r[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])          # nop; jp $0150
    t = title.encode()[:15]
    r[0x134:0x134 + len(t)] = t
    r[0x143] = cgb
    r[0x147] = cart; r[0x148] = 0x00; r[0x149] = ram
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - r[i] - 1) & 0xFF
    r[0x14D] = x
    code = body + bytes([0x18, 0xFE])                          # jr -2
    r[0x150:0x150 + len(code)] = code
    return bytes(r)

POWER = ldh(0x26, 0x80) + ldh(0x24, 0x77) + ldh(0x25, 0xFF)
roms = {
    # Pulse 1, volume 15, decreasing envelope pace 3: hardware fades to 0
    # in 15 x 3/64 s = 0.703 s. 256 Hz, 50 % duty, no sweep.
    "apu-fade.gb": POWER + ldh(0x10, 0x00) + ldh(0x11, 0x80) + ldh(0x12, 0xF3)
                   + ldh(0x13, 0x00) + ldh(0x14, 0x86),
    # "Jump" style effect: rising sweep (pace 1, shift 5) plus a fast fade
    # (pace 1: 15/64 s = 0.234 s).
    "apu-jump.gb": POWER + ldh(0x10, 0x15) + ldh(0x11, 0x80) + ldh(0x12, 0xF1)
                   + ldh(0x13, 0x00) + ldh(0x14, 0x85),
    # Sweep pace 7 with shift 0 on a 102 Hz note (freq 768): on hardware
    # 768 + 768 <= 2047, so the note keeps playing (upstream minigb_apu
    # silenced every shift-0 sweep at once). apu-sweep0-ovf uses freq 1536,
    # which the hardware does silence after the first sweep period (55 ms).
    "apu-sweep0.gb": POWER + ldh(0x10, 0x70) + ldh(0x11, 0x80) + ldh(0x12, 0xF0)
                     + ldh(0x13, 0x00) + ldh(0x14, 0x83),
    "apu-sweep0-ovf.gb": POWER + ldh(0x10, 0x70) + ldh(0x11, 0x80) + ldh(0x12, 0xF0)
                     + ldh(0x13, 0x00) + ldh(0x14, 0x86),
    # Pulse 2 sustained, then retriggered quieter mid-playback ~30 frames in:
    # the level change must appear ~0.5 s in, not at a frame-quantized time.
    # 2048 Hz so the change point can be timed to ~1 ms; the wait ends half
    # way through frame 30, so a frame-quantized APU would move it ~8 ms.
    "apu-midframe.gb": POWER + ldh(0x16, 0x80) + ldh(0x17, 0xF0) + ldh(0x18, 0xC0)
                       + ldh(0x19, 0x87) + wait_frames(30.5) + ldh(0x17, 0x70) + ldh(0x19, 0x87),
    # Wave channel (square-shaped pattern) at 100 % then 50 % level
    # (NR32 0x20 then 0x40): the second half must be about half as loud.
    "apu-wave.gb": POWER + bytes().join(ldh(0x30 + i, 0xFF if i < 8 else 0x00) for i in range(16))
                   + ldh(0x1A, 0x80) + ldh(0x1C, 0x20) + ldh(0x1D, 0x00) + ldh(0x1E, 0x86)
                   + wait_frames(30) + ldh(0x1C, 0x40),
    # Noise with decreasing envelope pace 2 (0.47 s fade).
    "apu-noise.gb": POWER + ldh(0x21, 0xF2) + ldh(0x22, 0x35) + ldh(0x23, 0x80),
}
# Battery-save check: MBC1+RAM+BATTERY, 8 KiB RAM. Enables cart RAM,
# increments the byte at $A000 once per boot, then idles. Each load of the
# ROM must see the value the previous session saved.
SAVE_COUNTER = (bytes([0x3E, 0x0A, 0xEA, 0x00, 0x00])      # ld a,$0A ; ld ($0000),a
                + bytes([0xFA, 0x00, 0xA0, 0x3C, 0xEA, 0x00, 0xA0]))  # ld a,($A000) ; inc a ; ld ($A000),a
out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
for name, data in roms.items():
    (out / name).write_bytes(rom(name.split(".")[0].upper(), data))
    print("wrote", out / name)
(out / "save-counter.gb").write_bytes(rom("SAVE COUNTER", SAVE_COUNTER, cart=0x03, ram=0x02))
print("wrote", out / "save-counter.gb")
