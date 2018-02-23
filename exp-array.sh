#!/bin/bash

EXPDIR=exp
SPIN_STEP_THR=3
SPIN_MAX_THR=72
SPIN_STEP_LAT=100
SPIN_MAX_LAT=0

./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S parallel | tee $EXPDIR/001
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S crossed | tee $EXPDIR/002
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ptriangle | tee $EXPDIR/003
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ctriangle | tee $EXPDIR/004

./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S parallel | tee $EXPDIR/005
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S crossed | tee $EXPDIR/006
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ptriangle | tee $EXPDIR/007
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ctriangle | tee $EXPDIR/008

./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S parallel | tee $EXPDIR/009
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S crossed | tee $EXPDIR/010
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ptriangle | tee $EXPDIR/011
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ctriangle | tee $EXPDIR/012

./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S parallel | tee $EXPDIR/013
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S crossed | tee $EXPDIR/014
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ptriangle | tee $EXPDIR/015
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ctriangle | tee $EXPDIR/016
