#!/bin/bash

perf_output_extract() {
	local GR=$(grep "$1" $TMPF)
	echo $GR | grep "not supported" > /dev/null
	if [ "$?" == "0" ]; then
		# default value
		echo "0"
	else
		echo $GR | awk '{print $1}'
	fi
}

export LANG=C

# Core to monitor
CORE=${1:-0}

# Duration
DUR=${2:-2}

# Otput file
OUTF=${3:-cms.out}

TMPF=$(mktemp)
sudo perf stat -C $CORE -e instructions,L1-dcache-load-misses,L1-dcache-store-misses sleep $DUR > $TMPF 2>&1
#cat $TMPF
LDMISS=$(perf_output_extract "L1-dcache-load-misses")
STMISS=$(perf_output_extract "L1-dcache-store-misses")
INSN=$(perf_output_extract "instructions")
SECS=$(perf_output_extract "seconds time elapsed")
#rm $TMPF
# Print L1 dcache miss rates in M/sec, instructions per second in B/sec
awk "BEGIN {print $LDMISS / $SECS / 1000000, $STMISS / $SECS / 1000000, $INSN / $SECS / 1000000000.0}" > $OUTF
