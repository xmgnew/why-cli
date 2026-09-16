#!/bin/sh
# Every runtime file stays in this test's build-directory fixture.
set -eu
scratch=$(mktemp -d why-recorder-XXXXXX)
export WHY_SOCKET_NAME="$scratch"
export WHY_RUNTIME_DIR="$PWD/$scratch/must-not-be-created"
recorder=
cleanup() {
    if [ -n "$recorder" ]; then
        kill "$recorder" 2>/dev/null || :
        wait "$recorder" 2>/dev/null || :
    fi
    rm -f "$scratch/watch" "$scratch/query" "$scratch/second" "$scratch/error"
    rm -f "$WHY_RUNTIME_DIR/recorder.sock" "$WHY_RUNTIME_DIR/recorder.sock.lock"
    rmdir "$WHY_RUNTIME_DIR" 2>/dev/null || :
    rmdir "$scratch"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"$1" watch > "$scratch/watch" 2>&1 &
recorder=$!
i=0
until "$1" > "$scratch/query" 2> "$scratch/error"; do
    kill -0 "$recorder"
    i=$((i + 1))
    [ "$i" -lt 10 ]
    sleep 1
done
if "$1" watch > "$scratch/error" 2>&1; then
    echo 'Second recorder unexpectedly started' >&2
    exit 1
fi
sleep 2
"$1" > "$scratch/query"
grep -q 'Query-window totals' "$scratch/query"
grep -q 'requested 60 seconds' "$scratch/query"
"$1" cpu 2s > "$scratch/second"
grep -q 'requested 2 seconds' "$scratch/second"
"$1" 300s > "$scratch/second"
grep -q 'Requested prefix unavailable' "$scratch/second"
first_end=$(sed -n 's/^Query window: [0-9.]*\.\.\([0-9.]*\) monotonic.*/\1/p' "$scratch/query")
sleep 2
"$1" cpu > "$scratch/query"
second_end=$(sed -n 's/^Query window: [0-9.]*\.\.\([0-9.]*\) monotonic.*/\1/p' "$scratch/query")
[ -n "$first_end" ] && [ -n "$second_end" ] && [ "$first_end" != "$second_end" ]
kill "$recorder"
wait "$recorder"
recorder=
[ ! -e "$WHY_RUNTIME_DIR" ]
if "$1" > "$scratch/error" 2>&1; then
    echo 'Query unexpectedly succeeded without recorder' >&2
    exit 1
fi
grep -q "Start 'why watch'" "$scratch/error"
echo 'Linux watch/query, continued sampling, duplicate prevention and shutdown passed.'
