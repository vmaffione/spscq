#!/bin/bash

# Core to monitor
CORE=${1:-0}

# Duration
DUR=${2:-2}

# Otput file
OUTF=${3:-cms.out}

TMPF=$(mktemp)
sudo perf stat -C $CORE -e instructions,L1-dcache-load-misses,L1-dcache-store-misses sleep $DUR > $TMPF 2>&1
#cat $TMPF
LDMISS=$(grep "L1-dcache-load-misses" $TMPF | awk '{print $1}')
STMISS=$(grep "L1-dcache-store-misses" $TMPF | awk '{print $1}')
INSN=$(grep "instructions" $TMPF | awk '{print $1}')
SECS=$(grep "seconds time elapsed" $TMPF | awk '{print $1}')
#rm $TMPF
# Print L1 dcache miss rates in M/sec, instructions per second in B/sec
awk "BEGIN {print $LDMISS / $SECS / 1000000, $STMISS / $SECS / 1000000, $INSN / $SECS / 1000000000.0}" > $OUTF
