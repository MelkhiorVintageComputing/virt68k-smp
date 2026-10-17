#!/bin/sh
# Run inside the NetBSD guest: parallel compile + file copy soak, with
# checksums to detect memory/TLB corruption.  usage: soak.sh ROUNDS JOBS
R=${1:-5}; J=${2:-8}
D=/tmp/soak; rm -rf $D; mkdir -p $D && cd $D || exit 1
i=1; while [ $i -le 48 ]; do
  printf '#include <stdio.h>\nint main(void){unsigned s=%d;for(int k=0;k<20000;k++)s=s*1103515245u+12345u;printf("%%u\\n",s);return 0;}\n' $i > p$i.c
  i=$((i+1)); done
printf 'P!=ls p*.c | sed s/.c$$//\nall: $(P:=.out)\n.SUFFIXES: .out .c\n.c.out:\n\tcc -O1 -o $*.bin $< && ./$*.bin > $@\n' > Makefile
dd if=/dev/urandom of=big bs=64k count=64 2>/dev/null; ref=$(cksum < big)
first=""
r=1; fail=0
while [ $r -le $R ]; do
  rm -f *.out *.bin c*
  make -j$J >/dev/null 2>&1 || { echo "round $r: make failed"; fail=1; }
  for k in 1 2 3 4 5 6 7 8; do cp big c$k & done; wait
  for k in 1 2 3 4 5 6 7 8; do [ "$(cksum < c$k)" = "$ref" ] || { echo "round $r: copy $k corrupt"; fail=1; }; done
  sum=$(cat $(ls p*.out | sort) | cksum)
  [ -z "$first" ] && first=$sum
  [ "$sum" = "$first" ] || { echo "round $r: compile outputs differ"; fail=1; }
  echo "round $r ok-so-far=$((1-fail)) $(date +%T)"
  r=$((r+1))
done
[ $fail = 0 ] && echo SOAK PASS || echo SOAK FAIL
