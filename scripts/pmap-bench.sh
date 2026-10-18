#!/bin/sh
# Run inside the NetBSD guest: pmap micro-benchmarks with 1, 2, 4, ...
# parallel copies, plus a small parallel compile.  For each run, prints
#   BENCH <test> <copies> <real> <user> <sys>
# followed by the pmap event counters (vmstat -e, summed over the CPUs)
# that changed during the run.
# usage: pmap-bench.sh BENCH.c [COPIES...]
# (BENCH.c is scripts/pmap-bench.c, copied into the guest)
SRC=${1:?usage: pmap-bench.sh pmap-bench.c [copies...]}
shift
COPIES=${*:-"1 2 4 8"}
B=/tmp/pmap-bench
cc -O2 -o $B "$SRC" -lpthread || exit 1

pmapev() {
	vmstat -e | awk '$1 == "pmap" && $2 ~ /^cpu/ {
		n = $3; for (i = 4; i <= NF - 3; i++) n = n "_" $i
		t[n] += $(NF - 2) } END { for (n in t) print n, t[n] }' | sort
}

run() {	# name copies command...
	name=$1; np=$2; shift 2
	pmapev > /tmp/ev.before
	/usr/bin/time -p sh -c "i=0; while [ \$i -lt $np ]; do $* & i=\$((i+1)); done; wait" \
	    2> /tmp/time.out
	pmapev > /tmp/ev.after
	set -- $(awk '{print $2}' /tmp/time.out)
	echo "BENCH $name $np $1 $2 $3"
	awk 'NR == FNR { b[$1] = $2; next }
	    { d = $2 - b[$1]; if (d) printf "  %s %d\n", $1, d }' /tmp/ev.before /tmp/ev.after
}

# one source tree per parallel copy for the compile test
for d in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	mkdir -p /tmp/cc$d; t=""
	i=1; while [ $i -le 16 ]; do
		printf 'int f%d(int x){return x*%d+1;}\nint main(void){return f%d(0)-1;}\n' $i $i $i > /tmp/cc$d/c$i.c
		t="$t c$i.x"; i=$((i+1)); done
	printf 'all:%s\n.SUFFIXES: .x .c\n.c.x:\n\tcc -O1 -o $@ $<\n' "$t" > /tmp/cc$d/Makefile
done

for np in $COPIES; do
	run fork  $np $B fork 300
	run exec  $np $B exec 150
	run mmap  $np $B mmap 20 8
	run mprot $np $B mprot 20000
	run fault $np $B fault 5 4
	run cc    $np 'make -s -j4 -B -C /tmp/cc$i >/dev/null'
done
echo BENCH DONE
