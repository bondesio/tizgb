#!/bin/sh
# On-TV functional checks for TizGB that were deferred on 2026-10-23 because
# the TV was in use (1.0.0 install, then the 1.1.0 USB library explorer).
# RUN ONLY WHEN THE TV IS FREE: step 1 launches TizGB on screen via its Home
# launcher (same path as pressing the tile).
# Needs a USB controller selected in TizGB Settings for steps 3-4.
#
# usage: tools/deferred-tv-checks.sh            steps 0-6
#        tools/deferred-tv-checks.sh --usb-log  only re-read the library/USB log
#                                               lines (after the hands-on
#                                               pendrive steps printed in 6)
set -u
cd "$(dirname "$0")/.."
D=/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data

usb_log() {
    echo "== USB library log lines (storage API roots, scans, hot-plug, launches, saves)"
    ssh q60a-tv-root "grep -E '^(storage |library|rom-start|rom-load|save-key|save-import|save-load|save )' $D/tizgb-runtime.log"
    echo "== internal saves (content-keyed names <TITLE>-<16 hex>.sav/.rtc)"
    ssh q60a-tv-root "ls -la $D/saves 2>/dev/null || echo '(no saves yet)'"
}
if [ "${1:-}" = "--usb-log" ]; then usb_log; exit 0; fi

make -s -C app probe
echo "== 0. UEP must be disabled (uep_toggle loaded) for native UI to render"
ssh q60a-tv-root 'grep -q "^uep_toggle " /proc/modules && echo uep_toggle loaded || echo "UEP NOT DISABLED: reboot and wait ~75 s"'
echo "== 0b. mounted USB drives right now (the app must find the same paths via the Storage API)"
ssh q60a-tv-root 'grep -E " /opt/media/| /run/media/" /proc/mounts || echo "(no USB drive mounted)"'
ssh q60a-tv-root 'touch /tmp/tizgb-launch-stamp'
echo "== 1. launch through the Home launcher (cold start)"
ssh q60a-tv-root 'app_launcher -s GBLAUNCH01.Launcher'
sleep 12
ssh q60a-tv-root 'app_launcher -S | grep -E "tizgb|GBLAUNCH01" || echo "(not listed)"; pgrep -f /opt/usr/apps/org.pablo.tizgb/bin/tizgb >/dev/null && echo "tizgb process running"'
echo "== 2. runtime log (expect app-start, storage ... use=1 path=<USB mount>, library-root n=1 external=1,"
echo "      library roms=N ... external_roots>=1 when a pendrive is in, input lines, no errors)"
ssh q60a-tv-root "cat $D/tizgb-runtime.log"
echo "== 3. watcher grant and app grab"
ssh q60a-tv-root "cat $D/tizgb-input.conf 2>/dev/null; cat /run/tizgb-input-access.event 2>/dev/null; tail -5 /run/tizgb-input-access.log"
NODE=$(ssh q60a-tv-root 'cat /run/tizgb-input-access.event 2>/dev/null')
if [ -n "$NODE" ]; then
    echo "== 4. second EVIOCGRAB on $NODE must fail with EBUSY while TizGB runs"
    ssh q60a-tv-root 'cat > /tmp/grab-probe && chmod 755 /tmp/grab-probe' < app/bin/grab-probe
    ssh q60a-tv-root "/tmp/grab-probe $NODE; rm -f /tmp/grab-probe"
    ssh q60a-tv-root "ls -l $NODE /dev/input/event2"
else
    echo "== 4. skipped: no controller selected yet (Settings > Controller in TizGB)"
fi
echo "== 5. USB library: the app (owner + SMACK, externalstorage privilege) could read the drive?"
ssh q60a-tv-root "grep -E 'library-root-unreadable|library-read-error' $D/tizgb-runtime.log && echo 'APP COULD NOT READ A ROOT (see above)' || echo 'no unreadable roots'"
ssh q60a-tv-root "for m in \$(grep -E ' /opt/media/| /run/media/' /proc/mounts | cut -d' ' -f2); do echo \"files written on \$m since launch (must be none):\"; find \"\$m\" -newer /tmp/tizgb-launch-stamp -type f 2>/dev/null | head; done"
usb_log
cat <<'EOF'
== 6. hands-on pendrive steps (TV remote), then run: tools/deferred-tv-checks.sh --usb-log
   a. Put a .gb/.gbc you own in a nested folder on the pendrive (e.g. ROMs/Nintendo/GBC/Europe/x.gbc).
   b. With the library open, plug the pendrive in: within ~2 s plus mount time it shows "Scanning..."
      and the game appears with "(USBDriveA1)" or the drive's name; the footer shows its folder.
      Log: library-drives-changed, library-rescan reason=drives-changed.
   c. Play it (rom-start path=/opt/media/... external=1), save in game, Back: saves/<TITLE>-<hash>.sav.
   d. Start it again and pull the pendrive while playing: the game keeps running; Back returns to
      the library, which rescans without the drive's games.
   e. Plug it back, select the game, pull the drive and press OK within ~2 s: the error
      "The USB drive with this game is no longer connected." then any key returns to the library.
   f. Settings > Rescan ROMs reports "ROM scan complete: N found (k on m USB drive(s), ...)".
== 7. leave TizGB at its library screen for the hands-on test
EOF
