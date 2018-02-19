#!/usr/bin/env python
#
# Written by: Vincenzo Maffione <v.maffione@gmail.com>

import subprocess
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
argparser.add_argument('--duration', help = "Duration of each test run in seconds",
                       type = int, default = 1)

args = argparser.parse_args()

spin_p = args.spin_min
spin_c = args.spin_min + args.delta_max
try:
    queue = 'lq'
    while spin_p <= args.spin_min + args.delta_max:
        #print("P=%d C=%d" % (spin_p, spin_c))
        cmd = './spscq -D %d -P %d -C %d -t %s' % (args.duration, spin_p, spin_c, queue)
        print("Running '%s'" % cmd)
        try:
            out = subprocess.check_output(cmd.split())
        except subprocess.CalledProcessError:
            print('Command "%s" failed' % cmd)
            quit()
        out = str(out, 'ascii')  # decode
        for line in out.split('\n'):
            m = re.match(r'^Throughput\s+([0-9]+\.[0-9]+)\s+Mpps', line)
            if m:
                mpps = float(m.group(1))
                print(mpps)
        spin_p += args.delta_step
        spin_c -= args.delta_step
except KeyboardInterrupt:
    pass
