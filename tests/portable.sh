# portable.sh - sourced by the check scripts: the few tools they use that GNU/Linux has
# and macOS does not, with a stand-in where the host lacks them.
#
#   timeout SECS CMD...  run CMD, killed with SIGTERM after SECS seconds (exit 124), as
#                        GNU coreutils' timeout; gtimeout or a perl stand-in on macOS
#   sha256               SHA-256 of stdin, as sha256sum prints it

if ! command -v timeout >/dev/null 2>&1; then
    if command -v gtimeout >/dev/null 2>&1; then
        timeout() { gtimeout "$@"; }
    else
        timeout() {
            perl -e '
                my $t = shift;
                my $pid = fork() // die "timeout: fork: $!\n";
                if (!$pid) { exec { $ARGV[0] } @ARGV; exit 127 }
                local $SIG{ALRM} = sub { kill "TERM", $pid; waitpid $pid, 0; exit 124 };
                $SIG{TERM} = $SIG{INT} = sub { kill "TERM", $pid; waitpid $pid, 0; exit 143 };
                alarm $t;
                waitpid $pid, 0;
                exit(($? & 127) ? 128 + ($? & 127) : $? >> 8);
            ' "$@"
        }
    fi
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum; }
else
    sha256() { shasum -a 256; }
fi
