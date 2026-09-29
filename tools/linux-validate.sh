#!/bin/sh
# Run inside the isolated image; the sole output mount receives evidence logs.
set -eu
if ! test -d /wadb-full-disk; then
    echo 'Mount --tmpfs /wadb-full-disk:rw,size=16m,mode=0700; see docs/platform-validation.md' >&2
    exit 1
fi
output=${1:-/results}
mkdir -p "$output"
{
    date -u
    uname -a
    cc --version
    ldd --version
    pkg-config --modversion libmicrohttpd
    dpkg-query -W gcc libc6 make python3
    stat -f -c 'filesystem=%T' /tmp
    if test -f /sys/fs/cgroup/memory.max; then cat /sys/fs/cgroup/memory.max; fi
    if test -f /sys/fs/cgroup/cpu.max; then cat /sys/fs/cgroup/cpu.max; fi
} > "$output/environment.txt" 2>&1

echo 'Running Linux correctness and subprocess crash tests'
if ! make test > "$output/test.log" 2>&1; then tail -n 100 "$output/test.log"; exit 1; fi
echo 'Running Linux AddressSanitizer and UndefinedBehaviorSanitizer'
if ! make sanitize > "$output/sanitize.log" 2>&1; then tail -n 100 "$output/sanitize.log"; exit 1; fi
echo 'Running Linux HTTP replay, slow-client and shutdown smoke'
if ! python3 tools/http-stress-smoke.py --output "$output/http-smoke.json" > "$output/http-smoke.log" 2>&1; then
    cat "$output/http-smoke.log"; exit 1
fi
echo 'Running actual ENOSPC and recovery on the private tmpfs'
if ! python3 tools/full-disk-smoke.py --output "$output/full-disk.json" > "$output/full-disk.log" 2>&1; then
    cat "$output/full-disk.log"; exit 1
fi
echo 'Linux validation passed'
