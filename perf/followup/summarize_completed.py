#!/usr/bin/env python3
"""Summarize complete cases, retaining a record of failed/incomplete comparisons."""
import importlib.util
import json
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('run_ab', here.parent / 'run_ab.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
inventory = {}
for path in sorted((here / 'results').glob('*.jsonl')):
    config = json.loads(path.with_name(path.stem + '-config.json').read_text())
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    completed = []
    status = {}
    for case in config['cases']:
        pairs = {}
        for row in rows:
            if row['case'] == case['name'] and row['phase'] == 'sample':
                pairs.setdefault(row['pair'], set()).add(row['variant'])
        if len(pairs) == 12 and all(v == {'A', 'B'} for v in pairs.values()):
            completed.append(case)
            status[case['name']] = 'complete; inspect equivalence_mismatches before use'
        else:
            status[case['name']] = 'incomplete or not reached; no performance conclusion'
    summary = runner.summarize(rows, {'cases': completed})
    path.with_suffix('.complete.summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    inventory[path.stem] = status
(here / 'results/completion-status.json').write_text(json.dumps(inventory, indent=2) + '\n')
