#!/usr/bin/env bash
#
# Starts the whole network application in one terminal: the clients go into the
# background (they wait for the server), the server works in the foreground.
#
#   ./run_network.sh [-m] [-M n] [-H host] [options of the server]
#
#     -m        start one observer in plain mode (its output goes into this terminal)
#     -M n      start n observers in plain mode
#     -H host   address that the clients use to reach the server (default 127.0.0.1)
#
# Examples:
#   ./run_network.sh -c config.txt -d 100
#   ./run_network.sh -m -c tests/test_stress.txt -d 0 -p 5100
#
# Ctrl+C / Ctrl+\ work as usual: every program of the terminal gets the signal,
# the clients send the request to the server and the server decides what to do.
#
# The programs can of course be started by hand in different terminals or on
# different computers, see README.md.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$DIR/bin"

MONITORS=0
HOST=127.0.0.1

# the options of this script come first, the rest belongs to the server
while [ $# -gt 0 ]; do
    case "$1" in
    -m)
        MONITORS=1
        shift
        ;;
    -M)
        MONITORS=${2:-1}
        shift 2
        ;;
    -H)
        HOST=${2:-127.0.0.1}
        shift 2
        ;;
    *)
        break
        ;;
    esac
done

# configuration and port are needed to start the right number of teams
CONFIG=config.txt
PORT=5035
ARGS=("$@")

for ((i = 0; i < ${#ARGS[@]}; i++)); do
    case "${ARGS[$i]}" in
    -c) CONFIG=${ARGS[$((i + 1))]:-$CONFIG} ;;
    -p) PORT=${ARGS[$((i + 1))]:-$PORT} ;;
    esac
done

if [ ! -f "$CONFIG" ]; then
    echo "configuration file $CONFIG not found" >&2
    exit 1
fi

if [ ! -x "$BIN/ed_net_server" ]; then
    echo "build the programs first: make" >&2
    exit 1
fi

# number of lines in the [TEAMS] section (comments and empty lines skipped)
TEAMS=$(awk '/^\[/{s=$0; next} s=="[TEAMS]" && $0 !~ /^[ \t]*(#|$)/ {n++} END{print n+0}' "$CONFIG")

pids=()

for ((m = 0; m < MONITORS; m++)); do
    "$BIN/ed_net_monitor" -H "$HOST" -p "$PORT" -w 15 -P &
    pids+=($!)
done

"$BIN/ed_net_dispatcher" -H "$HOST" -p "$PORT" -w 15 &
pids+=($!)

for ((t = 0; t < TEAMS; t++)); do
    "$BIN/ed_net_team" -H "$HOST" -p "$PORT" -w 15 &
    pids+=($!)
done

"$BIN/ed_net_callers" -H "$HOST" -p "$PORT" -w 15 &
pids+=($!)

"$BIN/ed_net_server" "$@"
code=$?

# the clients end when the server sends them MSG_SHUTDOWN
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null
done

exit $code
