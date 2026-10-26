#!/bin/sh
# Root watcher: grant TizGB exclusive access to the selected physical USB
# controller named in TizGB app data ("identity=", plus an optional
# "identity2=" second slot kept from the TizNES design; TizGB 1.0 writes only
# the first). No config is sourced or evaluated; values are only compared
# with sysfs attributes. Only an event node whose sysfs input parent is a
# button-capable joystick of a matching USB device can be changed.
#
# Clone of tizen-tv/tiznes-input-watcher/watch.sh (TizNES 1.0.26) with every
# fix kept: button-capability filtering (DualShock 4 motion-sensor node),
# SIGTERM exits instead of re-entering the loop, stale-grant adoption from a
# previous instance, serial-or-port identity matching, major-only char-device
# check. Independent of TizNES: own config, state, log and systemd unit, and
# it never grants/restores a node another emulator watcher records as held.
#
# Runs as `cat watch.sh | /bin/sh` (bash 3.2 on the Q60A): the script itself
# arrives on stdin, so nothing here may read stdin.
#
# The TIZGB_WATCH_* variables exist only for the host test harness
# (input-watcher/test-watch.sh under fakeroot); the systemd unit sets none.
set -u
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
CONFIG=${TIZGB_WATCH_CONFIG:-/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data/tizgb-input.conf}
RUN=${TIZGB_WATCH_RUN:-/run}
SYSIN=${TIZGB_WATCH_SYS:-/sys/class/input}
DEVIN=${TIZGB_WATCH_DEV:-/dev/input}
GRANT=${TIZGB_WATCH_GRANT:-owner:users}
STOCK=${TIZGB_WATCH_STOCK:-root:input}
STATE1=$RUN/tizgb-input-access.event
STATE2=$RUN/tizgb-input-access.event2
LOG=$RUN/tizgb-input-access.log
LOCK=$RUN/tizen-input-access.lock
exec 9>"$LOCK"
LOCKED=0
# Other emulator watcher state (read only): nodes they currently hold.
FOREIGN1=$RUN/tiznes-input-access.event
FOREIGN2=$RUN/tiznes-input-access.event2
FOREIGN3=$RUN/tizneo-input-access.event
FOREIGN4=$RUN/tizneo-input-access.event2
POLL=${TIZGB_WATCH_POLL:-1}
CUR1=
CUR2=

lock_access() { /usr/bin/flock -x 9 || return 1; LOCKED=1; }
unlock_access() { if [ "$LOCKED" = 1 ]; then LOCKED=0; /usr/bin/flock -u 9; fi; }
record_state() {
    record_tmp=$1.tmp.$$
    if ! printf '%s\n' "$2" > "$record_tmp" || ! mv -f "$record_tmp" "$1"; then
        rm -f "$record_tmp"
        return 1
    fi
    return 0
}

log() {
    if [ -f "$LOG" ] && [ "$(stat -c %s "$LOG" 2>/dev/null || echo 0)" -gt 65536 ]; then
        mv -f "$LOG" "$LOG.1"
    fi
    printf '%s %s\n' "$(date '+%F %T' 2>/dev/null)" "$*" >> "$LOG"
}

valid_node() {
    case "$1" in
        "$DEVIN"/event[0-9]|"$DEVIN"/event[0-9][0-9]|"$DEVIN"/event[0-9][0-9][0-9]) return 0 ;;
    esac
    return 1
}

# Equality-compared only; ':' separates vid:pid:serial:port. '.' allows ports
# behind a hub (e.g. 1-1.2); letters allow non-hex USB serials.
valid_identity() {
    case "$1" in
        ''|*[!0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ:.-]*) return 1 ;;
    esac
    return 0
}

foreign_holds() {
    for f in "$FOREIGN1" "$FOREIGN2" "$FOREIGN3" "$FOREIGN4"; do
        if [ -r "$f" ]; then
            IFS= read -r fnode < "$f" 2>/dev/null || fnode=
            [ "$fnode" = "$1" ] && return 0
        fi
    done
    return 1
}

# Return a node to the stock root:input 0660, but only if it still carries a
# grant and another emulator watcher does not hold it (the same pad may be
# selected in multiple apps; restoring it here would break the other grant).
restore_node() {
    valid_node "$1" || return 0
    if foreign_holds "$1"; then
        log "keep $1 (held by tiznes-input-access)"
        return 0
    fi
    [ -c "$1" ] || return 0
    node_owner=$(stat -c '%U:%G' "$1" 2>/dev/null) || return 1
    node_mode=$(stat -c '%a' "$1" 2>/dev/null) || return 1
    if [ "$node_owner" = "$STOCK" ] && [ "$node_mode" = 660 ]; then return 0; fi
    if [ "$node_owner" != "$GRANT" ] && [ "$node_owner" != "$STOCK" ]; then
        log "restore-unknown-owner $1 owner=${node_owner:-unknown}"; return 1
    fi
    if ! chown "$STOCK" "$1" || ! chmod 660 "$1" ||
       [ "$(stat -c '%U:%G:%a' "$1" 2>/dev/null)" != "$STOCK:660" ]; then
        log "restore-failed $1"; return 1
    fi
    log "restore $1"
    return 0
}

grant_node() {
    valid_node "$1" && [ -c "$1" ] || return 1
    foreign_holds "$1" && return 1
    chown "$GRANT" "$1" && chmod 600 "$1"
}

granted() { [ "$(stat -c '%U:%G:%a' "$1" 2>/dev/null)" = "$GRANT:600" ]; }

read_state() {
    STATE_NODE=
    if [ -r "$1" ]; then IFS= read -r STATE_NODE < "$1" || :; fi
    valid_node "$STATE_NODE" || STATE_NODE=
}

ATTR=
read_attr() {
    ATTR=
    if [ -r "$1" ]; then IFS= read -r ATTR < "$1" 2>/dev/null || :; fi
    return 0
}

restore_all() {
    was_locked=$LOCKED
    [ "$LOCKED" = 1 ] || lock_access || return 1
    failed=0
    read_state "$STATE1"; node=$STATE_NODE; [ -n "$node" ] || node=$CUR1
    if [ -z "$node" ]; then rm -f "$STATE1" && CUR1= || failed=1
    elif restore_node "$node" && rm -f "$STATE1"; then CUR1=
    else failed=1
    fi
    read_state "$STATE2"; node=$STATE_NODE; [ -n "$node" ] || node=$CUR2
    if [ -z "$node" ]; then rm -f "$STATE2" && CUR2= || failed=1
    elif restore_node "$node" && rm -f "$STATE2"; then CUR2=
    else failed=1
    fi
    [ "$was_locked" = 1 ] || unlock_access
    return "$failed"
}
trap 'restore_all' EXIT
# Without the explicit exit, bash runs the handler and then resumes the loop,
# which re-granted the node before systemd's SIGKILL left it granted.
trap 'restore_all; rc=$?; [ "$rc" -eq 0 ] && exit 0; exit 1' HUP INT TERM

# Sets TARGET to the single event node of the USB controller with identity
# $1, or leaves it empty. Only button-capable joysticks count (the DualShock 4
# adds a button-less "Motion Sensors" joystick and has no USB serial), and
# exactly one must match. Exact vid:pid:serial:port is preferred; a pad with
# a non-empty serial is also recognized on another port when unique.
TARGET=
resolve_identity() {
    TARGET=
    [ -n "$1" ] || return 0
    IFS=: read -r want_vid want_pid want_serial want_port <<EOF_ID
$1
EOF_ID
    selected_parent=
    js_matches=0
    loose_parent=
    loose_matches=0
    for jslink in "$SYSIN"/js*/device; do
        [ -e "$jslink" ] || continue
        js_parent=$(readlink -f "$jslink" 2>/dev/null) || continue
        read_attr "$js_parent/capabilities/key"
        case "$ATTR" in *[123456789abcdefABCDEF]*) ;; *) continue ;; esac
        dir=$js_parent; depth=0
        while [ "$depth" -lt 12 ]; do
            if [ -r "$dir/idVendor" ] && [ -r "$dir/idProduct" ]; then
                read_attr "$dir/idVendor"; vid=$ATTR
                read_attr "$dir/idProduct"; pid=$ATTR
                read_attr "$dir/serial"; serial=$ATTR
                port=${dir##*/}
                if [ "$vid" = "$want_vid" ] && [ "$pid" = "$want_pid" ] && [ "$serial" = "$want_serial" ]; then
                    if [ "$port" = "$want_port" ]; then
                        js_matches=$((js_matches+1)); selected_parent=$js_parent
                    elif [ -n "$want_serial" ]; then
                        loose_matches=$((loose_matches+1)); loose_parent=$js_parent
                    fi
                fi
                break
            fi
            parent=${dir%/*}; [ "$parent" != "$dir" ] || break; dir=$parent; depth=$((depth+1))
        done
    done
    if [ "$js_matches" -eq 0 ] && [ "$loose_matches" -eq 1 ]; then
        js_matches=1; selected_parent=$loose_parent
    fi
    [ "$js_matches" -eq 1 ] || return 0
    matches=0; found=
    for devlink in "$SYSIN"/event*/device; do
        [ -e "$devlink" ] || continue
        event_parent=$(readlink -f "$devlink" 2>/dev/null) || continue
        [ "$event_parent" = "$selected_parent" ] || continue
        handler=${devlink%/device}
        node=$DEVIN/${handler##*/}
        # Input-subsystem character device (major 13 = 0xd); the minor changes
        # with enumeration order, so it is deliberately not pinned.
        if [ -c "$node" ] && [ "$(stat -c '%t' "$node" 2>/dev/null)" = "d" ]; then
            matches=$((matches+1)); found=$node
        fi
    done
    [ "$matches" -eq 1 ] && TARGET=$found
    return 0
}

# Adopt grants left by a previous instance (crash, restart), so they are
# restored if no longer selected instead of being orphaned.
lock_access || exit 1
read_state "$STATE1"; CUR1=$STATE_NODE
read_state "$STATE2"; CUR2=$STATE_NODE
if [ -n "$CUR1" ] && foreign_holds "$CUR1"; then CUR1=; rm -f "$STATE1"; fi
if [ -n "$CUR2" ] && foreign_holds "$CUR2"; then CUR2=; rm -f "$STATE2"; fi
[ "$CUR2" != "$CUR1" ] || CUR2=
log "start cur1=${CUR1:-none} cur2=${CUR2:-none}"
unlock_access

while :; do
    lock_access || { sleep 1; continue; }
    enabled=0
    id1=
    id2=
    if [ -r "$CONFIG" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                exclusive=1) enabled=1 ;;
                exclusive=0) enabled=0 ;;
                identity=*) id1=${line#identity=} ;;
                identity2=*) id2=${line#identity2=} ;;
                *) ;;
            esac
        done < "$CONFIG"
    fi
    valid_identity "$id1" || id1=
    valid_identity "$id2" || id2=
    [ "$id2" != "$id1" ] || id2=

    T1=
    T2=
    if [ "$enabled" = 1 ]; then
        resolve_identity "$id1"; T1=$TARGET
        resolve_identity "$id2"; T2=$TARGET
        [ "$T2" != "$T1" ] || T2=
        if [ -n "$T1" ] && foreign_holds "$T1"; then T1=; fi
        if [ -n "$T2" ] && foreign_holds "$T2"; then T2=; fi
    fi

    # Release old grants completely before publishing new slot ownership.
    if [ "$T1" != "$CUR1" ]; then
        if [ -n "$CUR1" ]; then
            if restore_node "$CUR1" && rm -f "$STATE1"; then CUR1=
            else log "release-pending player=1 node=$CUR1"; T1=; fi
        fi
    fi
    if [ "$T2" != "$CUR2" ]; then
        if [ -n "$CUR2" ]; then
            if restore_node "$CUR2" && rm -f "$STATE2"; then CUR2=
            else log "release-pending player=2 node=$CUR2"; T2=; fi
        fi
    fi
    [ -z "$CUR1" ] || [ "$T2" != "$CUR1" ] || T2=
    [ -z "$CUR2" ] || [ "$T1" != "$CUR2" ] || T1=

    # Grant phase; also re-grant a held node whose permissions were reset
    # (a replug that reused the same eventN, or another watcher restoring it).
    if [ -n "$T1" ]; then
        if [ -z "$CUR1" ] || ! granted "$CUR1"; then
            record_ok=0
            if record_state "$STATE1" "$T1"; then CUR1=$T1; record_ok=1
            else read_state "$STATE1"; [ "$STATE_NODE" = "$T1" ] && { CUR1=$T1; record_ok=1; }
            fi
            if [ "$record_ok" = 1 ] && grant_node "$T1"; then
                log "grant slot=1 node=$T1 identity=$id1"
            else
                if [ "$record_ok" = 1 ] && restore_node "$T1" && rm -f "$STATE1"; then CUR1=
                elif [ "$record_ok" = 1 ]; then log "grant-cleanup-pending slot=1 node=$T1"
                else log "state-record-failed slot=1 node=$T1"
                fi
            fi
        fi
    fi
    if [ -n "$T2" ]; then
        if [ -z "$CUR2" ] || ! granted "$CUR2"; then
            record_ok=0
            if record_state "$STATE2" "$T2"; then CUR2=$T2; record_ok=1
            else read_state "$STATE2"; [ "$STATE_NODE" = "$T2" ] && { CUR2=$T2; record_ok=1; }
            fi
            if [ "$record_ok" = 1 ] && grant_node "$T2"; then
                log "grant slot=2 node=$T2 identity=$id2"
            else
                if [ "$record_ok" = 1 ] && restore_node "$T2" && rm -f "$STATE2"; then CUR2=
                elif [ "$record_ok" = 1 ]; then log "grant-cleanup-pending slot=2 node=$T2"
                else log "state-record-failed slot=2 node=$T2"
                fi
            fi
        fi
    fi
    unlock_access
    sleep "$POLL"
done
