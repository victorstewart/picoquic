#!/usr/bin/env python3
"""Build an experiment against one explicit, matching header/library snapshot."""
import argparse,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('variant',type=Path);p.add_argument('output',type=Path)
p.add_argument('--define',action='append',default=[]);p.add_argument('--sanitize',action='store_true')
a=p.parse_args();inc=a.variant/'include' if (a.variant/'include').exists() else Path('picoquic')
deps=Path('build-perf-sanitize' if a.sanitize else 'build-perf')/'_deps/picotls-build'
cmd=['cc','-arch','arm64','-Wall','-Wextra','-Werror','-I'+str(inc),'-I/opt/homebrew/opt/openssl@3/include']
cmd+=['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'] if a.sanitize else ['-O3','-DNDEBUG']
cmd+=['-D'+d for d in a.define]
if 'PERSISTENT_DETERMINISTIC' in a.define:cmd+=['-Wno-deprecated-declarations']
cmd+=[str(a.source),'-o',str(a.output)]
cmd+=[str(a.variant/f'libpicoquic-{n}.a') for n in ['core','log']]
cmd+=[str(deps/f'libpicotls-{n}.a') for n in ['openssl','minicrypto','core']]
cmd+=['-L/opt/homebrew/opt/openssl@3/lib','-lssl','-lcrypto','-L/opt/homebrew/opt/brotli/lib','-lbrotlidec','-lbrotlienc']
subprocess.run(cmd,check=True)
