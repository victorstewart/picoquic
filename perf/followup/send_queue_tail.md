# Queued-write tail-pointer experiment

`send_queue_tail.patch` changes the two append paths to retain the last queued
node. It covers application STREAM writes and TLS CRYPTO writes because both use
`picoquic_stream_head_t`. It resets the pointer when the last node is formatted,
when a reset discards queued data, and when a stream is cleared. The existing
test that manually seeds a TLS queue also initializes the tail.

Apply the candidate only to a clean checkout at `bc3793cb`:

```sh
git apply perf/followup/send_queue_tail.patch
```

`send_queue_bench.c` calls the real `picoquic_add_to_stream` API on real,
initialized connections. Stream lookup, stream scheduling updates, allocations,
and copying are deliberately inside timing. Queue clearing occurs between timed
batches; it is identical setup for both variants and is excluded. Run the
correctness oracle first. It uses actual STREAM formatting to check payload
order after a partial drain followed by append, a complete drain followed by
refill of the still-open stream, and reset discard. It also formats a manually
seeded TLS queue through empty-to-refill. Candidate builds verify the tail after
each transition.

Build with matching headers and libraries from the candidate snapshot. The
explicit define enables candidate-only tail invariant assertions:

```sh
python3 perf/followup/build.py perf/followup/send_queue_bench.c \
  ../perf-artifacts/send-queue-tail /tmp/send_queue_bench \
  --define PICOQUIC_SEND_QUEUE_TAIL
/tmp/send_queue_bench --correctness
/tmp/send_queue_bench 1000 64 200
```

Run matched AB/BA pairs primarily at population `1000`, depths `1`, `8`, `64`,
and `256`, with enough batches to amortize setup. Population `1` is optional
microbenchmark evidence because timer and measurement overhead dominate more
readily. After each timed batch, the harness validates every queued node's
count, payload order, and offset, derives the checksum from that observation,
then clears it. `picoquic_clear_stream` deliberately does not decrement
`nb_bytes_queued`; since this fixture owns one app stream per connection, the
harness restores that telemetry counter to zero after every validation so it
cannot accumulate across batches or affect later scheduler decisions. The
intended result is a common-case write queue test, not a network throughput
claim.

The baseline is compiled against `followup-base`, without the tail define.
The checked-in configuration and raw samples contain the actual timed commands.

## Measured proof

Native M2 Max, matched Release builds, baseline `bc3793cb`, 12 alternating pairs
after one discarded warm-up per variant. Positive values mean less CPU per append.
Every operation checksum matched.

| Queue depth, 1,000 connections | Median reduction | Bootstrap 95% interval |
| --- | ---: | ---: |
| 1 | -0.36% | [-1.14, 0.91] |
| 8 | 2.76% | [2.10, 3.44] |
| 64 | 75.53% | [74.99, 75.80] |
| 256 | 94.95% | [94.81, 95.09] |

The depth-64 median cost fell from 137.895 to 33.975 ns per append; depth 256
fell from 692.411 to 34.998 ns. This removes a queue walk from every append.
The common single-write case has no measured material regression. The cost is
one additional pointer per stream, including TLS streams; there is no new
allocation or external dependency.

The 10,000-connection/four-stream protocol comparison was +1.01% [-0.43, 2.65],
below the 5% target. Existing bulk multiconnection was -0.28% [-1.87, 1.52];
existing bulk loss was +0.45% [-2.00, 2.80]. Therefore this is a queued-write
CPU win, not a demonstrated general network-throughput win.

Proof: `results/send-queue-tail.jsonl`,
`results/send-queue-tail-remaining.jsonl`, and `results/loss-controls.jsonl`.
The persistent-loss fixture is inconclusive because the unchanged baseline also
hits an idle timeout; see `RESULTS.md`. Candidate-only invariant checks and the
four focused stream/frame tests passed. Combined Release and ASan/UBSan checks
are recorded in `results/combined-correctness.log`. Binary/header identities are
in `results/identities.json`.

The final combined build also passed the 15-case cross-usage regression matrix:
active send, DATAGRAM, multipath, direct receive, reset/FIN, MTU, connection
lifecycle, and separate scheduler polling/churn states. No row showed a
repeatable slowdown. Combined real-TLS setup CPU was unchanged (+0.06%
[-0.53, 0.38]); queue-only setup was -0.48% [-1.32, 0.30]. Detailed baselines,
metrics, intervals and finite-coverage limits are in `RESULTS.md`; raw proof is
in `results/cross-usage*.jsonl` and `results/persistent-controls.json`.
