#!/bin/sh
# Copy Game Boy / Game Boy Color ROM files you own into TizGB's library on
# the TV (app data folder), with the ownership and SMACK label the app needs,
# and verify each copy by SHA-256. Does not launch or touch anything else.
#
# usage: tools/add-rom.sh <rom.gb|rom.gbc> [...]
set -eu
D=/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data
LABEL=User::Pkg::org.pablo.tizgb
for f in "$@"; do
    case "$f" in *.gb|*.gbc|*.GB|*.GBC|*.cgb|*.CGB) ;; *) echo "skip (not .gb/.gbc): $f"; continue ;; esac
    name=$(basename "$f")
    case "$name" in *"'"*|*'\'*) echo "skip (quote in name): $name"; continue ;; esac
    ssh q60a-tv-root "test -d '$D'" || { echo "TizGB is not installed (no $D)"; exit 1; }
    ssh q60a-tv-root "cat > '$D/.incoming' && mv -f '$D/.incoming' '$D/$name' && chown owner:system_share '$D/$name' && chmod 644 '$D/$name' && chsmack -a '$LABEL' '$D/$name'" < "$f"
    local_sum=$(sha256sum "$f" | cut -d' ' -f1)
    tv_sum=$(ssh q60a-tv-root "sha256sum '$D/$name'" | cut -d' ' -f1)
    [ "$local_sum" = "$tv_sum" ] && echo "ok  $name  $local_sum" || { echo "HASH MISMATCH $name"; exit 1; }
done
echo "Open TizGB (or Settings > Rescan ROMs) to see the new games."
