#!/bin/bash

# Core to monitor
CORE=${1:-0}

# Duration
DUR=${2:-2}

# Otput file
OUTF=${3:-cms.out}

TMPF=$(mktemp)
sudo perf stat -d -C $CORE sleep $DUR > $TMPF 2>&1
# -e cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores,L1-dcache-store-misses
#cat $TMPF
LDRATE=$(grep "L1-dcache-loads" $TMPF | awk '{print $4}')
MISSPERC=$(grep "L1-dcache-load-misses" $TMPF | awk '{print $4}'|rev|cut -c 2- | rev)
GHZ=$(grep "\<cycles\>" $TMPF | grep GHz | awk '{print $4}')
INSNPC=$(grep "\<instructions\>" $TMPF | grep "insn per cycle" | awk '{print $4}')
rm $TMPF
# Print L1 dcache miss rate in M/sec, instructions per second in B/sec
awk "BEGIN {print $LDRATE * $MISSPERC / 100.0, $INSNPC * $GHZ}" > $OUTF

