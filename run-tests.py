#!/usr/bin/env python
#
# Written by: Vincenzo Maffione <v.maffione@gmail.com>

import argparse
import re
import os

description = "Experiment with SPSC queues"
epilog = "2018 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('--spin-min', help = "Minimum emulated load (spin)",
                       type = int, default = 5)
argparser.add_argument('--delta-step', help = "Step to use to increase delta",
                       type = int, default = 5)
argparser.add_argument('--delta-max', help = "Max delta between P and C spins",
                       type = int, default = 50)

args = argparser.parse_args()

spin_p = args.spin_min
spin_c = args.spin_min + args.delta_max
try:
    while spin_p <= args.spin_min + args.delta_max:
        print("P=%d C=%d" % (spin_p, spin_c))
        spin_p += args.delta_step
        spin_c -= args.delta_step
except KeyboardInterrupt:
    pass
