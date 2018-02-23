#!/bin/bash

SPIN_STEP=3
SPIN_MAX=72

./run-tests.py --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S parallel | tee exp/001
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S crossed | tee exp/002
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ptriangle | tee exp/003
./run-tests.py --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ctriangle | tee exp/004

./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S parallel | tee exp/005
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S crossed | tee exp/006
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ptriangle | tee exp/007
./run-tests.py -M --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ctriangle | tee exp/008

./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S parallel | tee exp/009
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S crossed | tee exp/010
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ptriangle | tee exp/011
./run-tests.py --exp-type latency --spin-min 0 --spin-max $SPIN_MAX --spin-step $SPIN_STEP -S ctriangle | tee exp/012
