#!/usr/bin/env python3
"""Time repeated existing self-validating protocol tests, including their setup."""
import argparse
import hashlib
import re
import resource
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary')
p.add_argument('repetitions', type=int)
p.add_argument('tests', nargs='+')
a = p.parse_args()
if not 1 <= a.repetitions <= 100:
    p.error('repetitions must be between 1 and 100')
before = resource.getrusage(resource.RUSAGE_CHILDREN)
wall_start = time.monotonic_ns()
passed = 0
hashes = []
for _ in range(a.repetitions):
    proc = subprocess.run([a.binary, '-n'] + a.tests, capture_output=True, text=True)
    started = re.findall(r'^Starting test number \d+, (\S+)$', proc.stdout, re.M)
    successes = re.findall(r'^    Success\.$', proc.stdout, re.M)
    matches = re.findall(r'Tried (\d+) tests, (\d+) fails\.', proc.stdout)
    expected_summary = [] if len(a.tests) == 1 else [(str(len(a.tests)), '0')]
    if (proc.returncode or sorted(started) != sorted(a.tests) or len(successes) != len(a.tests)
            or matches != expected_summary):
        print(proc.stdout, end='')
        print(proc.stderr, end='')
        raise SystemExit(proc.returncode or 1)
    passed += len(successes)
    hashes.append(hashlib.sha256((proc.stdout + proc.stderr).encode()).hexdigest())
after = resource.getrusage(resource.RUSAGE_CHILDREN)
cpu = ((after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)) * 1e9
wall = time.monotonic_ns() - wall_start
print(f'tests_passed={passed} repetitions={a.repetitions} '
      f'cpu_ns_op={cpu / a.repetitions:.3f} wall_ns_op={wall / a.repetitions:.3f}')
print('successful_child_output_sha256=' + ','.join(hashes))
