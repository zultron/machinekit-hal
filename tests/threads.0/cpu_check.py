#!/usr/bin/env python3
#
# cpu_check.py
#
# Check that all HAL threads are running on the same CPU

import hal
import os
import sys
from sh import halcmd, ps
import re
import time

err_file = 'cpu_check_errors.txt'
def fail(msgs):
    with open(err_file, 'w') as f:
        for msg in msgs.splitlines():
            f.write('ERROR:  {}\n'.format(msg))
        halcmd_cmd = halcmd.show.thread()
        f.write(halcmd_cmd.stdout.decode())
        ps_cmd = ps("-C", "rtapi:0", "-Lo",
                    "pid,tid,class,rtprio,ni,pri,psr,pcpu,stat,comm,args")
        f.write(ps_cmd.stdout.decode())
    raise SystemExit(1)

# Be sure we're in the right directory; remove results file
script_dir = os.path.dirname(__file__)
os.chdir(script_dir)
if os.path.exists(err_file):
    os.unlink(err_file)

# Create HAL component
h = hal.component("cpu_check")
h.ready()

# Wait for `fast` and `slow` threads to start running
status_regex = re.compile('^Realtime Threads.*currently ([^)]*)\)')
while True:
    output = halcmd.show.thread()
    lines = output.stdout.decode().splitlines()
    try:
        status = status_regex.search(lines[0]).group(1)
    except IndexError:
        fail('Error reading "halcmd show thread" output:\n' + '\n'.join(lines))
    if status == 'running':
        break

    time.sleep(0.2)

# Read `halcmd show thread` output to determine requested CPUs
cpus = []
for line in lines[2:]:
    if not line:
        continue  # Skip trailing blank line
    try:
        cpu = int(line.split()[2])
    except IndexError:
        continue
    if cpu == -1:
        fail("HAL thread CPU not set; `newthread cpu=0` arg broken?")
    cpus.append(cpu)
if len(cpus) != 2:
    fail("Expected exactly two threads; found %d" % len(cpus))
if cpus[0] != cpus[1]:
    fail("Threads not running different CPUs:  %d and %d" % cpus)

# Read `ps` output for more checks
err_msg = ''
for line in ps('-C', 'rtapi:0', '-Lo', 'class=,rtprio=,psr=,comm='):
    cls, rtprio, psr, comm = line.rstrip().split()
    if comm not in ('fast:0', 'slow:0'):
        continue
    if cls != 'FF':
        err_msg += 'Thread {} cls is {}, not FF\n'.format(comm, cls)
    if psr != '0':
        err_msg += 'Thread {} CPU is {}, not 0\n'.format(comm, psr)
    expected_rtprio = 98 if comm=='fast:0' else 97
    if int(rtprio) != expected_rtprio:
        err_msg += 'Thread {} RTPrio is {}, not {}\n'.format(
            comm, rtprio, expected_rtprio)
if err_msg:
    # Fail!!!
    fail(err_msg)
else:
    # Success!! Exit HAL component
    h.exit()
