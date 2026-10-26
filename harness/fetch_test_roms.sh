#!/bin/sh
# Fetch the free, MIT-licensed acid2 PPU test ROMs (Matt Currie) and their
# reference images, pinned by SHA-256, and extract the freely distributable
# Blargg CPU test ROMs that Walnut-CGB vendors as C arrays. Test use only;
# nothing here is packaged into the TV app.
set -eu
cd "$(dirname "$0")"
OUT=out/roms; mkdir -p "$OUT"
fetch() { # url file sha256
    if [ ! -f "$OUT/$2" ] || ! echo "$3  $OUT/$2" | sha256sum -c --status; then
        curl -sSLf -o "$OUT/$2.part" "$1"; mv "$OUT/$2.part" "$OUT/$2"
    fi
    echo "$3  $OUT/$2" | sha256sum -c --quiet
}
fetch https://github.com/mattcurrie/cgb-acid2/releases/download/v1.1/cgb-acid2.gbc cgb-acid2.gbc 197fb0bcec544f0400527fc707e0a94f55435974986e6986b424ace5de81720e
fetch https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb dmg-acid2.gb 464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1
fetch https://raw.githubusercontent.com/mattcurrie/cgb-acid2/master/img/reference.png cgb-acid2-reference.png 9ea9c262c5383353e77d715d021a0f7c5ccbe438f88082cb225756e50c4fdf01
fetch https://raw.githubusercontent.com/mattcurrie/dmg-acid2/master/img/reference-dmg.png dmg-acid2-reference.png ca966d50895c7efef05838590d148c2cbfd7fba57dab986f25b35b4da71abb57
python3 -I - "$OUT" <<'PY'
import re, sys, pathlib
out = pathlib.Path(sys.argv[1])
for name in ("cpu_instrs", "instr_timing"):
    src = pathlib.Path("../vendor/Walnut-CGB/test/%s.h" % name).read_text()
    body = src[src.index("{") + 1: src.index("}")]
    data = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", body))
    (out / ("%s.gb" % name)).write_bytes(data)
    print(name, len(data))
PY
