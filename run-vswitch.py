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

description = "Experiment with vswitch, using SPSC queues"
epilog = "2018 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-n', '--max-num-clients', help = "Number of points to take",
                       type = int, default = 20)
argparser.add_argument('-D', '--duration',
                       help = "Duration of a test run in seconds",
                       type = int, default = 10)
argparser.add_argument('--max-trials', help = "Max test runs per point",
                       type = int, default = 10)
argparser.add_argument('--min-trials', help = "Min test runs per point",
                       type = int, default = 5)
argparser.add_argument('--exp-type', type = str, default = 'throughput',
                       choices = ['throughput', 'latency'],
                       help = "Experiment type")
argparser.add_argument('-l', '--queue-length', help = "Queue length",
                       type = int, default = 256)
argparser.add_argument('--Bv', dest = 'vswitch_batch', help = "Producer batch size",
                       type = int, default = 8)
argparser.add_argument('--Bc', dest = 'client_batch', help = "Consumer batch size",
                       type = int, default = 1)
argparser.add_argument('--dry-run', action = 'store_true',
                       help = "Don't actually run vswitch")
argparser.add_argument('-v', '--verbose', action = 'store_true',
                       help = "Be verbose")
argparser.add_argument('--only-queue', type = str, default = '',
                       choices = ['lq', 'llq', 'blq', 'ffq', 'iffq', 'biffq'],
                       help = "Test only a given queue")
argparser.add_argument('-x', '--reserved-cpu', action = 'append',
                        help = 'CPU to be reserved (can be used multiple times)')

args = argparser.parse_args()

queues = ['lq', 'llq', 'blq', 'ffq', 'iffq', 'biffq']
results = {}

if args.max_num_clients < 1:
    printfl("Error: max_num_clients < 1")
    quit(1)

if args.max_trials < 1 or args.min_trials < 1:
    printfl("Error: trials < 1")
    quit(1)

if args.max_trials < args.min_trials:
    args.min_trials = args.max_trials

step = 2 if args.exp_type == 'latency' else 1
points = [x for x in range(step, args.max_num_clients+1, step)]

try:
    for num_clients in points:
        results[num_clients] = {}
        for queue in queues:
            flags = '-p'
            if args.reserved_cpu:
                for cpu in args.reserved_cpu:
                    flags += ' -x %s' % cpu
            cmd = './vswitch %s -D %d -l %d -b %d -b %d -n %d -t %s'\
                     % (flags, args.duration, args.queue_length,
                        args.vswitch_batch, args.client_batch, num_clients,
                        queue)
            if args.exp_type == 'latency':
                cmd += ' -T'
            if args.verbose:
                printfl("Running '%s'" % cmd)
            mpps_values = []
            for k in range(1, args.max_trials+1):
                if args.dry_run or (args.only_queue != '' and queue != args.only_queue):
                    mpps_values.append(0)  # mock it
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
                        m = re.match(r'^Total rate ([0-9]+\.[0-9]+)\s+Mpps$', line)
                        if m:
                            mpps = float(m.group(1))
                            mpps_values.append(mpps)
                            if args.verbose:
                                printfl("Got %f Mpps" % (mpps))
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
            results[num_clients][queue] = mpps_values
except KeyboardInterrupt:
    printfl("Interrupted. Bye.")
    quit(1)

# Command line invocation
printfl(' '.join(sys.argv))

printfl((('%8s '*3) + ('%8s '*6)) % ('N', 'Bv', 'Bc',
        'lq.T', 'llq.T', 'blq.T', 'ffq.T', 'iffq.T', 'biffq.T'))

for num_clients in sorted(results):
    row = [num_clients, args.vswitch_batch, args.client_batch]
    # Throughput
    for queue in queues:
        avg = cmean(results[num_clients][queue])
        if args.min_trials > 1:
            std = cstddev(results[num_clients][queue])
        else:
            std = 0.0
        row.append(avg)
        # std (standard deviation) is unused

    fmt = '%8s ' * 3
    fmt += '%8.2f ' * 6
    printfl(fmt % tuple(row))
