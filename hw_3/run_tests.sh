#!/usr/bin/env bash
#
# Automatic tests of the emergency dispatch simulator.
#
#  1. tests/test_*.txt   - scenarios. The program must exit with code 0 and the
#     log must contain every line of tests/test_*.expect
#     (a line that starts with '!' must NOT be in the log).
#     For every scenario the common checks are done as well:
#       - "Invariant violations: 0";
#       - received = served + cancelled + unserved.
#  2. tests/bad/*.txt    - invalid configurations, the program must refuse
#     them with a non-zero exit code.
#  3. Reproducibility of the random generator (same seed -> same set of log
#     lines; inside one model minute threads may print in a different order,
#     so the logs are compared after sorting).
#  4. Interruption by SIGINT (Ctrl+C) during a run with a visual delay.
#  5. Command-line checks (-h, wrong -d).

set -u

BIN=${1:-./emergency_dispatch}
echo "### Testing $BIN"
OUT_DIR=$(mktemp -d)
trap 'rm -rf "$OUT_DIR"' EXIT

total=0
passed=0
failed=0

pass() { printf 'PASS\n'; passed=$((passed + 1)); }
fail() { printf 'FAIL (%s)\n' "$1"; failed=$((failed + 1)); }

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

    if [ "$received" -ne $((served + cancelled + unserved)) ]; then
        echo "received=$received != served+cancelled+unserved=$((served + cancelled + unserved))"
        return 1
    fi
    return 0
}

if [ ! -x "$BIN" ]; then
    echo "Build the program first: make"
    exit 1
fi

echo "=== Scenarios ==="
for config in tests/test_*.txt; do
    [ -f "$config" ] || continue
    total=$((total + 1))

    name=$(basename "$config" .txt)
    log="tests/${name}.log"
    expect="tests/${name}.expect"

    printf '[TEST] %-22s ... ' "$name"

    if ! "$BIN" -c "$config" -d 0 -l "$log" >"$OUT_DIR/$name.out" 2>&1; then
        fail "non-zero exit code"
        sed 's/^/    /' "$OUT_DIR/$name.out" | tail -20
        continue
    fi

    if ! reason=$(common_checks "$log"); then
        fail "$reason"
        continue
    fi

    # the expectations do not contain the thread tag "{B1  }", remove it
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

    [ "$ok" -eq 1 ] && pass
done

echo "=== Invalid configurations (must be rejected) ==="
for config in tests/bad/*.txt; do
    [ -f "$config" ] || continue
    total=$((total + 1))
    name=$(basename "$config" .txt)
    printf '[TEST] %-22s ... ' "$name"

    if "$BIN" -c "$config" -d 0 -l "$OUT_DIR/$name.log" >"$OUT_DIR/$name.out" 2>&1; then
        fail "accepted an invalid configuration"
    else
        pass
    fi
done

echo "=== Main configuration ==="
total=$((total + 1))
printf '[TEST] %-22s ... ' "config.txt"
if "$BIN" -c config.txt -d 0 -l "$OUT_DIR/main.log" >/dev/null 2>&1 && reason=$(common_checks "$OUT_DIR/main.log"); then
    pass
else
    fail "${reason:-non-zero exit code}"
fi

echo "=== Random generator ==="
total=$((total + 1))
printf '[TEST] %-22s ... ' "same seed, same log"
"$BIN" -c config.txt -d 0 -r 42 -t 300 -l "$OUT_DIR/r1.log" >/dev/null 2>&1
"$BIN" -c config.txt -d 0 -r 42 -t 300 -l "$OUT_DIR/r2.log" >/dev/null 2>&1
if cmp -s <(sort "$OUT_DIR/r1.log") <(sort "$OUT_DIR/r2.log") && grep -q 'GENERATOR' "$OUT_DIR/r1.log" &&
    reason=$(common_checks "$OUT_DIR/r1.log"); then
    pass
else
    fail "${reason:-logs differ or no generated calls}"
fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "20 runs, same result"
# the threads are scheduled differently every time, the outcome must not change
"$BIN" -c tests/test_stress.txt -d 0 -l "$OUT_DIR/ref.log" >/dev/null 2>&1
ok=1
for run in $(seq 1 20); do
    "$BIN" -c tests/test_stress.txt -d 0 -l "$OUT_DIR/run.log" >/dev/null 2>&1
    if ! cmp -s <(sort "$OUT_DIR/ref.log") <(sort "$OUT_DIR/run.log"); then ok=0; break; fi
done
if [ "$ok" -eq 1 ]; then pass; else fail "run $run differs from the first one"; fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "60 runs, no deadlock"
# random scenarios with a time limit; on one CPU (taskset) the threads are
# interleaved in other ways, a lost wake-up would show up here as a timeout
pin=""
command -v taskset >/dev/null 2>&1 && pin="taskset -c 0"
ok=1
for seed in $(seq 1 60); do
    prefix=""
    [ $((seed % 2)) -eq 0 ] && prefix=$pin
    if ! timeout 10 $prefix "$BIN" -c tests/test_stress.txt -d 0 -r "$seed" -t 400 -l "$OUT_DIR/dl.log" >/dev/null 2>&1 ||
        ! reason=$(common_checks "$OUT_DIR/dl.log"); then
        ok=0; break
    fi
done
if [ "$ok" -eq 1 ]; then pass; else fail "seed $seed: hang, crash or ${reason:-bad summary}"; fi

echo "=== Interruption ==="
total=$((total + 1))
printf '[TEST] %-22s ... ' "SIGINT, unlimited"
# unlimited shift + random calls: the run can only be stopped by the user
"$BIN" -c config.txt -d 20 -t 0 -r 7 -l "$OUT_DIR/int.log" >/dev/null 2>&1 &
pid=$!
sleep 1
kill -INT "$pid"
wait "$pid"
code=$?
if [ "$code" -eq 0 ] && grep -q 'SIMULATION INTERRUPTED BY USER' "$OUT_DIR/int.log" &&
    grep -q '^Stop reason: interrupted by user' "$OUT_DIR/int.log" && reason=$(common_checks "$OUT_DIR/int.log"); then
    pass
else
    fail "${reason:-exit code $code or no interruption record}"
fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "SIGQUIT, close intake"
"$BIN" -c config.txt -d 30 -l "$OUT_DIR/quit.log" >/dev/null 2>&1 &
pid=$!
sleep 0.8
kill -QUIT "$pid"
wait "$pid"
code=$?
if [ "$code" -eq 0 ] && grep -q 'INTAKE CLOSED BY USER' "$OUT_DIR/quit.log" &&
    grep -q '^Stop reason: intake closed' "$OUT_DIR/quit.log" && ! grep -q 'UNSERVED: simulation' "$OUT_DIR/quit.log" &&
    reason=$(common_checks "$OUT_DIR/quit.log"); then
    pass
else
    fail "${reason:-exit code $code or no intake record}"
fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "SIGUSR1, live status"
"$BIN" -c config.txt -d 30 -l "$OUT_DIR/usr1.log" >"$OUT_DIR/usr1.out" 2>&1 &
pid=$!
sleep 0.5
kill -USR1 "$pid"
sleep 0.3
kill -INT "$pid"
wait "$pid"
if grep -q 'LIVE STATUS' "$OUT_DIR/usr1.out"; then pass; else fail "no live status"; fi

echo "=== Command line ==="
total=$((total + 1))
printf '[TEST] %-22s ... ' "-h"
if "$BIN" -h >/dev/null 2>&1; then pass; else fail "-h must exit with 0"; fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "-d abc"
if "$BIN" -c config.txt -d abc -l "$OUT_DIR/x.log" >/dev/null 2>&1; then fail "wrong delay accepted"; else pass; fi

total=$((total + 1))
printf '[TEST] %-22s ... ' "-s fastest overrides"
"$BIN" -c tests/test_nearest.txt -d 0 -s fastest -l "$OUT_DIR/s.log" >/dev/null 2>&1
if grep -Fq -- '-> B2 (FASTEST' "$OUT_DIR/s.log"; then pass; else fail "command line did not override the file"; fi

printf '\nTests: %d total, %d passed, %d failed\n' "$total" "$passed" "$failed"
[ "$failed" -eq 0 ]
