# Native Mac follow-up: persistent connections and three mechanisms

**Outcome:** retain constant-time send-queue append and metadata-only packet
initialization, each with its own proof commit. Reject in-place heap updates
because they slow monotonic updates. The previously retained timer heap remains.
All 15 added cross-usage comparisons completed without a repeatable slowdown;
Release and ASan/UBSan correctness checks passed. No external dependency is added.

This phase evaluates exactly three new mechanisms against the retained indexed
heap (`bc3793cb`): constant-time send-queue append, in-place future-heap updates,
and metadata-only packet initialization. It also tests the retained heap against
upstream under many persistent connections with one or four streams.

Upstream master was checked again on 2026-10-27 and remains
`1d81a730ebbc0f4598f8127a7f43260a71a9ac7d`; see
`results/upstream-verification.json`. All work ran natively on the M2 Max Mac,
on local branch `codex/ordered-index-perf`. The original checkout was preserved.
Compiler and dependency configuration match the first phase: Apple clang 21,
arm64, Release `-O3`, OpenSSL 3.6.2, picotls `f06553b`, Fusion disabled.

## Dependency and index choices

No retained change adds an external dependency. Upstream's ordered indexes use
the intrusive `picosplay` splay tree, not a B-tree. The earlier cached-minimum
alternative keeps that tree; the selected timer heap is an internal C
implementation. Queue tails and packet clearing are independent of ordered
indexes. The earlier compact-page screens were experimental C code, not Rust
`bbtree`, SIMD search or prefix compression. Their large-index gains do not prove
that a small modification to the existing splay layout would achieve the same
result, and none of those compact-page implementations is retained in production.

## Method and interpretation

Each reported comparison uses 12 serial, alternating AB/BA pairs following a
discarded warm-up per variant. Positive percentages mean lower CPU cost, except
the existing `perf_loopback` tests, whose metric is their reported elapsed time.
Intervals are paired-median bootstrap 95% intervals. The target remains at least
5% median reduction, interval above zero, exact application/operation checksums,
and no reproducible material control regression above 5%.

Performance binaries use normal crypto randomness. The persistent harness uses
real TLS and QUIC processing in one context, with 1,000 or 10,000 client/server
connection pairs. Thus 10,000 pairs means 20,000 endpoint connection objects,
and CPU timing includes both endpoints, not only a deployed server. There are
no operating-system UDP sockets in this harness.
Connection establishment and stream priming are outside the
message timing region. Every eighth round sends a burst; other rounds send fewer
messages, with a 20 ms simulated idle period between rounds. Every message's
content, stream, connection and sequence are checked. The network is simulated:
the reported p99 is **simulated delivery latency**, not real socket latency.
The pool and queue benchmarks exercise real Picoquic APIs and include their
payload work; their percentages are not network-throughput percentages.

Configurations, commands, stdout/stderr, raw samples and binary/source SHA-256
identities are retained in `results/`. `summarize_completed.py` derives only
complete comparisons and records incomplete cases explicitly.
`analyze_controls.py` reports packet counts, CPU per packet, wall cost, RSS and
simulated p99 without editing the original samples.

## Persistent workload: retained heap versus upstream

These compare the previously retained heap alone against pinned upstream;
they do not add the queue or packet changes. Do not add percentages across
experiments. All application checks match exactly.

| Connection pairs / streams / rounds / payload | CPU reduction [95% interval] | Interpretation |
| --- | ---: | --- |
| 1,000 / 1 / 2,048 / 64 B | 10.32% [7.70, 13.71] | Exploratory; short A/A was noisy |
| 1,000 / 1 / 32,768 / 64 B | 6.34% [-0.09, 12.69] | Inconclusive; confirmation interval crosses zero |
| 10,000 / 1 / 4,096 / 64 B | 13.26% [11.06, 13.76] | Measured local workload gain |
| 10,000 / 4 / 2,048 / 1,024 B | 55.14% [53.68, 56.66] | Strongest measured local workload gain |

The independent longer 1,000-pair A/A control is -1.09% [-6.21, 8.48]; the
10,000-pair/four-stream A/A control is +0.68% [-1.03, 4.09]. The Mac is not CPU
pinned and individual samples vary. The larger gains are evidence for these
native encrypted in-process workloads, not a cross-machine or real-network
throughput guarantee. The original bulk workload did not show the same gain.

Packet-normalized CPU confirms the two larger results: 13.26% [11.06, 13.77]
for one stream and 55.15% [53.69, 56.66] for four. Maximum paired packet-count
variation is 0.0128% and 0.0272%, respectively; the independent four-stream A/A
control reaches 0.0204%. Exact application checks hold throughout. Simulated
p99 remains 127 us in both variants for both 10,000-pair cases. Heap-only
four-stream RSS high-water is 2.14% higher [1.52, 4.46]; that observed memory
cost is recorded rather than presented as a memory win. Setup CPU improves
12.00% and 11.71%, respectively.

Proof: `results/persistent-baseline.jsonl`,
`results/persistent-confirmation.jsonl`, and the derived
`results/persistent-controls.json`. The full source and workload details are
in `persistent_bench.c` and `persistent_bench.md`.

## Queue-tail and packet-initialization results

| Mechanism | Qualifying operation-level win | Relevant controls |
| --- | --- | --- |
| Send-queue tail | 75.53% less append CPU at depth 64; 94.95% at depth 256 | Depth 1 unchanged; persistent four-stream +1.01%, bulk -0.28%, bulk loss +0.45% |
| Metadata-only packet initialization | 12.55–29.02% less CPU for 64-byte pool cases | 1,200-byte cases improve 1.26–2.12%; persistent median 4.99917%, below target; bulk +2.13% |

The queue results include allocation, copying, stream lookup and scheduling in
the timed append call. The packet results include payload writes and checksum
reads. Both compare against the retained heap baseline. Neither result claims
an equally large network-throughput gain. All operation checksums match; relevant
successful controls show no reproducible >5% regression. The isolated packet
bulk-loss comparability failure is documented below.

Each retained change carries its own detailed proof: `send_queue_tail.md` and
`packet_init.md`, including complete intervals, raw-data paths, lifecycle checks,
and limitations. The in-place heap candidate below is not part of their build.

With both queue and packet changes applied together, the 10,000-pair/four-stream
workload improves 3.05% [2.14, 9.56] against the retained heap baseline. Existing
bulk loss improves 3.62% [2.76, 4.96], with exact application bytes and every
packet/wire count within the original 0.001% tolerance. Neither reaches the 5%
protocol-win target. This is a combined regression check, not an independent
additional optimization. Proof: `results/combined.jsonl`.

The same combined persistent samples show setup CPU unchanged: +0.06%
[-0.53, 0.38]. Queue-only setup is -0.48% [-1.32, 0.30], also inconclusive.
Thus excluding handshake from the primary metric does not conceal a measured
startup regression in these samples. The combined RSS high-water comparison is
-0.21% [-1.58, 1.71], with no resolved difference. These are process high-water
measurements; the extra stream pointer remains a real structural memory cost.

## Cross-usage regression results

The caller audits are `regression-audit-queue.md`, `regression-audit-packet.md`
and `regression-audit-timers.md`. Existing active-send, loss, queue, packet and
scheduler controls are reused. The following finite additional matrix fulfills
the user's request to inspect other usages of the changed structures. Each row
has 12 complete pairs, with all configured equivalence checks passing.

| Control | Reduction [95% interval] |
| --- | ---: |
| Active send, single stream, 500 MB | 2.14% [1.91, 2.66] |
| Active send, 500 streams, 500 MB | 2.25% [1.68, 3.16] |
| DATAGRAM, 100-byte frames | 0.75% [-0.98, 1.59] |
| DATAGRAM, four flows of 1,300-byte frames | 0.65% [0.03, 1.20] |
| Multipath race, delayed race, settled | -0.35% [-1.25, 0.21] |
| Direct receive | 0.84% [-1.03, 1.67] |
| Reset/FIN terminal queues | 0.88% [0.58, 1.58] |
| MTU discovery and maximum | 1.16% [-0.76, 2.22] |
| Connection limit and stress | 0.83% [0.03, 1.30] |
| Poll delay, four contexts | 26.07% [25.80, 26.45] |
| Poll delay, empty | 39.87% [39.50, 40.23] |
| Poll delay, future | 12.39% [12.26, 12.98] |
| Poll delay, ready | 10.34% [9.95, 11.00] |
| Poll delay, capped | 13.04% [12.72, 13.28] |
| Create/reschedule/delete churn | 0.22% [-0.50, 0.40] |

The first two rows compare combined queue+packet against the retained heap and
measure reported elapsed time. All others compare the final combined build
against pinned upstream and measure CPU. Protocol-test batches use the existing
self-validating tests and include process startup, handshake and teardown. Their
successful test names/counts must match; their timing is not isolated formatter
CPU. Scheduler controls call real APIs on real contexts/connections, validate
delay/checksum and final-empty invariants, and measure each polling state
independently. Churn starts with capacity one, grows to 64 connections, moves
timers through ready/future/UINT64_MAX and destroys everything.

No row shows a repeatable slowdown, including below the original 5% material
threshold. This is evidence for the named workloads and shared API paths on this
Mac, not proof of zero slowdown for every possible application, platform or
traffic mixture. Actual OS sleep/socket performance and each individual
QinQ/media application are not separately measured.

Proof: `results/cross-usage.jsonl` (first two complete cases) and
`results/cross-usage-remaining.jsonl` (remaining thirteen); corresponding configs
and complete summaries identify the exact commands and samples. Release smoke
checks passed for upstream and combined; the combined sanitizer build passed all
13 added protocol tests plus scheduler polling/churn, recorded in
`results/cross-usage-smoke.log` and `results/cross-usage-sanitize.log`.

## Rejected: in-place future-heap updates

The candidate refreshes the FIFO tie sequence and repairs an existing heap slot
instead of removing and reinserting the connection. The scheduler oracle and
five lifecycle tests passed, including forced sequence renumbering during updates.

Initial 12-pair results showed query-heavy CPU down 22.60% [19.86, 24.54], random
update/query down 10.70% [8.79, 13.61], and randomized updates down 7.01%
[3.11, 7.64]. Ready-only was effectively unchanged; mixed ready/future was 2.45%
slower. However, monotonic updates were 8.56% slower [6.79, 11.12].

A separately run, longer confirmation with 30 million operations per process
confirmed a **13.87% slowdown [9.82, 14.65]**. All 12 confirmation pairs regressed.
That fails the frozen control rule, so this candidate is rejected. The persistent
10,000-connection result was only +2.67% [-0.62, 4.36]; it supplies no compensating
protocol win. Existing bulk/loss controls showed no material change.

Sources and proof: `heap_update.patch`, `results/heap-update-remaining.jsonl`,
`results/heap-update-confirmation.jsonl`, and `results/loss-controls.jsonl`.
The production scheduler remains the previously retained heap.

## Measurement problems preserved in the record

The initial short 1,000-connection A/A comparison of an unchanged binary showed
an apparent 6.30% gain [1.22, 13.85]. That is a warning about short-run noise, not
an optimization. The initial 10.32% heap result on that workload is exploratory;
longer samples are used for confirmation. Normal crypto randomness also changes
packet counts slightly despite exact application content. Packet-normalized CPU
is therefore reported as a sensitivity check, with the independent A/A spread.

The persistent-loss fixture intermittently advances to the configured
600-second simulated idle timeout (`PICOQUIC_ERROR_IDLE_TIMEOUT`, local reason
1075), including on the unchanged baseline. No scheduling or timeout workaround
was added. Failed/partial traces and close diagnostics are retained; this fixture
provides **no valid performance conclusion**. Existing `perf_loopback_loss`
comparisons and correctness tests provide separate loss coverage.

The isolated packet-init bulk-loss comparison delivered identical application
bytes but one pair had 372,427 versus 372,422 packets, exceeding the frozen
0.001% tolerance. Its apparent 3.23% timing improvement is not accepted as proof.
The tolerance was not relaxed, and the raw mismatch remains visible.

The initial A5/5A payload-poison check accidentally reused the A5 object for both
builds. `packet-poison-initial-invalid-5a.json` preserves that invalid evidence.
The corrected 5A build forces object recompilation; all four binary hashes are
distinct. All four wire cases then matched byte-for-byte across full clearing,
metadata clearing, A5 fill and 5A fill. The deterministic RNG and poison fills
are correctness-only and absent from performance and production builds.

The first cross-usage batch stopped after the two successful bulk comparisons
because the wrapper expected a summary line for a single test. Picoquic omits
that line and executes multiple named tests in test-table order. The corrected
wrapper requires every requested test name and success, checks the optional
summary and exit code, and passed all seven single/multiple groups against both
binaries before timing resumed. The interrupted run and parser-check log remain
in `results/`; no production change or failed protocol test was involved.

## Correctness and lifecycle

The queue oracle checks actual STREAM formatting after partial drain plus append,
empty-to-refill, FIN and reset, and TLS CRYPTO empty-to-refill. Candidate builds
enable `PICOQUIC_SEND_QUEUE_TAIL` to assert the tail invariant. Every timed batch
also verifies node count, order, offsets and payload before cleanup.

Packet checks cover pool metadata reset and reuse; the wire differential covers
small messages, small messages with loss, four streams with loss, and full-size
messages with loss. Nine padding/MTU/packet/frame tests passed with each poison
pattern. This is finite exercised-path evidence; ASan/UBSan alone cannot detect
uninitialized payload reads.

The combined queue-plus-packet build passed, in both Release and ASan/UBSan:

- 16 selected core tests covering connection/wake lifecycle, streams, frame
  formatting, retransmit copy, packet encryption, padding and MTU handling.
- Queue and packet standalone correctness oracles.
- 1,000 persistent connection pairs with four streams and validated messages.
- Existing multiconnection and loss protocol smoke tests, 5 MB each.

Commands and output are in `results/combined-correctness.log`; both clean build
logs are retained. Root diff review and the focused candidate reviews found no
unmaintained queue mutation. The queue adds one pointer per stream, including
the four embedded TLS streams; packet initialization adds no fields or allocations.

For reproduction, use the matching-source snapshot table and build commands in
`README.md`, then run `python3 perf/run_ab.py CONFIG NEW_OUTPUT.jsonl --pairs 12`
from the repository root. Configurations use this Mac's absolute paths. Derive
complete-case summaries with `python3 perf/followup/summarize_completed.py` and
persistent secondary metrics with `python3 perf/followup/analyze_controls.py`.
Failed and incomplete fixtures remain explicitly excluded; no remote push was
performed.
