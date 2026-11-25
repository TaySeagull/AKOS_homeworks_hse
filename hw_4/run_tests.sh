#!/usr/bin/env bash
#
# Automatic tests of the emergency dispatch simulator (homework 4, network).
#
# Usage: ./run_tests.sh [net] [fork_named] [fork_unnamed] [shm] [mq]  (default: all five)
#
# "net" is the client-server version of homework 4, the other four variants are
# the versions of homework 3 whose simulation logic it reuses.
#
# Every program is checked with the same scenarios:
#  1. tests/test_*.txt   - the program must exit with code 0 and the log must
#     contain every line of tests/test_*.expect ('!' = must NOT be in the log);
#     common checks: "Invariant violations: 0", received = served + cancelled + unserved;
#  2. tests/bad/*.txt    - invalid configurations must be refused;
#  3. the main configuration;
#  4. random generator: same seed -> same log; 20 runs -> the same result;
#     60 random runs (half of them on one CPU) -> no deadlock;
#  5. signals: SIGINT, SIGQUIT, SIGUSR1, SIGTERM; in the independent versions the
#     signal is sent to a CLIENT process, which must forward it to the clock;
#  6. robustness: a team process is killed with SIGKILL -> the clock notices it,
#     stops with exit code 2 and removes the IPC objects;
#  7. after every test no IPC object (shared memory, semaphore, queue) may remain;
#  8. independent versions: clients started by hand, team chosen with -i,
#     an extra client is refused, a client without a clock gives up;
#  9. network version only: observers that come and go, a client that is killed
#     and started again, a server that is killed, the address and the port of
#     the command line, the requests of the user that travel as frames.

set -u

cd "$(dirname "$0")"

VARIANTS=("$@")
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=(net fork_named fork_unnamed shm mq)

# every run of the network version gets its own TCP port
PORT_BASE=${PORT_BASE:-5600}
PORT=$PORT_BASE

OUT_DIR=$(mktemp -d)
trap 'rm -rf "$OUT_DIR"' EXIT

total=0
passed=0
failed=0
counter=0

pass() { printf 'PASS\n'; passed=$((passed + 1)); }
fail() { printf 'FAIL (%s)\n' "$1"; failed=$((failed + 1)); }
start() { total=$((total + 1)); printf '[%-12s] %-30s ... ' "$VARIANT" "$1"; }

# value of "Name: N" from the plain-text summary in the log
summary_value() {
    grep -m1 "^$2:" "$1" | awk -F': ' '{print $2}'
}

# common checks for every log, prints the reason of a failure
common_checks() {
    local log=$1
    grep -q 'SHIFT FINISHED' "$log" || { echo "no SHIFT FINISHED"; return 1; }
    grep -q '^Invariant violations: 0$' "$log" || { echo "invariant violation"; return 1; }

    local received served cancelled unserved
    received=$(summary_value "$log" 'Calls received')
    served=$(summary_value "$log" 'Served')
    cancelled=$(summary_value "$log" 'Cancelled')
    unserved=$(summary_value "$log" 'Unserved')

    if [ -z "$received" ] || [ "$received" -ne $((served + cancelled + unserved)) ]; then
        echo "received=$received != served+cancelled+unserved"
        return 1
    fi
    return 0
}

# the lines of a log without the tag of the program and without the lines that
# differ between two runs by design (addresses of the clients and their pids)
log_body() {
    sed 's/^\(\[[0-9:]*\]\) {[^}]*} /\1 /' "$1" |
        grep -Ev 'CLIENT (CONNECTED|RECONNECTED|DISCONNECTED|FINISHED|LOST)|OBSERVER (CONNECTED|LEFT)|^Processes: '
}

# names of the IPC objects of this user that are still present
ipc_left() {
    {
        ls /dev/shm 2>/dev/null | grep 'ed35'
        [ -d /dev/mqueue ] && ls /dev/mqueue 2>/dev/null | grep 'ed35'
    } | tr '\n' ' '
}

# a new session name for the independent versions
new_session() {
    counter=$((counter + 1))
    SESSION="t$$_$counter"
}

# a new TCP port for the network version
new_port() {
    counter=$((counter + 1))
    PORT=$((PORT_BASE + counter % 300))
}

# runs the current variant in the foreground: run_sim <args of the clock/server>
run_sim() {
    case "$VARIANT" in
    fork_named | fork_unnamed) ./bin/ed_$VARIANT "$@" ;;
    shm | mq)
        new_session
        ./run_independent.sh "$VARIANT" -n "$SESSION" "$@"
        ;;
    net)
        new_port
        # -T 20: a test must not wait for a client for ever; an explicit -T wins
        ./run_network.sh -T 20 -p "$PORT" "$@"
        ;;
    esac
}

# starts the current variant in the background, sets CLOCK (pid of the clock)
# and RUNNER (pid to wait for)
start_sim() {
    case "$VARIANT" in
    fork_named | fork_unnamed)
        ./bin/ed_$VARIANT "$@" >"$OUT_DIR/bg.out" 2>&1 &
        RUNNER=$!
        CLOCK=$RUNNER
        ;;
    shm | mq)
        new_session
        ./run_independent.sh "$VARIANT" -n "$SESSION" "$@" >"$OUT_DIR/bg.out" 2>&1 &
        RUNNER=$!
        CLOCK=""
        for _ in $(seq 1 50); do
            CLOCK=$(pgrep -f "ed_${VARIANT}_clock -n $SESSION" | head -1)
            [ -n "$CLOCK" ] && break
            sleep 0.1
        done
        ;;
    net)
        new_port
        ./run_network.sh -T 20 -p "$PORT" "$@" >"$OUT_DIR/bg.out" 2>&1 &
        RUNNER=$!
        CLOCK=""
        for _ in $(seq 1 50); do
            CLOCK=$(pgrep -f "ed_net_server .*-p $PORT" | head -1)
            [ -n "$CLOCK" ] && break
            sleep 0.1
        done
        ;;
    esac
}

# pid of a process that plays a team (child of the clock / independent client)
team_pid() {
    case "$VARIANT" in
    fork_named | fork_unnamed) pgrep -P "$CLOCK" | sed -n '2p' ;; # 1st child = dispatcher
    shm | mq) pgrep -f "ed_${VARIANT}_team -n $SESSION" | head -1 ;;
    net) pgrep -f "ed_net_team .*-p $PORT" | head -1 ;;
    esac
}

# the process that receives the signal from the user in the tests
signal_target() {
    case "$VARIANT" in
    fork_named | fork_unnamed) echo "$CLOCK" ;;
    *) team_pid ;; # the client must pass the request of the user to the clock/server
    esac
}

for VARIANT in "${VARIANTS[@]}"; do
    case "$VARIANT" in
    fork_named | fork_unnamed) need=bin/ed_$VARIANT ;;
    shm | mq) need=bin/ed_${VARIANT}_clock ;;
    net) need=bin/ed_net_server ;;
    *) echo "unknown variant $VARIANT"; exit 1 ;;
    esac

    if [ ! -x "$need" ]; then
        echo "Build the programs first: make"
        exit 1
    fi

    echo "################ $VARIANT ################"
    echo "=== Scenarios ==="
    for config in tests/test_*.txt; do
        name=$(basename "$config" .txt)
        log="tests/${name}.${VARIANT}.log"
        expect="tests/${name}.expect"
        start "$name"

        if ! run_sim -c "$config" -d 0 -l "$log" >"$OUT_DIR/$name.out" 2>&1; then
            fail "non-zero exit code"
            tail -5 "$OUT_DIR/$name.out" | sed 's/^/    /'
            continue
        fi

        if ! reason=$(common_checks "$log"); then
            fail "$reason"
            continue
        fi

        # the expectations do not contain the process tag "{B1  }", remove it
        plain="$OUT_DIR/$name.plain"
        sed 's/^\(\[[0-9:]*\]\) {[^}]*} /\1 /' "$log" >"$plain"

        ok=1
        if [ -f "$expect" ]; then
            while IFS= read -r line || [ -n "$line" ]; do
                [ -z "$line" ] && continue
                if [ "${line:0:1}" = "!" ]; then
                    if grep -Fq -- "${line:1}" "$plain"; then
                        fail "unexpected: ${line:1}"; ok=0; break
                    fi
                elif ! grep -Fq -- "$line" "$plain"; then
                    fail "expected: $line"; ok=0; break
                fi
            done <"$expect"
        fi

        left=$(ipc_left)
        if [ "$ok" -eq 1 ] && [ -n "$left" ]; then fail "IPC objects left: $left"; ok=0; fi
        [ "$ok" -eq 1 ] && pass
    done

    echo "=== Invalid configurations (must be rejected) ==="
    for config in tests/bad/*.txt; do
        name=$(basename "$config" .txt)
        start "$name"
        # the independent versions: the clock refuses the file before the clients come
        case "$VARIANT" in
        fork_named | fork_unnamed) prog=./bin/ed_$VARIANT ;;
        net) prog=./bin/ed_net_server ;;
        *) prog=./bin/ed_${VARIANT}_clock ;;
        esac
        if $prog -c "$config" -d 0 -l "$OUT_DIR/$name.log" >"$OUT_DIR/$name.out" 2>&1; then
            fail "accepted an invalid configuration"
        elif [ -n "$(ipc_left)" ]; then
            fail "IPC objects left"
        else
            pass
        fi
    done

    echo "=== Main configuration ==="
    start "config.txt"
    if run_sim -c config.txt -d 0 -l "$OUT_DIR/main.log" >/dev/null 2>&1 && reason=$(common_checks "$OUT_DIR/main.log"); then
        pass
    else
        fail "${reason:-non-zero exit code}"
    fi

    echo "=== Random generator ==="
    start "same seed, same log"
    run_sim -c config.txt -d 0 -r 42 -t 300 -l "$OUT_DIR/r1.log" >/dev/null 2>&1
    run_sim -c config.txt -d 0 -r 42 -t 300 -l "$OUT_DIR/r2.log" >/dev/null 2>&1
    if cmp -s <(log_body "$OUT_DIR/r1.log" | sort) <(log_body "$OUT_DIR/r2.log" | sort) &&
        grep -q 'GENERATOR' "$OUT_DIR/r1.log" && reason=$(common_checks "$OUT_DIR/r1.log"); then
        pass
    else
        fail "${reason:-logs differ or no generated calls}"
    fi

    start "20 runs, same result"
    # the processes are scheduled differently every time, the outcome must not change
    run_sim -c tests/test_stress.txt -d 0 -l "$OUT_DIR/ref.log" >/dev/null 2>&1
    ok=1
    for run in $(seq 1 20); do
        run_sim -c tests/test_stress.txt -d 0 -l "$OUT_DIR/run.log" >/dev/null 2>&1
        if ! cmp -s <(log_body "$OUT_DIR/ref.log" | sort) <(log_body "$OUT_DIR/run.log" | sort); then
            ok=0; break
        fi
    done
    if [ "$ok" -eq 1 ]; then pass; else fail "run $run differs from the first one"; fi

    start "60 runs, no deadlock"
    # random scenarios with a time limit; on one CPU (taskset) the processes are
    # interleaved in other ways, a lost wake-up would show up here as a timeout
    pin=""
    command -v taskset >/dev/null 2>&1 && pin="taskset -c 0"
    ok=1
    for seed in $(seq 1 60); do
        prefix=""
        [ $((seed % 2)) -eq 0 ] && prefix=$pin
        if ! timeout 25 $prefix bash -c "$(declare -f run_sim new_session new_port); VARIANT=$VARIANT; PORT_BASE=$PORT_BASE; counter=$seed$$; run_sim -c tests/test_stress.txt -d 0 -r $seed -t 400 -l $OUT_DIR/dl.log" >/dev/null 2>&1 ||
            ! reason=$(common_checks "$OUT_DIR/dl.log"); then
            ok=0; break
        fi
    done
    if [ "$ok" -eq 1 ]; then pass; else fail "seed $seed: hang, crash or ${reason:-bad summary}"; fi
    pkill -9 -f "ed_${VARIANT}" 2>/dev/null # nothing must be left, but just in case of a failure

    echo "=== Signals ==="
    start "SIGINT, unlimited shift"
    # unlimited shift + random calls: the run can only be stopped by the user
    start_sim -c config.txt -d 20 -t 0 -r 7 -l "$OUT_DIR/int.log"
    sleep 1.5
    kill -INT "$(signal_target)"
    wait "$RUNNER"
    code=$?
    if [ "$code" -eq 0 ] && grep -q 'SIGNAL SIGINT' "$OUT_DIR/int.log" && grep -q 'SIMULATION INTERRUPTED BY USER' "$OUT_DIR/int.log" &&
        grep -q '^Stop reason: interrupted by user' "$OUT_DIR/int.log" && reason=$(common_checks "$OUT_DIR/int.log") &&
        [ -z "$(ipc_left)" ]; then
        pass
    else
        fail "${reason:-exit code $code, no interruption record or IPC left}"
    fi

    start "SIGQUIT, close intake"
    start_sim -c config.txt -d 30 -l "$OUT_DIR/quit.log"
    sleep 1.3
    kill -QUIT "$(signal_target)"
    wait "$RUNNER"
    code=$?
    if [ "$code" -eq 0 ] && grep -q 'INTAKE CLOSED BY USER' "$OUT_DIR/quit.log" &&
        grep -q '^Stop reason: intake closed' "$OUT_DIR/quit.log" && ! grep -q 'UNSERVED: simulation' "$OUT_DIR/quit.log" &&
        reason=$(common_checks "$OUT_DIR/quit.log"); then
        pass
    else
        fail "${reason:-exit code $code or no intake record}"
    fi

    start "SIGUSR1, live status"
    start_sim -c config.txt -d 60 -l "$OUT_DIR/usr1.log"
    sleep 1.2
    kill -USR1 "$(signal_target)"
    sleep 0.5
    kill -INT "$CLOCK"
    wait "$RUNNER"
    if grep -q 'LIVE STATUS' "$OUT_DIR/bg.out" && grep -q 'SIGNAL SIGUSR1' "$OUT_DIR/usr1.log"; then pass; else fail "no live status"; fi

    start "SIGTERM to a team process"
    # fork versions: the child forwards SIGTERM to the parent; independent: the client does
    start_sim -c config.txt -d 30 -l "$OUT_DIR/term.log"
    sleep 1.2
    kill -TERM "$(team_pid)"
    wait "$RUNNER"
    code=$?
    if [ "$code" -eq 0 ] && grep -q 'SIGNAL SIGTERM' "$OUT_DIR/term.log" && reason=$(common_checks "$OUT_DIR/term.log") &&
        [ -z "$(ipc_left)" ]; then
        pass
    else
        fail "${reason:-exit code $code or IPC left}"
    fi

    start "SIGKILL of a team process"
    # the network server waits for the client of the role: -T 5 says how long
    [ "$VARIANT" = "net" ] && KILL_EXTRA=(-T 5) || KILL_EXTRA=()
    start_sim "${KILL_EXTRA[@]}" -c config.txt -d 30 -l "$OUT_DIR/kill.log"
    sleep 1.2
    kill -KILL "$(team_pid)"
    wait "$RUNNER"
    code=$?
    sleep 0.2
    if [ "$VARIANT" = "net" ]; then failure_mark='CLIENT LOST'; else failure_mark='PROCESS FAILURE'; fi
    if [ "$code" -eq 2 ] && grep -q "$failure_mark" "$OUT_DIR/kill.log" &&
        grep -q '^Stop reason: a process terminated unexpectedly' "$OUT_DIR/kill.log" && [ -z "$(ipc_left)" ] &&
        [ -z "$(pgrep -f "ed_${VARIANT}")" ]; then
        pass
    else
        fail "exit code $code, no failure record, processes or IPC left: $(ipc_left)"
        pkill -9 -f "ed_${VARIANT}" 2>/dev/null
    fi

    echo "=== Command line ==="
    case "$VARIANT" in
    fork_named | fork_unnamed) prog=./bin/ed_$VARIANT ;;
    net) prog=./bin/ed_net_server ;;
    *) prog=./bin/ed_${VARIANT}_clock ;;
    esac

    start "-h"
    if $prog -h >/dev/null 2>&1; then pass; else fail "-h must exit with 0"; fi

    start "-d abc"
    if $prog -c config.txt -d abc -l "$OUT_DIR/x.log" >/dev/null 2>&1; then fail "wrong delay accepted"; else pass; fi

    start "-s fastest overrides"
    run_sim -c tests/test_nearest.txt -d 0 -s fastest -l "$OUT_DIR/s.log" >/dev/null 2>&1
    if grep -Fq -- '-> B2 (FASTEST' "$OUT_DIR/s.log"; then pass; else fail "command line did not override the file"; fi

    if [ "$VARIANT" = "shm" ] || [ "$VARIANT" = "mq" ]; then
        echo "=== Independent start ==="
        start "clients by hand, -i, extra"
        # the clock first, then the clients in a different order; team 2 is asked by id
        new_session
        ./bin/ed_${VARIANT}_clock -n "$SESSION" -c tests/test_fastest.txt -d 0 -l "$OUT_DIR/hand.log" >"$OUT_DIR/hand.out" 2>&1 &
        clock=$!
        sleep 0.3
        ./bin/ed_${VARIANT}_team -n "$SESSION" -i 2 >"$OUT_DIR/team2.out" 2>&1 &
        t2=$!
        sleep 0.3
        ./bin/ed_${VARIANT}_callers -n "$SESSION" >/dev/null 2>&1 &
        cl=$!
        ./bin/ed_${VARIANT}_team -n "$SESSION" >"$OUT_DIR/team1.out" 2>&1 &
        t1=$!
        sleep 0.3
        ./bin/ed_${VARIANT}_team -n "$SESSION" >"$OUT_DIR/extra.out" 2>&1 # no free team any more
        extra=$?
        ./bin/ed_${VARIANT}_dispatcher -n "$SESSION" >/dev/null 2>&1 &
        dp=$!
        wait "$clock"
        code=$?
        wait "$t1" "$t2" "$cl" "$dp"
        if [ "$code" -eq 0 ] && [ "$extra" -ne 0 ] && grep -q 'plays team B2' "$OUT_DIR/team2.out" &&
            grep -q 'plays team B1' "$OUT_DIR/team1.out" && grep -q 'No free role' "$OUT_DIR/extra.out" &&
            reason=$(common_checks "$OUT_DIR/hand.log") && [ -z "$(ipc_left)" ]; then
            pass
        else
            fail "${reason:-clock code $code, extra code $extra}"
        fi

        start "client without a clock"
        if ./bin/ed_${VARIANT}_team -n "nobody$$" -w 1 >/dev/null 2>&1; then fail "the client did not give up"; else pass; fi

        start "Ctrl+C while registering"
        new_session
        ./bin/ed_${VARIANT}_clock -n "$SESSION" -c config.txt -d 0 -l "$OUT_DIR/reg.log" >/dev/null 2>&1 &
        clock=$!
        sleep 0.3
        ./bin/ed_${VARIANT}_dispatcher -n "$SESSION" >/dev/null 2>&1 &
        dp=$!
        sleep 0.5
        kill -INT "$clock"
        wait "$clock"
        code=$?
        wait "$dp"
        sleep 0.2
        if [ "$code" -ne 0 ] && [ -z "$(ipc_left)" ] && [ -z "$(pgrep -f "ed_${VARIANT}_.* -n $SESSION")" ]; then
            pass
        else
            fail "clock code $code, IPC or processes left"
        fi
    fi

    if [ "$VARIANT" = "net" ]; then
        echo "=== Network: start by hand, address and port ==="
        start "clients by hand, -i, extra, -b"
        # the server listens on the loopback interface only, the clients get the
        # address and the port in the command line
        new_port
        ./bin/ed_net_server -b 127.0.0.1 -p "$PORT" -c tests/test_fastest.txt -d 0 -T 20 \
            -l "$OUT_DIR/hand.log" >"$OUT_DIR/hand.out" 2>&1 &
        server=$!
        sleep 0.3
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -i 2 -w 10 >"$OUT_DIR/team2.out" 2>&1 &
        t2=$!
        sleep 0.2
        ./bin/ed_net_callers -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        cl=$!
        # a client works in any directory: it needs no configuration file at all
        here=$PWD
        (cd / && exec "$here/bin/ed_net_team" -H 127.0.0.1 -p "$PORT" -w 10) >"$OUT_DIR/team1.out" 2>&1 &
        t1=$!
        sleep 0.3
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 2 >"$OUT_DIR/extra.out" 2>&1 # no free team
        extra=$?
        ./bin/ed_net_dispatcher -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        dp=$!
        wait "$server"
        code=$?
        wait "$t1" "$t2" "$cl" "$dp" 2>/dev/null
        if [ "$code" -eq 0 ] && [ "$extra" -ne 0 ] && grep -q 'plays team B2' "$OUT_DIR/team2.out" &&
            grep -q 'plays team B1' "$OUT_DIR/team1.out" && grep -q 'already played' "$OUT_DIR/extra.out" &&
            grep -q "listens on 127.0.0.1:$PORT" "$OUT_DIR/hand.out" && reason=$(common_checks "$OUT_DIR/hand.log"); then
            pass
        else
            fail "${reason:-server code $code, extra code $extra}"
        fi

        start "client without a server"
        new_port
        if ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 1 >/dev/null 2>&1; then
            fail "the client did not give up"
        else
            pass
        fi

        start "client with a wrong port"
        new_port
        wrong=$((PORT + 1))
        ./bin/ed_net_server -b 127.0.0.1 -p "$PORT" -c tests/test_single_call.txt -d 0 -T 10 \
            -l "$OUT_DIR/wrong.log" >/dev/null 2>&1 &
        server=$!
        sleep 0.3
        ./bin/ed_net_team -H 127.0.0.1 -p "$wrong" -w 1 >/dev/null 2>&1
        bad=$?
        kill -INT "$server" 2>/dev/null
        wait "$server" 2>/dev/null
        if [ "$bad" -ne 0 ]; then pass; else fail "a client reached a port where nobody listens"; fi

        echo "=== Network: observers ==="
        start "observers come and go"
        new_port
        ./bin/ed_net_dispatcher -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        dp=$!
        ./bin/ed_net_callers -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        cl=$!
        teams=$(awk '/^\[/{s=$0; next} s=="[TEAMS]" && $0 !~ /^[ \t]*(#|$)/ {n++} END{print n+0}' tests/test_stress.txt)
        tpids=()
        for ((t = 0; t < teams; t++)); do
            ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
            tpids+=($!)
        done
        ./bin/ed_net_monitor -H 127.0.0.1 -p "$PORT" -w 10 -P >"$OUT_DIR/obs1.out" 2>&1 &
        obs1=$!
        ./bin/ed_net_server -c tests/test_stress.txt -d 120 -T 20 -p "$PORT" \
            -l "$OUT_DIR/obs.log" >"$OUT_DIR/obs.out" 2>&1 &
        server=$!
        sleep 1.5
        # a second observer comes while the model is running, the first one leaves
        ./bin/ed_net_monitor -H 127.0.0.1 -p "$PORT" -w 10 -P >"$OUT_DIR/obs2.out" 2>&1 &
        obs2=$!
        sleep 0.7
        kill -INT "$obs1"
        wait "$server"
        code=$?
        wait "$obs1" "$obs2" "$dp" "$cl" "${tpids[@]}" 2>/dev/null
        if [ "$code" -eq 0 ] && grep -q 'STATE waiting=' "$OUT_DIR/obs1.out" &&
            grep -q 'CALL #' "$OUT_DIR/obs1.out" && grep -q 'FINAL stop_reason' "$OUT_DIR/obs2.out" &&
            grep -q 'observers=2' "$OUT_DIR/obs2.out" && grep -q 'OBSERVER LEFT' "$OUT_DIR/obs.log" &&
            reason=$(common_checks "$OUT_DIR/obs.log"); then
            pass
        else
            fail "${reason:-server code $code, the observers did not see the application}"
        fi

        echo "=== Network: a client that is killed and started again ==="
        start "team client restarted, run goes on"
        new_port
        ./bin/ed_net_dispatcher -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        dp=$!
        ./bin/ed_net_callers -H 127.0.0.1 -p "$PORT" -w 10 >/dev/null 2>&1 &
        cl=$!
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 10 -i 1 >/dev/null 2>&1 &
        victim=$!
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 10 -i 2 >/dev/null 2>&1 &
        t2=$!
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 10 -i 3 >/dev/null 2>&1 &
        t3=$!
        ./bin/ed_net_server -c tests/test_stress.txt -d 120 -p "$PORT" \
            -l "$OUT_DIR/again.log" >"$OUT_DIR/again.out" 2>&1 &
        server=$!
        sleep 1.5
        kill -9 "$victim" 2>/dev/null
        sleep 1.5
        ./bin/ed_net_team -H 127.0.0.1 -p "$PORT" -w 10 -i 1 >/dev/null 2>&1 &
        back=$!
        wait "$server"
        code=$?
        wait "$dp" "$cl" "$t2" "$t3" "$back" 2>/dev/null
        if [ "$code" -eq 0 ] && grep -q 'CLIENT DISCONNECTED: team B1' "$OUT_DIR/again.log" &&
            grep -q 'CLIENT RECONNECTED: team B1' "$OUT_DIR/again.log" &&
            grep -q 'SHIFT FINISHED' "$OUT_DIR/again.log" && reason=$(common_checks "$OUT_DIR/again.log"); then
            pass
        else
            fail "${reason:-server code $code, the run did not survive the restart of a client}"
        fi

        echo "=== Network: the server goes away ==="
        start "server killed, clients exit"
        new_port
        ./run_network.sh -T 20 -p "$PORT" -c tests/test_stress.txt -d 150 \
            -l "$OUT_DIR/gone.log" >"$OUT_DIR/gone.out" 2>&1 &
        runner=$!
        server=""
        for _ in $(seq 1 50); do
            server=$(pgrep -f "ed_net_server .*-p $PORT" | head -1)
            [ -n "$server" ] && break
            sleep 0.1
        done
        sleep 1.5
        kill -9 "$server" 2>/dev/null
        sleep 2
        left=$(pgrep -cf "ed_net_(team|dispatcher|callers|monitor) .*-p $PORT" || true)
        wait "$runner" 2>/dev/null
        if [ "${left:-0}" -eq 0 ] && grep -q 'connection to the server is gone' "$OUT_DIR/gone.out"; then
            pass
        else
            fail "$left client(s) stayed alive without the server"
            pkill -9 -f "ed_net_.* -p $PORT" 2>/dev/null
        fi

        start "server shutdown ends the clients"
        new_port
        ./run_network.sh -T 20 -p "$PORT" -c tests/test_stress.txt -d 0 \
            -l "$OUT_DIR/bye.log" >"$OUT_DIR/bye.out" 2>&1
        code=$?
        sleep 0.5
        left=$(pgrep -cf "ed_net_.* -p $PORT" || true)
        if [ "$code" -eq 0 ] && [ "${left:-0}" -eq 0 ] &&
            [ "$(grep -c 'the shift is over, exiting' "$OUT_DIR/bye.out")" -ge 5 ]; then
            pass
        else
            fail "code $code, $left program(s) left"
        fi

        echo "=== Network: command line of the clients ==="
        for client in dispatcher team callers monitor; do
            start "-h of ed_net_$client"
            if ./bin/ed_net_$client -h >/dev/null 2>&1; then pass; else fail "-h must exit with 0"; fi
        done

        start "ed_net_monitor -p abc"
        if ./bin/ed_net_monitor -p abc -w 1 >/dev/null 2>&1; then fail "a wrong port was accepted"; else pass; fi
    fi
done

echo
echo "IPC objects left after all tests: [$(ipc_left)]"
printf '\nTests: %d total, %d passed, %d failed\n' "$total" "$passed" "$failed"
[ "$failed" -eq 0 ]
