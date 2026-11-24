#!/usr/bin/env python3
"""utun-launch.py HOSTADDR PEERADDR -- COMMAND... - net-check.sh's tun case on macOS.

A utun interface exists only while its control socket is open, so it cannot be
created and addressed before the emulation the way `ip tuntap add` does on
Linux. This makes one (as root), gives it HOSTADDR with PEERADDR as the other
end, and runs COMMAND with the socket inherited: every "{fd}" in COMMAND is
replaced by its number (for uam-run --nic net0=tun-fd:{fd}). The guest's first
packet then finds the interface ready. Prints the interface's name on stderr.
"""
import fcntl
import os
import socket
import struct
import subprocess
import sys

UTUN_CONTROL_NAME = b"com.apple.net.utun_control"
CTLIOCGINFO = 0xC0644E03        # _IOWR('N', 3, struct ctl_info), sizeof 100
UTUN_OPT_IFNAME = 2


def main():
    if len(sys.argv) < 5 or sys.argv[3] != "--":
        sys.exit("usage: utun-launch.py HOSTADDR PEERADDR -- COMMAND...")
    host, peer, cmd = sys.argv[1], sys.argv[2], sys.argv[4:]
    s = socket.socket(socket.PF_SYSTEM, socket.SOCK_DGRAM, socket.SYSPROTO_CONTROL)
    info = fcntl.ioctl(s, CTLIOCGINFO, struct.pack("I96s", 0, UTUN_CONTROL_NAME))
    ctl_id = struct.unpack("I96s", info)[0]
    s.connect((ctl_id, 0))       # unit 0: the first free utun
    name = s.getsockopt(socket.SYSPROTO_CONTROL, UTUN_OPT_IFNAME, 16).rstrip(b"\0").decode()
    subprocess.run(["ifconfig", name, "inet", host, peer, "up"], check=True)
    print(f"utun-launch: {name} {host} -> {peer}", file=sys.stderr)
    fd = s.detach()
    os.set_inheritable(fd, True)
    cmd = [a.replace("{fd}", str(fd)) for a in cmd]
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    main()
