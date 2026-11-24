#!/bin/sh
# End-to-end network check: boot tests/firmware/nettest.c on the rv32-test board
# (tests/boards/; UAM_RUN is build/tests/uam-test-run) with virtio-net port net0 attached to a
# real tap or tun interface, and let the host kernel's IP stack talk to it (the guest
# ARPs for and pings 10.0.2.2, then the host pings the guest at 10.0.2.15).
#
# Linux: runs without root. Unless already root, it re-runs itself in a new user and
# network namespace (unshare -Urn), where it may create the tap and tun interfaces.
# macOS: needs root (sudo sh tests/net/net-check.sh ...), as creating a utun or a vmnet
# segment does; without it the check is skipped. tap is a vmnet host-mode segment with the
# host at 10.0.2.2 (tap:10.0.2.2/24), tun a utun made and addressed by utun-launch.py
# before the emulation starts and handed to it as tun-fd:FD.
#
# usage: net-check.sh UAM_RUN NETTEST.elf [tap|tun|both]
# Extra uam-run options can be passed in UAM_ARGS (e.g. UAM_ARGS=--jit).
set -u
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../portable.sh"
UAM_RUN=$(realpath "$1")
FW=$(realpath "$2")
MODE=${3:-both}

skip() { echo "net-check: $*; skipping" >&2; exit 0; }

os=$(uname -s)
case $os in
Linux)
    if [ "$(id -u)" -ne 0 ] && [ -z "${NET_CHECK_NS:-}" ]; then
        command -v unshare >/dev/null 2>&1 || skip "unshare not found"
        unshare -Urn true 2>/dev/null || skip "cannot create a user/network namespace"
        NET_CHECK_NS=1 exec unshare -Urn sh "$0" "$@"
    fi
    command -v ip >/dev/null 2>&1 || skip "ip (iproute2) not found"
    [ -c /dev/net/tun ] || skip "/dev/net/tun missing"
    ip link set lo up
    ;;
Darwin)
    [ "$(id -u)" -eq 0 ] ||
        skip "on macOS the tap (vmnet) and tun (utun) interfaces need root: sudo sh $0 $*"
    command -v python3 >/dev/null 2>&1 || skip "python3 not found"
    ;;
*)
    skip "the tap and tun backends are Linux and macOS only"
    ;;
esac
command -v ping >/dev/null 2>&1 || skip "ping not found"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ping_guest FILE: three echo requests to the guest, with a deadline.
ping_guest() {
    if [ "$os" = Darwin ]; then
        ping -c 3 -i 0.2 -t 10 10.0.2.15 > "$1" 2>&1
    else
        ping -c 3 -i 0.2 -W 5 10.0.2.15 > "$1" 2>&1
    fi
}

# run_one KIND: returns 0 on success.
run_one() {
    kind=$1
    out=$TMP/$kind.console
    : > "$out"
    nic_opts=",pcap=$TMP/$kind.pcap"
    set -- -b rv32-test ${UAM_ARGS:-} --console "file:$out"
    if [ "$os" = Linux ]; then
        ifn=uam${kind}0
        ip tuntap add dev "$ifn" mode "$kind" || return 1
        if [ "$kind" = tap ]; then
            ip addr add 10.0.2.2/24 dev "$ifn"
        else
            ip addr add 10.0.2.2 peer 10.0.2.15 dev "$ifn"
        fi
        ip link set "$ifn" up
        timeout 30 "$UAM_RUN" "$@" --nic "net0=$kind:$ifn$nic_opts" "$FW" 2> "$TMP/$kind.err" &
    elif [ "$kind" = tap ]; then
        timeout 30 "$UAM_RUN" "$@" --nic "net0=tap:10.0.2.2/24$nic_opts" "$FW" 2> "$TMP/$kind.err" &
    else
        timeout 30 python3 "$here/utun-launch.py" 10.0.2.2 10.0.2.15 -- \
            "$UAM_RUN" "$@" --nic "net0=tun-fd:{fd}$nic_opts" "$FW" 2> "$TMP/$kind.err" &
    fi
    pid=$!

    # The guest first resolves and pings the host; then the host pings back.
    n=0
    while ! grep -q "host answered our ping" "$out" 2>/dev/null; do
        n=$((n + 1))
        if [ $n -gt 100 ] || ! kill -0 $pid 2>/dev/null; then
            echo "net-check[$kind]: the guest never reached the host"
            break
        fi
        sleep 0.1
    done
    ping_guest "$TMP/$kind.ping"
    ping_rc=$?
    wait $pid
    rc=$?
    [ "$os" = Linux ] && ip link del "$ifn"

    tr -d '\r' < "$out"
    if [ $rc -ne 0 ] || [ $ping_rc -ne 0 ]; then
        echo "net-check[$kind]: FAILED (uam-run status $rc, ping status $ping_rc)"
        cat "$TMP/$kind.ping" "$TMP/$kind.err"
        return 1
    fi
    echo "net-check[$kind]: ok ($(grep -oE '[0-9]+ (packets )?received' "$TMP/$kind.ping" | grep -oE '^[0-9]+') replies received by the host," \
         "capture $(wc -c < "$TMP/$kind.pcap" | tr -d ' ') bytes)"
    return 0
}

# macOS: tun:utunN creates the interface itself (the address comes after, so only its
# creation is checked here; the ping exchange above runs through tun-fd).
utun_create() {
    "$UAM_RUN" -v -b rv32-test -n 1000 --console null --nic net0=tun: "$FW" > /dev/null 2> "$TMP/utun.err"
    if grep -q 'attached to tun utun[0-9]' "$TMP/utun.err"; then
        echo "net-check[tun:]: ok ($(grep -o 'attached to tun utun[0-9]*' "$TMP/utun.err"))"
        return 0
    fi
    echo "net-check[tun:]: FAILED: tun: made no utun interface"
    cat "$TMP/utun.err"
    return 1
}

status=0
case $MODE in
tap)  run_one tap || status=1 ;;
tun)  run_one tun || status=1 ;;
both) run_one tap || status=1
      run_one tun || status=1 ;;
*)    echo "usage: $0 UAM_RUN NETTEST.elf [tap|tun|both]" >&2; exit 2 ;;
esac
if [ "$os" = Darwin ] && [ "$MODE" != tap ]; then
    utun_create || status=1
fi
exit $status
