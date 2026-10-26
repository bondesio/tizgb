#!/usr/bin/env python3
"""Procedural pixel-art icon for TizGB, in the style of the TizNES icon:
dark navy background, gold/cream sparkle burst behind the device, blocky
pixel art. Drawn on a 64x64 grid and upscaled 4x with nearest-neighbour to a
crisp 256x256 RGB PNG (the size/mode TizNES uses for both its TPK and WGT).

usage: make_icon.py <out.png> [<more-out.png> ...]
"""
import math
import sys
from PIL import Image

N = 64
NAVY = (0, 29, 73)
NAVY_HI = (6, 42, 96)
GOLD = (252, 204, 103)
CREAM = (255, 236, 176)
AMBER = (232, 160, 56)
CYAN = (88, 176, 214)
OUTLINE = (30, 36, 45)
BODY = (196, 196, 188)
BODY_HI = (222, 222, 214)
BODY_SH = (150, 150, 146)
BEZEL = (84, 86, 104)
BEZEL_SH = (64, 66, 84)
SCR0 = (155, 188, 15)
SCR1 = (139, 172, 15)
SCR2 = (48, 98, 48)
SCR3 = (15, 56, 15)
BLACK = (28, 30, 36)
RED = (210, 40, 64)
RED_HI = (244, 96, 112)
RED_SH = (140, 20, 44)
LED = (236, 48, 48)
GREY_BTN = (110, 112, 122)

img = Image.new("RGB", (N, N), NAVY)
px = img.load()


def put(x, y, c):
    if 0 <= x < N and 0 <= y < N:
        px[x, y] = c


def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(x, y, c)


# --- sparkle burst rising from behind the top of the device (as TizNES) ---
CX, CY = 32, 15
for k in range(13):                         # upper half-fan of rays
    a = math.pi + k * math.pi / 12
    length = 15 if k % 2 == 0 else 11
    for r in range(8, length):
        x = int(round(CX + math.cos(a) * r * 1.25))
        y = int(round(CY + math.sin(a) * r * 0.85))
        put(x, y, CREAM if r < 10 else (GOLD if (r + k) % 3 else AMBER))
for y in range(CY - 7, CY + 1):             # glowing core
    for x in range(CX - 10, CX + 11):
        d = ((x - CX) / 10.0) ** 2 + ((y - CY) / 7.0) ** 2
        if d <= 1.0:
            put(x, y, CREAM if d < 0.35 else (GOLD if d < 0.7 else AMBER))
for i, y in enumerate((CY - 1, CY - 3)):    # horizontal light bands
    for x in range(CX - 14 + 3 * i, CX + 15 - 3 * i):
        put(x, y, GOLD if px[x, y] == NAVY else px[x, y])
# rays escaping beside the device lower down, so the burst reads as behind it
for side in (-1, 1):
    for k in range(4):
        a = math.radians(200 + 18 * k) if side < 0 else math.radians(340 - 18 * k)
        for r in range(17, 22 - k):
            put(int(round(CX + math.cos(a) * r * 1.25)), int(round(CY + 12 + math.sin(a) * r)), GOLD if r % 2 else AMBER)
# 4-point twinkles and specks, as on the TizNES icon
for (x, y, c) in [(8, 9, GOLD), (55, 11, GOLD), (5, 30, CREAM), (58, 36, GOLD), (10, 52, CYAN), (54, 55, CYAN)]:
    put(x, y, c); put(x - 1, y, c); put(x + 1, y, c); put(x, y - 1, c); put(x, y + 1, c)
for (x, y, c) in [(15, 4, CYAN), (48, 3, CREAM), (3, 18, CYAN), (61, 22, CYAN), (2, 44, GOLD),
                  (61, 47, CREAM), (20, 60, GOLD), (45, 61, GOLD), (27, 2, GOLD), (38, 6, CYAN)]:
    put(x, y, c)

# --- device body: 34 x 52, bottom-right corner rounded more (DMG shape) ---
X0, Y0, X1, Y1 = 17, 15, 46, 61
for y in range(Y0, Y1 + 1):
    for x in range(X0, X1 + 1):
        inset = 0
        if y == Y0 or y == Y1:
            inset = 1
        if y >= Y1 - 5 and x >= X1 - 5:      # big rounded corner
            dx, dy = x - (X1 - 6), y - (Y1 - 6)
            if dx * dx + dy * dy > 36:
                continue
        if (x == X0 or x == X1) and (y == Y0 or y == Y1):
            continue
        if inset and (x == X0 + 0 or x == X1):
            continue
        put(x, y, BODY)
# outline: any body pixel touching navy/burst gets outlined outside
body = {(x, y) for y in range(N) for x in range(N) if px[x, y] == BODY}
for (x, y) in list(body):
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        if (x + dx, y + dy) not in body:
            put(x + dx, y + dy, OUTLINE)
# drop shadow down-right
for (x, y) in body:
    if (x + 1, y + 2) not in body and px[x + 1, y + 2] not in (OUTLINE,) and (x + 1, y + 2) not in body:
        if px[x + 1, y + 2] not in (BODY,):
            pass
# shading: highlight top-left edge, shadow bottom/right edge
for (x, y) in body:
    if (x - 1, y) not in body or (x, y - 1) not in body:
        put(x, y, BODY_HI)
    elif (x + 1, y) not in body or (x, y + 1) not in body:
        put(x, y, BODY_SH)
# the two engraved lines near the top of a DMG
for x in range(X0 + 2, X1 - 1):
    put(x, Y0 + 2, BODY_SH)
put(X0 + 2, Y0 + 3, BODY_SH); put(X1 - 2, Y0 + 3, BODY_SH)

# --- screen bezel and screen ---
BX0, BY0, BX1, BY1 = X0 + 2, Y0 + 4, X1 - 2, Y0 + 21
rect(BX0, BY0, BX1, BY1, BEZEL)
rect(BX0, BY1, BX1, BY1, BEZEL_SH)
for y in range(BY1 - 3, BY1 + 1):          # bezel's rounded lower-right corner
    for x in range(BX1 - 3, BX1 + 1):
        if (x - (BX1 - 4)) ** 2 + (y - (BY1 - 4)) ** 2 > 9:
            put(x, y, BODY)
put(BX0 + 1, BY0 + 6, LED); put(BX0 + 1, BY0 + 7, LED)
SX0, SY0, SX1, SY1 = BX0 + 4, BY0 + 2, BX1 - 4, BY1 - 3
rect(SX0, SY0, SX1, SY1, SCR0)
# little scene: sun, hills, ground, a heart
rect(SX0, SY1 - 2, SX1, SY1, SCR2)
for x in range(SX0, SX1 + 1):
    h = int(3 + 2 * math.sin((x - SX0) / 2.2))
    for y in range(SY1 - 2 - h, SY1 - 2):
        put(x, y, SCR1)
for (dx, dy) in [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)]:
    put(SX1 - 3 + dx, SY0 + 3 + dy, SCR2)
HX, HY = SX0 + 4, SY0 + 3
for (dx, dy) in [(0, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (2, 1), (3, 1), (0, 2), (1, 2), (2, 2), (1, 3)]:
    put(HX + dx, HY + dy, SCR3)

# --- controls ---
DX, DY = X0 + 6, Y0 + 29                   # D-pad centre
rect(DX - 4, DY - 1, DX + 4, DY + 1, BLACK)
rect(DX - 1, DY - 4, DX + 1, DY + 4, BLACK)
put(DX, DY, (60, 62, 70))
put(DX - 3, DY - 1, (70, 72, 82)); put(DX - 1, DY - 3, (70, 72, 82))


def button(cx, cy):
    for y in range(cy - 2, cy + 3):
        for x in range(cx - 2, cx + 3):
            if (x - cx) ** 2 + (y - cy) ** 2 <= 5:
                put(x, y, RED)
    put(cx - 1, cy - 1, RED_HI); put(cx, cy - 1, RED_HI)
    put(cx + 1, cy + 2, RED_SH); put(cx + 2, cy + 1, RED_SH)


button(X1 - 10, DY + 1)    # B
button(X1 - 4, DY - 2)     # A
# Start / Select: slanted pills
for (sx, sy) in [(X0 + 10, Y0 + 37), (X0 + 16, Y0 + 37)]:
    for i in range(4):
        put(sx + i, sy - i // 2, GREY_BTN)
        put(sx + i, sy - i // 2 + 1, OUTLINE)
# speaker grille: diagonal slots, bottom right
for i in range(4):
    for j in range(4):
        put(X1 - 9 + i * 2 + j // 2, Y1 - 3 - j + i // 3, BODY_SH)

big = img.resize((256, 256), Image.NEAREST)
for out in sys.argv[1:]:
    big.save(out, optimize=True)
    print("wrote", out)
