#!/bin/sh
# Build and run every test in src/test, reporting failures by exit code.
#
# Usage: scripts/test.sh
set -u

QZ=${QZ:-./build/quarzumc}
fail=0
total=0

for f in src/test/*.qz; do
    base=$(basename "$f")
    case "$base" in
        file.qz|map.qz) continue ;;        # no main()
        test_exit.qz) continue ;;          # its success IS exiting with 42
    esac
    total=$((total + 1))

    rm -f out out.asm out.o
    out=$("$QZ" "$f" --build 2>&1)
    if echo "$out" | grep -qiE 'error|undefined reference'; then
        echo "COMPILE FAIL  $base"
        echo "$out" | head -3
        fail=$((fail + 1))
        continue
    fi
    if [ ! -x ./out ]; then
        echo "NO OUTPUT     $base"
        fail=$((fail + 1))
        continue
    fi

    ./out >/dev/null 2>&1
    code=$?
    if [ "$code" -ne 0 ]; then
        echo "RUN FAIL      $base (exit $code)"
        fail=$((fail + 1))
    fi
done

rm -f out out.asm out.o
echo "ran $total tests, failures: $fail"
[ "$fail" -eq 0 ]
