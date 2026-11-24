#!/usr/bin/env bash
#
# Starts the independent programs of one session in this terminal:
# the clients are started in the background (in random order, before the
# clock: they wait for it), the clock works in the foreground.
#
#   ./run_independent.sh shm [clock options]     - 7-8 points version
#   ./run_independent.sh mq  [clock options]     - 9-10 points version
#
# Example: ./run_independent.sh mq -c tests/test_stress.txt -d 50
# Ctrl+C / Ctrl+\ work as usual: they reach every process of the
# terminal, the clients forward them to the clock.
#
# The programs can also be started by hand in different terminals, see README.md.

set -u

KIND=${1:-}
if [ "$KIND" != "shm" ] && [ "$KIND" != "mq" ]; then
    echo "usage: $0 shm|mq [options of the clock]" >&2
    exit 1
fi
shift

DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$DIR/bin"

# configuration and session name are needed to start the right number of teams
CONFIG=config.txt
SESSION=main
ARGS=("$@")
for ((i = 0; i < ${#ARGS[@]}; i++)); do
    case "${ARGS[$i]}" in
    -c) CONFIG=${ARGS[$((i + 1))]:-$CONFIG} ;;
    -n) SESSION=${ARGS[$((i + 1))]:-$SESSION} ;;
    esac
done

if [ ! -f "$CONFIG" ]; then
    echo "configuration file $CONFIG not found" >&2
    exit 1
fi

# number of lines in the [TEAMS] section (comments and empty lines skipped)
TEAMS=$(awk '/^\[/{s=$0; next} s=="[TEAMS]" && $0 !~ /^[ \t]*(#|$)/ {n++} END{print n+0}' "$CONFIG")

pids=()
"$BIN/ed_${KIND}_dispatcher" -n "$SESSION" -w 10 &
pids+=($!)
for ((t = 0; t < TEAMS; t++)); do
    "$BIN/ed_${KIND}_team" -n "$SESSION" -w 10 &
    pids+=($!)
done
"$BIN/ed_${KIND}_callers" -n "$SESSION" -w 10 &
pids+=($!)

"$BIN/ed_${KIND}_clock" "$@"
code=$?

# the clients end when the clock sends them MSG_SHUTDOWN
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null
done

exit $code
