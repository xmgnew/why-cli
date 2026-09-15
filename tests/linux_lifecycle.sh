#!/bin/sh
# CTest runs this in the build directory; keep every artifact here.
set -eu
scratch=$(mktemp -d why-live-XXXXXX)
recorder=
workload=
cleanup() {
    if [ -n "$workload" ]; then
        kill "$workload" 2>/dev/null || :
        wait "$workload" 2>/dev/null || :
    fi
    if [ -n "$recorder" ]; then
        kill "$recorder" 2>/dev/null || :
        wait "$recorder" 2>/dev/null || :
    fi
    rm -f "$scratch/output"
    rmdir "$scratch"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"$1" sample --count 7 --history > "$scratch/output" &
recorder=$!
sleep 1
sleep 3 &
workload=$!
pid=$workload
wait "$workload"
workload=
wait "$recorder"
recorder=
if ! grep -Eq "[[:space:]]$pid:[0-9]+[[:space:]]+first seen$" "$scratch/output"; then
    echo "Controlled workload was not observed" >&2
    exit 1
fi
if ! grep -Eq "[[:space:]]$pid:[0-9]+[[:space:]]+no longer observed$" "$scratch/output"; then
    echo "Controlled workload disappearance was not retained" >&2
    exit 1
fi
echo "Live Linux process appearance and disappearance retained successfully."
