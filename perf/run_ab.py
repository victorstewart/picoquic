#!/usr/bin/env python3
"""Serial, alternating paired A/B runner. Configuration is JSON, never a shell string."""
import argparse
import json
from pathlib import Path
import random
import re
import resource
import statistics
import subprocess
import time


def measured(command, cwd):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic_ns()
    proc = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    elapsed = time.monotonic_ns() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    sample = {
        "command": command, "returncode": proc.returncode,
        "wall_ns": elapsed,
        "process_cpu_ns": round((after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime) * 1e9),
        "stdout": proc.stdout, "stderr": proc.stderr,
    }
    sample.update({key: float(value) if "." in value else int(value)
                   for key, value in re.findall(r"\b([a-z_]+)=([0-9]+(?:\.[0-9]+)?)", proc.stdout)})
    match = re.search(r"Perf loopback: (\d+) app bytes, (\d+) wire bytes, (\d+) packets .*? in (\d+) us\.", proc.stdout)
    if match:
        sample.update(dict(zip(("app_bytes", "wire_bytes", "packets", "loopback_us"), map(int, match.groups()))))
    if proc.returncode:
        raise RuntimeError(json.dumps(sample))
    return sample


def summarize(rows, config):
    result = {}
    rng = random.Random(22167)
    for case in config["cases"]:
        pairs = {}
        for row in rows:
            if row["case"] == case["name"] and row["phase"] == "sample":
                pairs.setdefault(row["pair"], {})[row["variant"]] = row
        metric = case["metric"]
        reductions, aa, bb = [], [], []
        equality = []
        for pair in pairs.values():
            a, b = pair["A"], pair["B"]
            for key in case.get("equal", ["checksum"]):
                if key not in a or key not in b:
                    raise ValueError(f"Missing equality field {key}: {case['name']}")
                if a[key] != b[key]:
                    equality.append({"field": key, "a": a[key], "b": b[key]})
            for key, tolerance_percent in case.get("tolerance_percent", {}).items():
                if key not in a or key not in b:
                    raise ValueError(f"Missing tolerance field {key}: {case['name']}")
                if abs(b[key] / a[key] - 1) * 100 > tolerance_percent:
                    equality.append({"field": key, "a": a[key], "b": b[key], "tolerance_percent": tolerance_percent})
            aa.append(a[metric]); bb.append(b[metric])
            reductions.append(100 * (1 - b[metric] / a[metric]))
        if not reductions:
            continue
        bootstrap = sorted(statistics.median(rng.choices(reductions, k=len(reductions))) for _ in range(10000))
        result[case["name"]] = {
            "metric": metric, "pairs": len(pairs),
            "a_median": statistics.median(aa), "b_median": statistics.median(bb),
            "median_reduction_percent": statistics.median(reductions),
            "bootstrap_95_percent": [bootstrap[249], bootstrap[9749]],
            "paired_reductions_percent": reductions,
            "equivalence_mismatches": equality,
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--summary-only", action="store_true")
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    if args.summary_only:
        rows = [json.loads(line) for line in args.output.read_text().splitlines()]
    else:
        if args.output.exists():
            raise SystemExit(f"Refusing to overwrite samples: {args.output}")
        rows = []
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w") as output:
            for case in config["cases"]:
                for phase, pair in [("warmup", -1)] + [("sample", i) for i in range(args.pairs)]:
                    order = ("A", "B") if pair % 2 == 0 else ("B", "A")
                    for variant in order:
                        sample = measured(case[variant.lower()], config.get("cwd"))
                        sample.update(case=case["name"], variant=variant, pair=pair, phase=phase)
                        rows.append(sample)
                        output.write(json.dumps(sample) + "\n"); output.flush()
                print(case["name"], json.dumps(summarize(rows, {"cases": [case]})), flush=True)
    summary = summarize(rows, config)
    args.output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if any(value["equivalence_mismatches"] for value in summary.values()):
        raise SystemExit("A/B output mismatch; no performance conclusion is valid for those cases")


if __name__ == "__main__":
    main()
