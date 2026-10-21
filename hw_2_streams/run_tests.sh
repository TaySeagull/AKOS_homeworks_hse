found_tests=0

for config in tests/*.txt
do
    if [ ! -f "$config" ]
    then
        continue
    fi

    found_tests=1

    total=$((total + 1))

    name=$(basename "$config" .txt)
    log="tests/${name}.log"

    printf '[TEST] %s ... ' "$name"

    if ./emergency_dispatch -c "$config" -d 0 -l "$log" >/tmp/emergency_dispatch_test.out 2>&1
    then
        printf 'PASS\n'
        passed=$((passed + 1))
    else
        printf 'FAIL\n'
        failed=$((failed + 1))

        printf '\n--- Output of %s ---\n' "$name"
        cat /tmp/emergency_dispatch_test.out
        printf '%s\n' '--------------------'
    fi
done

if [ "$found_tests" -eq 0 ]
then
    printf 'No test files found in tests/.\n'
    exit 1
fi