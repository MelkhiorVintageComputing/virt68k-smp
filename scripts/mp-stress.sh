# Commands run inside the NetBSD guest by netbsd-run.py (one per line).
awk 'BEGIN{for(i=0;i<400000;i++)s+=i*i; print s}' > /dev/null; time awk 'BEGIN{for(i=0;i<400000;i++)s+=i*i}'
time sh -c 'for i in 1 2 3 4; do awk "BEGIN{for(i=0;i<400000;i++)s+=i*i}" & done; wait'
mkdir -p /tmp/b && cd /tmp/b && for i in 1 2 3 4 5 6 7 8; do printf 'int f%d(int x){int s=0;for(int i=0;i<x;i++)s+=i*%d;return s;}\n' $i $i > f$i.c; done && printf 'OBJS=f1.o f2.o f3.o f4.o f5.o f6.o f7.o f8.o\nall: $(OBJS)\n' > Makefile && echo ok
cd /tmp/b && rm -f *.o && time make -j4 > /dev/null
time sh -c 'i=0; while [ $i -lt 100 ]; do (/bin/echo -n &) ; i=$((i+1)); done; wait'
sysctl -n hw.ncpu; vmstat -s | grep -e "cpu context switches" -e "device interrupts" -e "software interrupts"
