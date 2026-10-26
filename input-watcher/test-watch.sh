#!/bin/sh
# Host test for watch.sh. Run as: fakeroot sh input-watcher/test-watch.sh
# Builds a fake sysfs tree and real (fakeroot) char-device nodes shaped like
# the Q60A: a DualShock 4 with five nodes and no serial, an Afterglow with a
# serial, the TV remote; then drives the watcher through its scenarios.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'kill $WPID 2>/dev/null; rm -rf "$T"' EXIT
PASS=0; FAIL=0
check() { if eval "$2"; then PASS=$((PASS+1)); echo "PASS $1"; else FAIL=$((FAIL+1)); echo "FAIL $1"; fi; }
SYS=$T/sys; DEV=$T/dev; RUN=$T/run; HW=$T/hw; CONF=$T/tizgb-input.conf
mkdir -p "$SYS" "$DEV" "$RUN" "$HW"
mkinput() { # usbdir inputname keycaps
    mkdir -p "$1/x:1.0/input/$2/capabilities"; echo "$3" > "$1/x:1.0/input/$2/capabilities/key"
}
link() { mkdir -p "$SYS/$1"; ln -s "$2" "$SYS/$1/device"; }
node() { mknod "$DEV/$1" c 13 "$2"; chown root:input "$DEV/$1"; chmod 660 "$DEV/$1"; }
usb() { mkdir -p "$1"; echo "$2" > "$1/idVendor"; echo "$3" > "$1/idProduct"; [ -z "${4:-}" ] || echo "$4" > "$1/serial"; }
usb "$HW/u6/6-1" 054c 09cc
mkinput "$HW/u6/6-1" input21 "7fff000000000000 0 0 0 0"
mkinput "$HW/u6/6-1" input18 "2420 0 0 0 0 0"
mkinput "$HW/u6/6-1" input20 "0"
usb "$HW/u1/1-1" 0e6f 0188 0000001D045B572C
mkinput "$HW/u1/1-1" input17 "ffff0000 0 0 0 0"
mkdir -p "$HW/virtual/input2/capabilities"; echo ffffffff > "$HW/virtual/input2/capabilities/key"
link js0 "$HW/u6/6-1/x:1.0/input/input20"; link event20 "$HW/u6/6-1/x:1.0/input/input20"; node event20 84
link js1 "$HW/u6/6-1/x:1.0/input/input21"; link event21 "$HW/u6/6-1/x:1.0/input/input21"; node event21 85
link event18 "$HW/u6/6-1/x:1.0/input/input18"; node event18 82
link js2 "$HW/u1/1-1/x:1.0/input/input17"; link event17 "$HW/u1/1-1/x:1.0/input/input17"; node event17 81
link event2 "$HW/virtual/input2"; node event2 66
ME=$(id -un 2>/dev/null || echo root)
export TIZGB_WATCH_CONFIG=$CONF TIZGB_WATCH_RUN=$RUN TIZGB_WATCH_SYS=$SYS TIZGB_WATCH_DEV=$DEV \
       TIZGB_WATCH_GRANT="$ME:users" TIZGB_WATCH_STOCK=root:input TIZGB_WATCH_POLL=0.1
mode() { stat -c '%U:%G:%a' "$DEV/$1"; }
G="$ME:users:600"; S="root:input:660"
start() { cat "$HERE/watch.sh" | ${WATCH_SHELL:-sh} & WPID=$!; sleep 0.5; }
settle() { sleep 0.5; }
start
check "no config: nothing granted" '[ "$(mode event21)" = "$S" ] && [ "$(mode event17)" = "$S" ]'
printf 'exclusive=1\nidentity=054c:09cc::6-1\n' > "$CONF"; settle
check "DS4 (no serial): controller node event21 granted" '[ "$(mode event21)" = "$G" ]'
check "DS4 motion/touchpad and remote untouched" '[ "$(mode event20)" = "$S" ] && [ "$(mode event18)" = "$S" ] && [ "$(mode event2)" = "$S" ]'
check "state file records event21" '[ "$(cat $RUN/tizgb-input-access.event)" = "$DEV/event21" ]'
printf 'exclusive=0\nidentity=054c:09cc::6-1\n' > "$CONF"; settle
check "exclusive=0 restores event21" '[ "$(mode event21)" = "$S" ] && [ ! -e $RUN/tizgb-input-access.event ]'
printf 'exclusive=1\nidentity=0e6f:0188:0000001D045B572C:1-1\n' > "$CONF"; settle
check "Afterglow selected: event17 granted, event21 stock" '[ "$(mode event17)" = "$G" ] && [ "$(mode event21)" = "$S" ]'
printf 'exclusive=1\nidentity=0e6f:0188:0000001D045B572C:3-2\n' > "$CONF"; settle
check "Afterglow saved on another port: matched by serial" '[ "$(mode event17)" = "$G" ]'
chown root:input "$DEV/event17"; chmod 660 "$DEV/event17"; settle
check "external permission reset is re-granted" '[ "$(mode event17)" = "$G" ]'
printf '%s\n' "$DEV/event17" > "$RUN/tiznes-input-access.event"
printf 'exclusive=1\nidentity=054c:09cc::6-1\n' > "$CONF"; settle
check "node held by TizNES's watcher is kept, not restored" '[ "$(mode event17)" = "$G" ] && grep -q "keep $DEV/event17" $RUN/tizgb-input-access.log'
check "and the new selection is granted" '[ "$(mode event21)" = "$G" ]'
rm -f "$RUN/tiznes-input-access.event"; chown root:input "$DEV/event17"; chmod 660 "$DEV/event17"
printf 'exclusive=1\nidentity=054c:09cc::9-9\n' > "$CONF"; settle
check "DS4 identity on wrong port (no serial): no loose match, event21 restored" '[ "$(mode event21)" = "$S" ]'
printf 'exclusive=1\nidentity=054c:09cc::6-1\nidentity2=0e6f:0188:0000001D045B572C:1-1\n' > "$CONF"; settle
check "two slots: both pads granted independently" '[ "$(mode event21)" = "$G" ] && [ "$(mode event17)" = "$G" ]'
printf 'exclusive=1\nidentity=bad;rm -rf /\n' > "$CONF"; settle
check "invalid identity rejected, grants released" '[ "$(mode event21)" = "$S" ] && [ "$(mode event17)" = "$S" ]'
printf 'exclusive=1\nidentity=054c:09cc::6-1\n' > "$CONF"; settle
t0=$(date +%s%N); kill -TERM $WPID; wait $WPID 2>/dev/null; t1=$(date +%s%N)
check "SIGTERM exits promptly ($(( (t1-t0)/1000000 )) ms) and restores event21" '[ $(( (t1-t0)/1000000 )) -lt 1500 ] && [ "$(mode event21)" = "$S" ]'
# stale grant from a dead instance: event21 granted + state file, config now none
chown "$ME:users" "$DEV/event21"; chmod 600 "$DEV/event21"; printf '%s\n' "$DEV/event21" > "$RUN/tizgb-input-access.event"
printf 'exclusive=1\nidentity=\n' > "$CONF"
start; settle
check "stale grant adopted at start and restored" '[ "$(mode event21)" = "$S" ] && grep -q "start cur1=$DEV/event21" $RUN/tizgb-input-access.log'
kill -TERM $WPID; wait $WPID 2>/dev/null
check "remote node never modified" '[ "$(mode event2)" = "$S" ] && [ "$(mode event20)" = "$S" ] && [ "$(mode event18)" = "$S" ]'
echo "watch.sh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
