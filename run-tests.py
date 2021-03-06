#!/usr/bin/env python
#
# Written by: Vincenzo Maffione <v.maffione@gmail.com>

import subprocess
import argparse
import sys
import re
import os
import itertools

def cmean(vec):
    try:
        import statistics
        return statistics.mean(vec)
    except:
        import numpy
        return numpy.mean(vec)

def cstddev(vec):
    try:
        import statistics
        return statistics.stdev(vec)
    except:
        import numpy
        return numpy.std(vec)

def printfl(s):
    print(s)
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
argparser.add_argument('-v', '--verbose', action = 'store_true',
                       help = "Be verbose")
argparser.add_argument('-w', '--disable-queue-prefetch', action = 'store_true',
                       help = "Try to prevent hw prefetcher from being effective "
                                "on prefetching queue slots")
argparser.add_argument('--only-queue', type = str, default = '',
                       choices = ['lq', 'llq', 'blq', 'ffq', 'iffq', 'biffq'],
                       help = "Test only a given queue")
argparser.add_argument('-H', '--hugepages', action = 'store_true',
                       help = "Use hugepages")

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
            flags = '-p'
            if args.disable_queue_prefetch:
                flags += 'w'
            if args.hugepages:
                flags += 'H'
            cmd = './spscq %s -D %d -l %d -L %d -b %d -b %d -P %d -C %d -t %s'\
                     % (flags, args.duration, args.queue_length, args.line_entries,
                        args.prod_batch, args.cons_batch, spin_p, spin_c,
                        queue)
            if args.indirect:
                cmd += ' -M'
            if args.exp_type == 'latency':
                cmd += ' -T'
            if args.verbose:
                printfl("Running '%s'" % cmd)
            mpps_values = []
            pmpp_values = []
            cmpp_values = []
            for k in range(1, args.max_trials+1):
                if args.dry_run or (args.only_queue != '' and queue != args.only_queue):
                    mpps_values.append(0)  # mock it
                    pmpp_values.append(0)  # mock it
                    cmpp_values.append(0)  # mock it
                else:
                    try:
                        out = subprocess.check_output(cmd.split())
                    except subprocess.CalledProcessError:
                        printfl('Command "%s" failed' % cmd)
                        quit(1)
                    try:
                        tmp = str(out, 'ascii')  # decode
                        out = tmp
                    except:
                        pass
                    for line in out.split('\n'):
                        m = re.match(r'^([0-9]+\.[0-9]+)\s+Mpps\s+([0-9]+\.[0-9]+)\s+Pmpp\s+([0-9]+\.[0-9]+)\s+Cmpp', line)
                        if m:
                            mpps = float(m.group(1))
                            pmpp = float(m.group(2))
                            cmpp = float(m.group(3))
                            mpps_values.append(mpps)
                            pmpp_values.append(pmpp)
                            cmpp_values.append(cmpp)
                            if args.verbose:
                                printfl("Got %f Mpps, %f Pmpp, %f Cmpp" % (mpps, pmpp, cmpp))
                    if k >= args.min_trials:
                        # We have reached the minimum number of trials. Let's
                        # see if standard deviation is small enough that we
                        # can stop.
                        mean = cmean(mpps_values)
                        if k > 1:
                            stddev = cstddev(mpps_values)
                        else:
                            stddev = 0.0
                        if mean != 0 and stddev / mean  < 0.01:
                            break
            results[(spin_p, spin_c)][queue] = (mpps_values, pmpp_values, cmpp_values)
except KeyboardInterrupt:
    printfl("Interrupted. Bye.")
    quit(1)

# Command line invocation
printfl(' '.join(sys.argv))

printfl((('%3s '*2) + ('%8s '*18)) % ('P', 'C',
                    'lq.T', 'llq.T', 'blq.T', 'ffq.T', 'iffq.T', 'biffq.T',
                    'lq.P', 'llq.P', 'blq.P', 'ffq.P', 'iffq.P', 'biffq.P',
                    'lq.C', 'llq.C', 'blq.C', 'ffq.C', 'iffq.C', 'biffq.C'
        ))
for (p, c) in sorted(results):
    row = [p, c]
    # Throughput
    for queue in queues:
        avg = cmean(results[(p, c)][queue][0])
        if args.min_trials > 1:
            std = cstddev(results[(p, c)][queue][0])
        else:
            std = 0.0
        row.append(avg)
        # std (standard deviation) is unused

    # Producer r/w cache misses
    for queue in queues:
        avg = cmean(results[(p, c)][queue][1])
        row.append(avg)

    # Consumer r/w cache misses
    for queue in queues:
        avg = cmean(results[(p, c)][queue][2])
        row.append(avg)

    fmt = '%3s ' * 2
    fmt += '%8.2f ' * 18
    printfl(fmt % tuple(row))
