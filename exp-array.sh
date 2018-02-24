#!/bin/bash

EXPDIR=exp2
NUM_POINTS_THR=30
NUM_POINTS_LAT=1
TRIALS=5

./run-tests.py --trials $TRIALS --spin-min 0 --num-points $NUM_POINTS_THR -S parallel | tee $EXPDIR/001
./run-tests.py --trials $TRIALS --spin-min 0 --num-points $NUM_POINTS_THR -S ptriangle | tee $EXPDIR/002
./run-tests.py --trials $TRIALS --spin-min 0 --num-points $NUM_POINTS_THR -S ctriangle | tee $EXPDIR/003

./run-tests.py --trials $TRIALS -M --spin-min 0 --num-points $NUM_POINTS_THR -S parallel | tee $EXPDIR/004
./run-tests.py --trials $TRIALS -M --spin-min 0 --num-points $NUM_POINTS_THR -S ptriangle | tee $EXPDIR/005
./run-tests.py --trials $TRIALS -M --spin-min 0 --num-points $NUM_POINTS_THR -S ctriangle | tee $EXPDIR/006

./run-tests.py --trials $TRIALS --exp-type latency --spin-min 0 --num-points $NUM_POINTS_LAT -S parallel | tee $EXPDIR/007
./run-tests.py --trials $TRIALS -M --exp-type latency --spin-min 0 --num-points $NUM_POINTS_LAT -S parallel | tee $EXPDIR/008
