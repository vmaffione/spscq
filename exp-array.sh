#!/bin/bash

EXPDIR=exp2
NUM_POINTS_THR=29
TRIALS=10
SPIN_MIN=2

./run-tests.py --trials $TRIALS --spin-min 0 --num-points 1 -S parallel | tee $EXPDIR/001
./run-tests.py --trials $TRIALS --spin-min $SPIN_MIN --num-points $NUM_POINTS_THR -S ptriangle | tee $EXPDIR/002
./run-tests.py --trials $TRIALS --spin-min $SPIN_MIN --num-points $NUM_POINTS_THR -S ctriangle | tee $EXPDIR/003

./run-tests.py --trials $TRIALS -M --spin-min 0 --num-points 1 -S parallel | tee $EXPDIR/004
./run-tests.py --trials $TRIALS -M --spin-min $SPIN_MIN --num-points $NUM_POINTS_THR -S ptriangle | tee $EXPDIR/005
./run-tests.py --trials $TRIALS -M --spin-min $SPIN_MIN --num-points $NUM_POINTS_THR -S ctriangle | tee $EXPDIR/006

./run-tests.py --trials $TRIALS --exp-type latency --spin-min 0 --num-points 1 -S parallel | tee $EXPDIR/007
./run-tests.py --trials $TRIALS -M --exp-type latency --spin-min 0 --num-points 1 -S parallel | tee $EXPDIR/008

./run-tests.py --trials $TRIALS --Bp 1 --Bc 1 --spin-min 0 --num-points 1 -S parallel | tee $EXPDIR/009
./run-tests.py --trials $TRIALS -M --Bp 1 --Bc 1 --spin-min 1 --num-points 1 -S parallel | tee $EXPDIR/010
