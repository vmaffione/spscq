#!/bin/bash

SPIN_STEP_THR=3
SPIN_MAX_THR=72
SPIN_STEP_LAT=100
SPIN_MAX_LAT=0

./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S parallel | tee exp/001
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S crossed | tee exp/002
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ptriangle | tee exp/003
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ctriangle | tee exp/004

./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S parallel | tee exp/005
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S crossed | tee exp/006
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ptriangle | tee exp/007
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX_THR --spin-step $SPIN_STEP_THR -S ctriangle | tee exp/008

./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S parallel | tee exp/009
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S crossed | tee exp/010
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ptriangle | tee exp/011
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ctriangle | tee exp/012

./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S parallel | tee exp/013
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S crossed | tee exp/014
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ptriangle | tee exp/015
./run-tests.py -M --exp-type latency --spin-min 0 --spin-max $SPIN_MAX_LAT --spin-step $SPIN_STEP_LAT -S ctriangle | tee exp/016
