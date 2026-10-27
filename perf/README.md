# Ordered-index experiments on native macOS

## Frozen scope and decision rule

Base: upstream `master` fetched on 2026-10-26, commit
`1d81a730ebbc0f4598f8127a7f43260a71a9ac7d` (2026-10-25).
Branch: `codex/ordered-index-perf`. The existing ready FIFO remains the scheduler policy.
The user's priority is many persistent connections with a few streams each.

This finite sweep covers the six ordered-index families in the supplied proposal:

| Family | Mechanisms to compare with upstream |
| --- | --- |
| Future connection timers | Cached minimum; indexed binary min-heap; compact C ordered pages |
| Stream lookup | Compact C ordered pages, tiny/hot and large/distributed maps |
| Receive reassembly | Compact C ordered pages, small/deep reordering |
| ACK intervals | Compact C ordered pages; inline sorted array with tree overflow |
| Lost STREAM data queue | Compact C ordered pages, small/large minimum queues |
| Token reuse | Compact C ordered pages, lookup/admission/expiry |

The hash and outstanding-packet paragraph in the proposal gives conditional profiling
ideas, not concrete replacements. These are recorded separately; no arbitrary hash
or packet-list redesign is part of this ordered-index sweep. The C compact-page
prototype tests the layout hypothesis, not the Rust bbtree implementation or its
SIMD/prefix-compression claims.

First screen mechanisms on identical deterministic operation sequences. Screening
is not evidence of a full Picoquic throughput improvement. Integrate viable timer
alternatives and any other candidate justified by the priority workload, then run
actual API and protocol checks before retaining production changes. Preserve the
source of rejected experiments and their results outside the production library.

Metrics: native process CPU time and elapsed time per completed operation; protocol
loopback time, delivered bytes and packet count; allocation footprint where measured.
Reference populations: 10,000 future connections (also 1,000), 2 live streams per
connection; larger stream/reassembly/range populations are secondary stress cases.

Each measured comparison uses a discarded warm-up for each variant and 12 paired
samples, with AB/BA order alternating, serial execution, identical Release flags,
seeds and work counts. Report median paired percentage reduction and a deterministic
bootstrap 95% interval. An eligible win is at least 5% lower median CPU cost in a
priority workload with the interval above zero, correct output, no reproducible
material regression (>5%) in its relevant controls, and acceptable memory/lifecycle
cost. Whole-protocol controls must pass; their speedups are claimed only if measured.
Borderline or contradictory results are inconclusive, not wins. At most three named
mechanisms are evaluated for any one family; do not expand this into an open backlog.

Every retained production mechanism gets its own commit with exact evidence paths.
The report distinguishes screening, integrated measurements, correctness, and
unavailable evidence. No remote branch push is included in this local experiment.

## Measurement calibration discovered during the runs

The original strict byte/packet equality check rejected loopback comparisons.
A four-pair upstream-versus-upstream calibration (`results/aa.jsonl`) reproduced
wire-byte variation and one-packet variation with identical binaries and payloads.
The protocol tests seed the scenario, but cryptographic handshakes remain variable.
Subsequent protocol comparisons require exact delivered application bytes and
at most **0.001%** difference in packets and wire bytes per pair. This is an output
comparability calibration, not a relaxed performance threshold. All raw counts
remain recorded. Synthetic index/API traces still require exactly equal checksums.
The initial strict results/config are preserved rather than silently rewritten.

Timing runs are serial; compilation and sanitizer work run outside timing windows.
The Mac is not CPU-pinned, and hardware cache-miss counters are not collected.
Bootstrap intervals describe these local samples, not independent replication
across machines or a deployment throughput guarantee.
