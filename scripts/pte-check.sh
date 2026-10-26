#!/bin/sh
# Boot KERNEL (UP, stock QEMU, stock 11.0 image), run mlock-hold in the
# guest (NLOCK wired + NFREE unwired pages, CPU-bound) and scan the
# supervisor and user page tables from the host (pte-scan.py).
# usage: pte-check.sh KERNEL [NLOCK NFREE]
TOP=$(cd "$(dirname "$0")/.." && pwd)
K=$1; NL=${2:-100}; NF=${3:-50}
S=${SCRATCH:-${TMPDIR:-/tmp}}
MON=$S/ptemon.$$		# AF_UNIX paths are limited to 108 bytes
LOG=$TOP/images/pte-check-$(basename "$K").log
rm -f "$MON" "$LOG"
"$TOP/scripts/netbsd-run.py" --qemu "$TOP/work/qemu-stock/build/qemu-system-m68k" \
    --smp 1 --mem 512M --kernel "$K" --image "$TOP/images/netbsd-11.0-stock-live.img" \
    --log "$LOG" --monitor "$MON" \
    --put "$TOP/scripts/mlock-hold.c:/tmp/mh.c" \
    -c 'cc -O2 -o /tmp/mh /tmp/mh.c' \
    -c "/tmp/mh $NL $NF & sleep 3; echo SCAN-NOW; sleep 60; kill %1; vmstat -s | grep -i wired" \
    > "$S/pte-check.$$.out" 2>&1 &
RUN=$!
until grep -aq "^SCAN-NOW" "$LOG" 2>/dev/null; do
	kill -0 $RUN 2>/dev/null || { echo "guest run ended early"; cat "$S/pte-check.$$.out"; exit 1; }
	sleep 1
done
grep -a "^locked" "$LOG"
python3 "$TOP/scripts/pte-scan.py" "$MON" -v
python3 "$TOP/scripts/pte-scan.py" "$MON" -u -v
wait $RUN
grep -a "wired" "$S/pte-check.$$.out"
rm -f "$MON" "$S/pte-check.$$.out"
