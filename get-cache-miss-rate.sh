#!/bin/bash

# Core to monitor
CORE=${1:-0}

# Duration
DUR=${2:-2}

# Otput file
OUTF=${3:-cms.out}

TMPF=$(mktemp)
sudo perf stat -d -C $CORE sleep $DUR > $TMPF 2>&1
#cat $TMPF
LDRATE=$(grep "L1-dcache-loads" $TMPF | awk '{print $4}')
MISSPERC=$(grep "L1-dcache-load-misses" $TMPF | awk '{print $4}'|rev|cut -c 2- | rev)
rm $TMPF
#echo "$LDRATE $MISSPERC"
awk "BEGIN {print $LDRATE * $MISSPERC / 100.0}" > $OUTF

