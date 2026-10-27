#!/usr/bin/env python3
"""Correctness-only differential wire capture with deterministic test crypto."""
import hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2];art=root.parent/'perf-artifacts'
variants=['followup-base','packet-init','packet-poison-a5','packet-poison-5a']
cases=[('small',['1000','1','32','64','64','0','42']),('small-loss',['1000','1','32','64','64','1','42']),('four-stream-loss',['1000','4','32','1024','64','1','42']),('full-packet-loss',['1000','1','16','1200','32','1','42'])]
identities={v:hashlib.sha256((art/v/'persistent_deterministic').read_bytes()).hexdigest() for v in variants}
assert len(set(identities.values())) == len(variants), 'candidate binaries must have distinct identities'
rows=[]
for name,args in cases:
 expected=None
 for variant in variants:
  binary=art/variant/'persistent_deterministic';capture=art/variant/(name+'.wire.bin')
  env=os.environ.copy();env['PICOQUIC_WIRE_CAPTURE']=str(capture)
  proc=subprocess.run([str(binary)]+args,cwd=root,env=env,capture_output=True,text=True,timeout=120)
  if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
  contents=capture.read_bytes();digest=hashlib.sha256(contents).hexdigest()
  row=dict(case=name,variant=variant,command=[str(binary)]+args,returncode=proc.returncode,stdout=proc.stdout,stderr=proc.stderr,capture=str(capture),wire_bytes=len(contents),wire_sha256=digest,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
  rows.append(row)
  (root/'perf/followup/results/packet-poison.json').write_text(json.dumps(rows,indent=2)+'\n')
  if expected is None:expected=contents
  assert contents==expected,(name,variant,'wire mismatch')
  print(name,variant,'exact_wire_match',len(contents),digest,flush=True)
