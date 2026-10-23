#!/bin/sh

set -u
found_tests=0
total=0
passed=0
failed=0

check_log() {
    log_file=$1
    expected=$2
    grep -Fq -- "$expected" "$log_file"
}

for config in tests/*.txt
do
    [ -f "$config" ] || continue
    found_tests=1
    total=$((total + 1))
    name=$(basename "$config" .txt)
    log="tests/${name}.log"
    output="/tmp/emergency_dispatch_${name}.out"

    printf '[TEST] %s ... ' "$name"

    if ! ./emergency_dispatch -c "$config" -d 0 -l "$log" >"$output" 2>&1; then
        printf 'FAIL (program returned a non-zero status)\n'
        failed=$((failed + 1))
        cat "$output"
        continue
    fi

    if ! check_log "$log" 'SHIFT FINISHED'; then
        printf 'FAIL (missing SHIFT FINISHED in log)\n'
        failed=$((failed + 1))
        continue
    fi

    case "$name" in
        test_cancel)
            expected='CALL #002 CANCELLED'
            ;;
        test_escalation)
            expected='PRIORITY ESCALATED'
            ;;
        test_fastest)
            expected='-> B2 (FASTEST)'
            ;;
        test_nearest)
            expected='-> B1 (NEAREST)'
            ;;
        test_no_team)
            expected='Unserved: 1'
            ;;
        test_shift_limit)
            expected='Unserved: 1'
            ;;
        test_single_call)
            expected='CALL #001 SERVICE COMPLETED'
            ;;
        *)
            expected=''
            ;;
    esac

    if [ -n "$expected" ] && ! check_log "$log" "$expected"; then
        printf 'FAIL (expected log entry not found: %s)\n' "$expected"
        failed=$((failed + 1))
        continue
    fi

    printf 'PASS\n'
    passed=$((passed + 1))
done

if [ "$found_tests" -eq 0 ]; then
    printf 'No test files found in tests/.\n'
    exit 1
fi

printf '\nTests: %d total, %d passed, %d failed\n' "$total" "$passed" "$failed"
[ "$failed" -eq 0 ]
