#!/usr/bin/env python3
"""Derive persistent-workload controls without modifying original samples."""
import json
from pathlib import Path
import random
import statistics


def paired_summary(pairs, value):
    a = [value(pair['A']) for pair in pairs]
    b = [value(pair['B']) for pair in pairs]
    reductions = [100 * (1 - y / x) for x, y in zip(a, b)]
    rng = random.Random(22167)
    bootstrap = sorted(statistics.median(rng.choices(reductions, k=len(pairs)))
                       for _ in range(10000))
    return dict(a_median=statistics.median(a), b_median=statistics.median(b),
                median_reduction_percent=statistics.median(reductions),
                bootstrap_95_percent=[bootstrap[249], bootstrap[9749]])


def main():
    directory = Path(__file__).resolve().parent / 'results'
    output = {}
    for path in sorted(directory.glob('*.jsonl')):
        grouped = {}
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row['phase'] == 'sample' and 'measured_packets' in row:
                grouped.setdefault(row['case'], {}).setdefault(row['pair'], {})[row['variant']] = row
        for case, indexed_pairs in grouped.items():
            pairs = list(indexed_pairs.values())
            if len(pairs) != 12 or any(set(pair) != {'A', 'B'} for pair in pairs):
                output[f'{path.stem}/{case}'] = dict(
                    status='inconclusive: incomplete 12-pair comparison',
                    observed_pair_count=len(pairs))
                continue
            packets = [pair[v]['measured_packets'] for pair in pairs for v in ('A', 'B')]
            differences = [100 * (pair['B']['measured_packets'] / pair['A']['measured_packets'] - 1)
                           for pair in pairs]
            output[f'{path.stem}/{case}'] = dict(
                pairs=len(pairs),
                exact_application_match=all(pair['A'][key] == pair['B'][key]
                    for pair in pairs for key in ('checksum', 'delivered_messages', 'delivered_bytes')),
                measured_packet_min=min(packets), measured_packet_max=max(packets),
                max_abs_paired_packet_difference_percent=max(map(abs, differences)),
                packet_difference_percent=differences,
                cpu_per_message=paired_summary(pairs, lambda r: r['cpu_ns_op']),
                cpu_per_packet=paired_summary(pairs, lambda r:
                    r['cpu_ns_op'] * r['delivered_messages'] / r['measured_packets']),
                setup_cpu=paired_summary(pairs, lambda r: r['setup_cpu_ns']),
                setup_wall=paired_summary(pairs, lambda r: r['setup_wall_ns']),
                wall_per_message=paired_summary(pairs, lambda r: r['wall_ns_op']),
                rss_high_water=paired_summary(pairs, lambda r: r['rss_max_bytes']),
                simulated_p99=paired_summary(pairs, lambda r: r['simulated_p99_delivery_us']))
    (directory / 'persistent-controls.json').write_text(json.dumps(output, indent=2) + '\n')
    for name, result in output.items():
        print(name, json.dumps(result))


if __name__ == '__main__':
    main()
