# Ordered-index mechanism screen

`index_screen.c` is a native macOS-only experimental harness. It compares
the upstream *intrusive* `picosplay.c` path (the `picosplay_node_t` is embedded
in each caller-owned item, and deletion uses its known node) with a compact,
two-level B+-tree-like index: a sorted
directory of leaf maxima and 32-entry leaves that pack compound `uint64_t`
deadline and ordinal arrays beside stable object pointers. The compound key is
the full-width pair `(deadline, ordinal)`;
the ordinal makes equal deadlines deterministic. The third candidate is for
ACK-shaped workloads: eight inline sorted pointers, with overflow in the
current splay tree.

This is a mechanism screen, not a Picoquic change. It has no integration with
connection state, packet processing, allocator policy, interval coalescing, or
end-to-end traffic. In particular, the ACK workload only represents range-style
predecessor lookup and mutation; it is not an interval implementation or a
claim about ACK behavior. A single CLI invocation is not a benchmark conclusion. The paired sweep
and its limited screening conclusions are recorded in `RESULTS.md`.

Build and first verify correctness:

```sh
cc -std=c11 -Wall -Wextra -Werror -O3 -I./picoquic \
  perf/index_screen.c picoquic/picosplay.c -o /tmp/index-screen
/tmp/index-screen --selfcheck
```

The self-check uses deterministic random mutation traces and an independent
sorted reference. It checks exact and missing lookup, inclusive predecessor,
first, duplicates, empty and one-entry indexes, repeated clear, delete/reinsert,
and an in-place key update represented as erase/reinsert. It runs every
candidate against the same trace and rejects differing checksums. Test items
also exercise deadlines and ordinals near `UINT64_MAX`.
The checked traces use seed `0x1234`; command-line invocations keep the trace
seed explicit and print a checksum, so candidates can be compared only when
checksums match.

CLI form:

```text
/tmp/index-screen WORKLOAD VARIANT ITERATIONS SEED
```

Variants are `splay`, `compact`, and `ack-inline`. `ack-inline` is meaningful
for `ack` but remains available for parity checks. Output includes monotonic
wall and process-CPU nanoseconds, both ns/op, embedded link bytes per entry,
and index heap bytes. Splay has zero index heap bytes; its embedded-link cost is
reported separately. Compact heap bytes include leaf storage and directory
capacity, exclude caller-owned objects, and exclude allocator metadata.

| Workload | Initial entries | Trace mix |
| --- | ---: | --- |
| `stream-small2`, `stream-hot2`, `stream-large4096` | 2, 2, 4096 | 90% exact/hot lookup; 10% erase/reinsert key update |
| `reassembly-4`, `reassembly-1024` | 4, 1024 | predecessor queries, ordered first/drain probes, mutation |
| `ack-1`, `ack-8`, `ack-1024` | 1, 8, 1024 | range-style predecessor queries, first probe, mutation |
| `lost-8`, `lost-1024` | 8, 1024 | minimum/delete/reinsert queue behavior |
| `token-10000` | 10000 | lookup plus delete/reinsert expiration behavior |
| `timers-10000` | 10000 | first/timer query plus deadline update |

Finite case matrix (run serially):

```text
stream-small2, stream-hot2, stream-large4096  splay,compact
reassembly-4, reassembly-1024                 splay,compact
ack-1, ack-8, ack-1024                        splay,compact,ack-inline
lost-8, lost-1024                             splay,compact
token-10000, timers-10000                     splay,compact
```

Run each selected case with identical iterations and seed, one at a time, for
example:

```sh
/tmp/index-screen ack-1024 compact 1000000 42
/tmp/index-screen ack-1024 ack-inline 1000000 42
```

The raw paired sweep is in `results/index-screen.jsonl`; it supports only
layout-screen conclusions, not integrated protocol speedup claims.
