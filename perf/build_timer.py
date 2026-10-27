#!/usr/bin/env python3
"""Build the native macOS timer benchmark against a matching header/library snapshot."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--include', type=Path, default=Path('picoquic'))
p.add_argument('--libdir', type=Path, default=Path('build-perf'))
p.add_argument('--depsdir', type=Path, default=Path('build-perf/_deps/picotls-build'))
p.add_argument('--output', type=Path, default=Path('build-perf/timer_bench'))
p.add_argument('--heap', action='store_true')
p.add_argument('--source', type=Path, default=Path('perf/timer_bench.c'))
a = p.parse_args()
cmd = ['cc', '-O3', '-DNDEBUG', '-arch', 'arm64', '-Wall', '-Wextra', '-Werror']
if a.heap:
    cmd.append('-DPICOQUIC_TIMER_HEAP')
cmd += ['-I' + str(a.include), str(a.source), '-o', str(a.output)]
cmd += [str(a.libdir / name) for name in ('libpicoquic-core.a', 'libpicoquic-log.a')]
cmd += [str(a.depsdir / ('libpicotls-' + name + '.a')) for name in ('openssl', 'minicrypto', 'core')]
cmd += ['-L/opt/homebrew/opt/openssl@3/lib', '-lssl', '-lcrypto', '-L/opt/homebrew/opt/brotli/lib', '-lbrotlidec', '-lbrotlienc']
subprocess.run(cmd, check=True)
