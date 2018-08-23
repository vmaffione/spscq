#!/bin/bash

EXPDIR=expV
QLEN=128
TRIALS=5
N=30
DURATION=10
#MORE="--dry-run"
SKIPCPU=20

./run-vswitch.py -v $MORE -l $QLEN --duration $DURATION --max-trials $TRIALS \
                -x $SKIPCPU --Bv 8 --Bc 1  -n $N | tee $EXPDIR/001
./run-vswitch.py -v $MORE -l $QLEN --duration $DURATION --max-trials $TRIALS \
                -x $SKIPCPU --Bv 8 --Bc 8  -n $N | tee $EXPDIR/002
./run-vswitch.py -v $MORE -l $QLEN --duration $DURATION --max-trials $TRIALS \
                -x $SKIPCPU --Bv 8 --Bc 1  -n $N --exp-type latency | tee $EXPDIR/003
./run-vswitch.py -v $MORE -l $QLEN --duration $DURATION --max-trials $TRIALS \
                -x $SKIPCPU --Bv 8 --Bc 8  -n $N --exp-type latency | tee $EXPDIR/004
