#!/bin/bash

EXPDIR=expS
SPMIN=0
TRIALS=3
N=10
DURATION=5
STEP=10
#MORE="--dry-run"

./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS --step $STEP \
                --spin-min $SPMIN --num-points $N -S square | tee $EXPDIR/001
./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS --step $STEP \
                --spin-min $SPMIN --num-points $N -S square -M | tee $EXPDIR/002
./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS  --step $STEP \
                --spin-min $SPMIN --num-points $N -S square --Bp 1 --Bc 1 | tee $EXPDIR/003
./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS --step $STEP \
                --spin-min $SPMIN --num-points $N -S square -M --Bp 1 --Bc 1 | tee $EXPDIR/004

./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS \
                --exp-type latency --spin-min 0 --num-points 1 | tee $EXPDIR/005
./run-tests.py -vw $MORE --duration $DURATION --max-trials $TRIALS -M \
                --exp-type latency --spin-min 0 --num-points 1 | tee $EXPDIR/006
