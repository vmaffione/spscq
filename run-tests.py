#!/usr/bin/env python
#
# Written by: Vincenzo Maffione <v.maffione@gmail.com>

import statistics
import subprocess
import argparse
import sys
import re
import os

description = "Experiment with SPSC queues"
epilog = "2018 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('--spin-min', help = "Minimum emulated load (cycles)",
                       type = int, default = 5)
argparser.add_argument('--spin-step', help = "Step to use to increase spin",
                       type = int, default = 5)
argparser.add_argument('--spin-max', help = "Maximum emulated load (cycles)",
                       type = int, default = 50)
argparser.add_argument('-D', '--duration',
                       help = "Duration of a test run in seconds",
                       type = int, default = 10)
argparser.add_argument('--trials', help = "How many test runs per point",
                       type = int, default = 10)
argparser.add_argument('-M', '--indirect', action = 'store_true',
                       help = "Use mbufs (indirect payload)")
argparser.add_argument('--exp-type', type = str, default = 'throughput',
                       choices = ['throughput', 'latency'],
                       help = "Experiment type")
argparser.add_argument('-l', '--queue-length', help = "Queue length",
                       type = int, default = 256)
argparser.add_argument('-b', '--batch-size', help = "Batch size",
                       type = int, default = 32)
argparser.add_argument('-S', '--sequencing', type = str, default = 'parallel',
                       choices = ['parallel', 'crossed'],
                       help = "How P and C emulated load vary")

args = argparser.parse_args()

queues = ['lq', 'llq', 'blq', 'ffq', 'iffq', 'biffq']
results = {}

if args.spin_min > args.spin_max:
    print("Error: spin_min > spin_max")
    quit(1)

if args.spin_min < 0:
    print("Error: spin_min < 1")
    quit(1)

if args.spin_step < 1:
    print("Error: spin_step < 1")
    quit(1)

if args.trials < 1:
    print("Error: trias < 1")
    quit(1)

try:
    spin_p = args.spin_min
    if args.sequencing == 'parallel':
        spin_c = spin_p
    else:
        spin_c = args.spin_max

    while spin_p <= args.spin_max:
        results[(spin_p, spin_c)] = {}
        for queue in queues:
            cmd = './spscq -D %d -l %d -L %d -b %d -P %d -C %d -t %s'\
                     % (args.duration, args.queue_length, args.batch_size,
                        args.batch_size, spin_p, spin_c, queue)
            if args.indirect:
                cmd += ' -M'
            if args.exp_type == 'latency':
                cmd += ' -T'
            print("Running '%s'" % cmd)
            mpps_values = []
            for _ in range(0, args.trials):
                try:
                    out = subprocess.check_output(cmd.split())
                except subprocess.CalledProcessError:
                    print('Command "%s" failed' % cmd)
                    quit(1)
                out = str(out, 'ascii')  # decode
                for line in out.split('\n'):
                    m = re.match(r'^Throughput\s+([0-9]+\.[0-9]+)\s+Mpps', line)
                    if m:
                        mpps = float(m.group(1))
                        mpps_values.append(mpps)
                        print("Got %f Mpps" % mpps)
            results[(spin_p, spin_c)][queue] = mpps_values

        spin_p += args.spin_step
        if args.sequencing == 'parallel':
            spin_c = spin_p
        else:
            spin_c -= args.spin_step
except KeyboardInterrupt:
    print("Interrupted. Bye.")
    quit(1)

# Command line invocation
print(' '.join(sys.argv))

print(('%8s ' * 15) % ('P', 'C', 'delta', 'lq', 'std', 'llq', 'std', 'blq', 'std',
                    'ffq', 'std', 'iffq', 'std', 'biffq', 'std'))
for (p, c) in results:
    row = [p, c, p-c]
    for queue in queues:
        avg = statistics.mean(results[(p, c)][queue])
        if args.trials > 1:
            std = statistics.stdev(results[(p, c)][queue])
        else:
            std = 0.0
        row.append(avg)
        row.append(std)
    fmt = '%8s ' * 3
    fmt += '%8.2f ' * 12
    print(fmt % tuple(row))