# Packet pool initialization follow-up

This is a pool-only microbenchmark for the proposed create-time change from
full packet clearing to metadata-only clearing. It calls the real
`picoquic_create_packet` and `picoquic_recycle_packet` APIs with a zeroed
`picoquic_quic_t` fixture containing only the pool fields those APIs use.

The metadata boundary is `offsetof(struct st_picoquic_packet_t, bytes)` in
`picoquic_internal.h`. `bytes` is the final member, so metadata initialization
does not touch packet payload storage. The benchmark writes and reads the
requested payload on every acquire before recycling it; the volatile checksum
prevents that work from being optimized out. Supported bench cases use depths
`1`, `64`, `1024` and payloads `64`, `1200`.

Build against the coordinator's immutable baseline snapshot:

```sh
python3 perf/followup/build.py perf/followup/packet_bench.c \
  ../perf-artifacts/followup-base /tmp/packet-bench
/tmp/packet-bench --correctness
/tmp/packet-bench --bench 64 64 15625
```

The correctness mode poisons a packet payload before recycling it, then checks
that all exercised metadata resets and pool counts are correct. It explicitly
does **not** require that payload bytes become zero: full-clear baseline and
metadata-clear candidate must both pass. It also performs intended payload
write/read work after every acquire.

This does not prove formatter, encryption, or retransmission paths never read
an unwritten byte. The protocol-level differential poison tests remain required
for that question; this harness only measures the packet pool operation.

## Measured proof

Native M2 Max, matched Release builds, baseline `bc3793cb`, 12 alternating pairs
after one discarded warm-up per variant. Positive values mean less CPU per
acquire/write/checksum/recycle operation. All checksums matched. The pool is
warmed with the full batch depth before timing; all tested depths fit within
the existing 8,192-packet pool limit.

| Pool depth | 64-byte payload reduction [95% interval] | 1,200-byte payload reduction [95% interval] |
| --- | ---: | ---: |
| 1 | 12.55% [12.07, 13.56] | 1.26% [0.17, 1.67] |
| 64 | 27.90% [27.19, 29.69] | 1.54% [0.98, 2.11] |
| 1,024 | 29.02% [27.81, 30.57] | 2.12% [1.58, 2.53] |

The small-payload cases meet the 5% target; none of the large-payload controls
regresses. The persistent 10,000-connection median reduction is 4.99917%
[3.21, 8.57], just below the 5% target: do not round it up into a qualifying
protocol win. Bulk multiconnection is +2.13% [1.46, 3.20]. The isolated bulk-loss
timing is inconclusive because one pair exceeds the frozen packet-count tolerance.
The change adds no fields, allocations, or dependencies.

Proof: `results/packet-init-remaining.jsonl` and its summary, plus
`results/packet-poison.json` and `results/packet-poison-wire.log`. The corrected
four-variant differential compares all emitted encrypted datagrams byte-for-byte
under full clearing, metadata clearing, A5 poisoning and 5A poisoning, in four
protocol scenarios. The first invalid 5A build is retained and explicitly named
`packet-poison-initial-invalid-5a.json`; it is not counted as verification.

Both poison patterns passed nine packet/padding/MTU/frame tests. The combined
queue-plus-packet build passed the Release and ASan/UBSan checks recorded in
`results/combined-correctness.log`. Neither sanitizer detects uninitialized reads
by itself; the formatter audit and differential poison checks are separate
evidence. Test-only poisoning and deterministic crypto are absent from production.

The combined build passed the added DATAGRAM (100/1,300-byte), multipath,
direct-receive, reset/FIN, MTU and connection-lifecycle tests under Release and
ASan/UBSan. All 15 added cross-usage A/B controls showed no repeatable slowdown;
the combined existing bulk-loss comparison also passed the original packet/wire
tolerance, with +3.62% elapsed-time reduction [2.76, 4.96]. See `RESULTS.md` and
`results/cross-usage*.jsonl` for exact baselines, metrics and limits. These checks
do not imply that every possible formatter or operating-system workload has been
timed.
