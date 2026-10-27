#!/bin/sh
# End-to-end network check: boot tests/firmware/nettest.c on the rv32-test board
# (tests/boards/; UAM_RUN is build/uam-test-run) with virtio-net port net0 attached to a
# real Linux tap or tun interface, and let the
# host kernel's IP stack talk to it (the guest ARPs for and pings 10.0.2.2, then
# the host pings the guest at 10.0.2.15).
#
# Runs without root: unless already root, it re-runs itself in a new user and
# network namespace (unshare -Urn), where it may create interfaces.
#
# usage: net-check.sh UAM_RUN NETTEST.elf [tap|tun|both]
# Extra uam-run options can be passed in UAM_ARGS (e.g. UAM_ARGS=--jit).
set -u
UAM_RUN=$(realpath "$1")
FW=$(realpath "$2")
MODE=${3:-both}

skip() { echo "net-check: $*; skipping" >&2; exit 0; }

if [ "$(id -u)" -ne 0 ] && [ -z "${NET_CHECK_NS:-}" ]; then
    command -v unshare >/dev/null 2>&1 || skip "unshare not found"
    unshare -Urn true 2>/dev/null || skip "cannot create a user/network namespace"
    NET_CHECK_NS=1 exec unshare -Urn sh "$0" "$@"
fi
command -v ip >/dev/null 2>&1 || skip "ip (iproute2) not found"
command -v ping >/dev/null 2>&1 || skip "ping not found"
[ -c /dev/net/tun ] || skip "/dev/net/tun missing"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ip link set lo up

# run_one KIND IFNAME: returns 0 on success.
run_one() {
    kind=$1 ifn=$2
    ip tuntap add dev "$ifn" mode "$kind" || return 1
    if [ "$kind" = tap ]; then
        ip addr add 10.0.2.2/24 dev "$ifn"
    else
        ip addr add 10.0.2.2 peer 10.0.2.15 dev "$ifn"
    fi
    ip link set "$ifn" up

    out=$TMP/$kind.console
    : > "$out"
    timeout 30 "$UAM_RUN" -b rv32-test ${UAM_ARGS:-} --console "file:$out" --nic "net0=$kind:$ifn,pcap=$TMP/$kind.pcap" \
        "$FW" 2> "$TMP/$kind.err" &
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
    ping -c 3 -i 0.2 -W 5 10.0.2.15 > "$TMP/$kind.ping" 2>&1
    ping_rc=$?
    wait $pid
    rc=$?
    ip link del "$ifn"

    tr -d '\r' < "$out"
    if [ $rc -ne 0 ] || [ $ping_rc -ne 0 ]; then
        echo "net-check[$kind]: FAILED (uam-run status $rc, ping status $ping_rc)"
        cat "$TMP/$kind.ping" "$TMP/$kind.err"
        return 1
    fi
    echo "net-check[$kind]: ok ($(grep -o '[0-9]* received' "$TMP/$kind.ping") by the host," \
         "capture $(wc -c < "$TMP/$kind.pcap") bytes)"
    return 0
}

status=0
case $MODE in
tap)  run_one tap uamtap0 || status=1 ;;
tun)  run_one tun uamtun0 || status=1 ;;
both) run_one tap uamtap0 || status=1
      run_one tun uamtun0 || status=1 ;;
*)    echo "usage: $0 UAM_RUN NETTEST.elf [tap|tun|both]" >&2; exit 2 ;;
esac
exit $status
