#!/bin/sh
# Command line tests: usage errors, and an end-to-end run through the real binary against the mock receiver.
BIN=${1:-./airplay-send}
MOCK=./tests/test_mock
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails+1)); }

$BIN --help >/dev/null 2>&1 && ok "--help exits 0" || bad "--help exits 0"
$BIN --version 2>&1 | grep -q airplay-send && ok "--version" || bad "--version"
$BIN </dev/null >/dev/null 2>&1; [ $? -eq 2 ] && ok "no target is a usage error" || bad "no target is a usage error"
$BIN -t 127.0.0.1:1 -V 500 --tone 1 >/dev/null 2>&1; [ $? -eq 2 ] && ok "bad volume rejected" || bad "bad volume rejected"
$BIN -t 127.0.0.1:1 --tone 1 2>err.txt >/dev/null; rc=$?
[ $rc -eq 1 ] && grep -q "connect to 127.0.0.1:1 failed" err.txt && ok "refused connection reports an error" || bad "refused connection reports an error"
echo hello | $BIN -t 127.0.0.1:1 --raw /nonexistent/file >/dev/null 2>&1; [ $? -eq 1 ] && ok "missing file is an error" || bad "missing file is an error"
$BIN --list --scan 300 >/dev/null 2>&1 && ok "--list runs" || bad "--list runs"

# end to end: stdin pipeline, host:port target with per-target volume
$MOCK --serve > mock.out &
mpid=$!
i=0
while ! grep -q '^PORT' mock.out 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
port=$(sed -n 's/^PORT //p' mock.out | head -1)
if [ -z "$port" ]; then bad "mock receiver started"; else
    head -c 352800 /dev/zero | $BIN -q -t "127.0.0.1:$port=50" 2>cli.err
    rc=$?
    wait $mpid; mrc=$?
    [ $rc -eq 0 ] && ok "pipeline exits 0" || { bad "pipeline exits 0"; cat cli.err; }
    [ $mrc -eq 0 ] && ok "receiver verified handshake and every packet" || { bad "receiver verified"; cat mock.out; }
    grep -q 'volume_db=-15.00' mock.out && ok "per-target volume (50% = -15 dB)" || bad "per-target volume"
fi
rm -f err.txt mock.out cli.err
[ $fails -eq 0 ] && echo "cli tests passed" || { echo "$fails cli test(s) failed"; exit 1; }
