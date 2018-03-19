#!/usr/bin/env python
#
# Written by: Vincenzo Maffione <v.maffione@gmail.com>

import statistics
import subprocess
import argparse
import sys
import re
import os
import itertools

def printfl(*args):
    print(*args)
    sys.stdout.flush()

description = "Experiment with SPSC queues"
epilog = "2018 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('--spin-min', help = "Minimum emulated load (cycles)",
                       type = int, default = 5)
argparser.add_argument('--num-points', help = "Number of points to take",
                       type = int, default = 25)
argparser.add_argument('--step', help = "How to generate steps (exp, 10, 20, ...)",
                       type = str, default = 'exp')
argparser.add_argument('-D', '--duration',
                       help = "Duration of a test run in seconds",
                       type = int, default = 10)
argparser.add_argument('--max-trials', help = "Max test runs per point",
                       type = int, default = 10)
argparser.add_argument('--min-trials', help = "Min test runs per point",
                       type = int, default = 5)
argparser.add_argument('-M', '--indirect', action = 'store_true',
                       help = "Use mbufs (indirect payload)")
argparser.add_argument('--exp-type', type = str, default = 'throughput',
                       choices = ['throughput', 'latency'],
                       help = "Experiment type")
argparser.add_argument('-l', '--queue-length', help = "Queue length",
                       type = int, default = 256)
argparser.add_argument('--Bp', dest = 'prod_batch', help = "Producer batch size",
                       type = int, default = 32)
argparser.add_argument('--Bc', dest = 'cons_batch', help = "Consumer batch size",
                       type = int, default = 32)
argparser.add_argument('-L', '--line-entries', help = "Number of entries per line",
                       type = int, default = 32)
argparser.add_argument('-S', '--sequencing', type = str, default = 'parallel',
                       choices = ['parallel', 'ptriangle', 'ctriangle',
                                    'square'],
                       help = "How P and C emulated load vary")
argparser.add_argument('--dry-run', action = 'store_true',
                       help = "Don't actually run spscq")

args = argparser.parse_args()

queues = ['lq', 'llq', 'blq', 'ffq', 'iffq', 'biffq']
results = {}

if args.spin_min < 0:
    printfl("Error: spin_min < 0")
    quit(1)

if args.num_points < 1:
    printfl("Error: min_points < 1")
    quit(1)

if args.max_trials < 1 or args.min_trials < 1:
    printfl("Error: trials < 1")
    quit(1)

if args.max_trials < args.min_trials:
    args.min_trials = args.max_trials

# Generate values for P and C spin
if args.step == 'exp':
    # Exponential
    z = args.spin_min
    points = [x for x in range(z,z+11)]
    z += 11
    points += [x for x in range(z, z+18, 2)]
    z += 18
    points += [x for x in range(z, z+30, 10)]
    z += 30
    points += [x for x in range(z, z+90, 20)]
    z += 90
    inc = 30
    while len(points) < args.num_points:
        points.append(points[-1] + inc)
        inc += 5
    points = points[:args.num_points]
else:
    # Check args.step is a positive integer
    try:
        args.step = int(args.step)
        if args.step < 1:
            raise ValueError()
    except ValueError:
        print('Invalid --step value "%s"' % args.step)
    points = []
    z = args.spin_min
    while len(points) < args.num_points:
        points.append(z)
        z += args.step

if args.sequencing == 'parallel':
    points = [(x, x) for x in points]
elif args.sequencing == 'ptriangle':
    points = [(x, args.spin_min) for x in points]
elif args.sequencing == 'ctriangle':
    points = [(args.spin_min, x) for x in points]
elif args.sequencing == 'square':
    points = [x for x in itertools.product(points, points)]
else:
    assert(False)

try:
    if args.sequencing == 'crossed':
        pi = 0
        ci = len(points) - 1
    else:
        pi = ci = 0

    for (spin_p, spin_c) in points:
        results[(spin_p, spin_c)] = {}
        for queue in queues:
            cmd = './spscq -D %d -l %d -L %d -b %d -b %d -P %d -C %d -t %s'\
                     % (args.duration, args.queue_length, args.line_entries,
                        args.prod_batch, args.cons_batch, spin_p, spin_c,
                        queue)
            if args.indirect:
                cmd += ' -M'
            if args.exp_type == 'latency':
                cmd += ' -T'
            printfl("Running '%s'" % cmd)
            mpps_values = []
            for k in range(1, args.max_trials+1):
                if args.dry_run:
                    mpps_values.append(0)  # mock it
                else:
                    try:
                        out = subprocess.check_output(cmd.split())
                    except subprocess.CalledProcessError:
                        printfl('Command "%s" failed' % cmd)
                        quit(1)
                    out = str(out, 'ascii')  # decode
                    for line in out.split('\n'):
                        m = re.match(r'^Throughput\s+([0-9]+\.[0-9]+)\s+Mpps', line)
                        if m:
                            mpps = float(m.group(1))
                            mpps_values.append(mpps)
                            printfl("Got %f Mpps" % mpps)
                    if k >= args.min_trials:
                        # We have reached the minimum number of trials. Let's
                        # see if standard deviation is small enough that we
                        # can stop.
                        mean = statistics.mean(mpps_values)
                        if k > 1:
                            stddev = statistics.stdev(mpps_values)
                        else:
                            stddev = 0.0
                        if mean != 0 and stddev / mean  < 0.01:
                            break
            results[(spin_p, spin_c)][queue] = mpps_values
except KeyboardInterrupt:
    printfl("Interrupted. Bye.")
    quit(1)

# Command line invocation
printfl(' '.join(sys.argv))

printfl(('%8s ' * 15) % ('P', 'C', 'ideal', 'lq', 'std', 'llq', 'std', 'blq', 'std',
                    'ffq', 'std', 'iffq', 'std', 'biffq', 'std'))
for (p, c) in results:
    slo = max(p, c)
    if slo != 0:
        maxr = 1000.0/slo
    else:
        maxr = 1000.0
    row = [p, c, maxr]
    for queue in queues:
        avg = statistics.mean(results[(p, c)][queue])
        if args.min_trials > 1:
            std = statistics.stdev(results[(p, c)][queue])
        else:
            std = 0.0
        row.append(avg)
        row.append(std)
    fmt = '%8s ' * 2
    fmt += '%8.2f ' * 13
    printfl(fmt % tuple(row))
