#!/bin/sh
# Run bare-metal tests on qemu-system-m68k -M virt.
# usage: run-baremetal.sh [-q QEMU] [-s "1 2 4"] [-t "single multi"] [test-glob]
# Each test must print PASS (and no FAIL) before powering off.
TOP=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$TOP/work/qemu/build/qemu-system-m68k}
SMPS=1
THREADS=single
TIMEOUT=${TIMEOUT:-60}
while getopts q:s:t:T: o; do
    case $o in
    q) QEMU=$OPTARG ;;
    s) SMPS=$OPTARG ;;
    t) THREADS=$OPTARG ;;
    T) TIMEOUT=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
GLOB=${1:-*}
LOG=$TOP/baremetal/build/logs
mkdir -p "$LOG"
make -s -C "$TOP/baremetal" || exit 1

pass=0; fail=0
for elf in "$TOP"/baremetal/build/$GLOB.elf; do
    t=$(basename "$elf" .elf)
    for smp in $SMPS; do
        for th in $THREADS; do
            log=$LOG/$t-smp$smp-$th.log
            timeout "$TIMEOUT" "$QEMU" -M virt -m 128M -smp "$smp" \
                -accel tcg,thread="$th" -display none -monitor none \
                -serial stdio -action panic=exit-failure \
                -kernel "$elf" $QEMU_EXTRA </dev/null >"$log" 2>&1
            rc=$?
            if [ $rc -eq 0 ] && grep -q '^PASS' "$log" && ! grep -q 'FAIL' "$log"; then
                res=PASS; pass=$((pass + 1))
            else
                res="FAIL (rc=$rc)"; fail=$((fail + 1))
            fi
            printf '%-16s smp=%-2s %-6s %s\n' "$t" "$smp" "$th" "$res"
        done
    done
done
echo "passed: $pass  failed: $fail  (logs in $LOG)"
[ $fail -eq 0 ]
