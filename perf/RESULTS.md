# Native Mac ordered-index experiment results

The subsequent [persistent-connection and queue/pool experiments](followup/RESULTS.md)
add targeted workloads and three further mechanisms. This report preserves the
original ordered-index sweep and its measurements.

The final production choice is an **indexed future-timer heap**, retaining the
existing ready FIFO. The smaller cached-minimum alternative also wins and is
preserved in commit `7714e4c0`; the heap replaces it, so their percentages
must not be added. The compact token replacement is rejected.

All six proposed ordered-index families were screened. Only timers and tokens
were integrated into Picoquic for this sweep. Large stream maps, deep receive
reassembly and ACK-gap cases have promising layout-screen results, **not proven
production wins**. This report does not claim that every possible implementation
of those ideas has been exhausted.

## Baseline and decision

Upstream master: `1d81a730ebbc0f4598f8127a7f43260a71a9ac7d`, dated 2026-10-25,
fetched 2026-10-26. Branch: `codex/ordered-index-perf`, in a separate worktree;
the original checkout was preserved. The upstream ready FIFO was already present
(`4e87515c`, 2026-05-04). Priority: many persistent connections with few streams.

Apple M2 Max, macOS 26.6.2, Apple clang 21, Release `-O3 -DNDEBUG`, arm64,
OpenSSL 3.6.2, Fusion disabled. Picotls and binary identities are recorded in
`results/environment.json` and `results/identities.json`. Everything ran natively
on this Mac. No CPU affinity or hardware cache counters were available in this run.

Each comparison has 12 alternating AB/BA pairs after one discarded warm-up per
variant. Tables show median paired CPU-cost reduction and a bootstrap 95%
interval; positive is faster. The eligibility threshold was >=5%, interval above
zero, identical operation checksums, no reproducible >5% material control
regression, and acceptable memory/lifecycle cost. These are local sample intervals,
not a cross-machine guarantee. See `README.md` for the full frozen rule.

## Integrated scheduler results

The confirmation fixture allocates each connection separately, matching the
production allocation pattern. It calls the real scheduler APIs on initialized
minimal fixtures. Most populations are 10,000; the 1,000 row is explicit.

| Workload | Cached minimum vs upstream | Heap vs upstream | Heap vs cached minimum |
| --- | ---: | ---: | ---: |
| Query-heavy, 1 update / 32 queries | +48.14% [+47.87, +48.44] | +75.65% [+75.46, +75.77] | +54.67% [+54.24, +54.89] |
| Random update + query | +6.43% [+5.74, +6.69] | +87.47% [+87.33, +87.59] | +87.03% [+86.96, +87.15] |
| Randomized control | +6.92% [+6.68, +7.62] | +87.45% [+87.38, +87.51] | +87.04% [+86.91, +87.16] |
| Monotonic updates | +6.92% [+5.87, +7.50] | +12.65% [+12.34, +12.94] | +9.21% [+8.68, +9.47] |
| Ready FIFO only | +1.43% [-0.15, +1.53] | +25.03% [+23.44, +25.65] | +23.83% [+23.54, +25.34] |
| 8 ready, random future deadlines | +18.87% [+18.23, +21.48] | +22.48% [+22.42, +22.70] | -1.91% [-2.13, -0.73] |
| 8 ready, ordered future deadlines | +52.55% [+51.31, +53.10] | +54.19% [+53.27, +54.36] | +2.71% [+2.09, +4.12] |
| Query-heavy, 1,000 connections | +42.24% [+41.99, +42.85] | +68.05% [+67.98, +68.23] | — |
| Random future deadlines, no updates | +34.48% [+34.35, +34.81] | +32.15% [+31.51, +32.32] | — |

For the query-heavy 10,000-connection heap run, median cost fell from 42.983 to
10.415 ns/iteration. Random update/query fell from 509.225 to 63.734 ns/iteration.
These are scheduler operations, not packet throughput. The separate pathological
ordered/static trace creates a long upstream splay spine and has a much larger
reduction; its raw results are preserved but it is not the headline workload.

Authoritative timing files: `cached-min-v2-separate`, `heap-separate`, and
`heap-vs-cache` (`.jsonl`, `.summary.json`, and `-config.json` in `results/`).

The heap preserves deadline ordering, FIFO ties, ready/future transitions and
cutoffs. Direct connection handles support removal without searching. Capacity is
reserved before a connection is registered, so the void reschedule API cannot
fail allocation. Connection creation grows capacity if clients exceed the initial
configured limit; allocation failure rejects the new connection. Equal-deadline
sequence wrap is tested. The retained patch is `timer_heap.patch`.

On this build `sizeof(picoquic_cnx_t)` falls from 4,936 to 4,928 bytes. The context
adds a pointer array initially sized to its configured maximum (80,000 bytes for
10,000 connections), doubling if exceeded. Thus sparse contexts pay for unused
slots; this is not a memory-reduction claim. Cached minimum adds one context
pointer. Heap sequence-wrap renumbering is O(n²), but occurs only after roughly
2^64 future insertions. See `timer_heap.md` for lifecycle details.

## Whole-protocol controls

500 MB application payload per run, 12 pairs. These use the same production
libraries as the confirmation timer binaries; fixing the standalone fixture did
not change the library. The multiconnection test uses 200 connections with one
stream each; multistream uses 500 streams. Loss uses the existing 1/64 drop mask.

| Protocol workload | Cached minimum reduction | Heap reduction |
| --- | ---: | ---: |
| `perf_loopback_multiconn` | -0.69% [-1.02, +0.97] | +1.43% [-0.02, +3.28] |
| `perf_loopback` | +0.04% [-0.34, +0.64] | -0.08% [-0.45, +0.16] |
| `perf_loopback_loss` | +0.32% [-0.21, +1.12] | -0.15% [-0.83, +0.23] |
| `perf_loopback_multistream` | +0.33% [-0.55, +5.88] | +1.26% [-0.23, +1.67] |

No whole-protocol speedup in this original bulk sweep meets the 5% target. The
gains above are confined to scheduler CPU cost. Application bytes match exactly.
Initial strict packet/wire
matching rejected some runs; an upstream-vs-upstream four-pair calibration
reproduced handshake-related variation. Later comparisons permit <=0.001%
packet/wire variation while requiring exact application bytes. All later pairs
pass that rule. Initial rejected data and calibration remain in `results/`.

## Compact layout screening

`index_screen.c` compares upstream intrusive splay with compact 32-entry leaves
and a sorted directory. The ACK alternative uses eight inline pointers with splay
overflow. It does not implement actual ACK coalescing, receive-buffer ownership,
or a full retransmission policy. Every timed pair has equal checksums.

| Trace | Candidate reduction |
| --- | ---: |
| `stream-small2-compact` | +1.72% [+1.17, +2.21] |
| `stream-hot2-compact` | -4.87% [-6.42, -3.50] |
| `stream-large4096-compact` | +57.22% [+56.87, +57.49] |
| `reassembly-4-compact` | +6.80% [+5.61, +7.47] |
| `reassembly-1024-compact` | +40.95% [+38.97, +42.39] |
| `ack-1-compact` | -11.48% [-12.05, -7.79] |
| `ack-1-ack-inline` | -2.55% [-3.12, -1.34] |
| `ack-8-compact` | +25.86% [+25.42, +26.23] |
| `ack-8-ack-inline` | +34.59% [+34.38, +34.92] |
| `ack-1024-compact` | +40.36% [+39.96, +40.64] |
| `ack-1024-ack-inline` | -3.97% [-5.32, -3.16] |
| `lost-8-compact` | -143.36% [-144.87, -141.57] |
| `lost-1024-compact` | -237.00% [-239.57, -233.57] |
| `token-10000-compact` | +54.29% [+53.59, +54.76] |
| `timers-10000-compact` | +42.63% [+42.04, +43.12] |

Disposition by family:

- **Streams:** do not replace the production index. Two-stream cases miss the
  target or regress; the 4,096-stream result is a secondary layout finding.
- **Receive reassembly:** retain upstream. Four-entry improvement is small and
  the compact prototype allocates at least 968 index bytes versus 96 embedded
  splay-link bytes for four entries; deep-reordering results merit a separately
  integrated experiment if that workload becomes a priority.
- **ACK ranges:** retain upstream. The usual one-range case regresses. Eight-range
  and large-range screens are promising but do not prove actual interval behavior;
  this prototype's inline array stores pointers, not inline interval payloads.
- **Lost STREAM data:** reject the compact minimum queue, which is much slower.
- **Tokens:** integrate and test, then reject based on the controls below.
- **Future timers:** compact pages screen positively, but the directly integrated
  indexed heap is the selected timer mechanism. No fourth timer mechanism was added.

This C layout experiment is not Rust bbtree, SIMD search or prefix compression.
It neither validates nor disproves those separate implementation claims.

## Integrated token experiment: rejected

`token_compact.patch` replaces the real reuse index with lazy compact pages,
preserving full expiry/hash keys, the cap, duplicate accounting and strict expiry.
`token_bench.c` calls actual admission/clear APIs, including object allocation and
expiry cost in admission batches. Repeated lookups preload outside timing.

| API workload | Compact reduction |
| --- | ---: |
| `admit-sequential-1` | -38.90% [-41.30, -37.11] |
| `admit-sequential-8` | +7.99% [+7.33, +8.16] |
| `admit-sequential-10000` | -54.21% [-54.89, -52.57] |
| `admit-shuffled-10000` | +53.83% [+53.31, +54.24] |
| `lookup-10000` | +56.55% [+55.83, +56.67] |
| `hot-10000` | -67.25% [-68.37, -66.18] |

Randomized lookup/admission wins do not compensate for severe sequential-admission
and hot-key regressions. No token-index production changes remain on the branch.
The patch and full raw evidence are retained for reproduction.

## Corrections and limits

The earliest timer fixture packed connections contiguously. Changing the
connection struct size changed its array stride and produced contradictory
ready-only regressions (about 11% for the heap). Production allocates connections
separately. The corrected fixture uses separate allocations for both variants;
all timer cases were rerun, then the two finalists were compared directly. Both
sets of raw results are retained, and `timer_bench_contiguous.c` preserves the v2
fixture. The earlier cached-min v1 is exploratory and superseded by v2.

The compact screen was corrected before timing to use the upstream intrusive
splay/delete-hint model, full-width compound keys and consistent predecessor
semantics. It is deliberately a small finite mechanism screen, not a generalized
new index library. No production bug fix is claimed by this work.

A five-second native `sample` profile of upstream's 200-connection, 20 GB loopback
found AES-GCM kernels prominent (513 and 491 leaf samples out of 4,192).
`picohash_retrieve` had nine leaf samples and `picoquic_dequeue_retransmit_packet`
13. These leaf counts are not full inclusive costs, but provide no clear evidence
that a hash-table or outstanding-packet index replacement would help this workload.
Those conditional suggestions were not implemented. Profile and workload log:
`results/verification/upstream-multiconn.sample.txt` and
`upstream-profile-workload.log`.

## Observed correctness checks

- Upstream and all timer finalists passed deterministic scheduler oracles; heap
  oracle includes forced sequence wrap. The new registered `wake_queue` test
  uses real connection creation/destruction, growth beyond initial capacity,
  equal-deadline order, 1,024 updates, UINT64_MAX and ready/future moves.
- Final heap Release checks passed: creation/lifecycle, getter, FIN-only wake,
  splay/stream/output/retransmit, ACK range/disorder/horizon/of-ACK, and token APIs.
- Final heap **16 core tests and 4 protocol tests passed under ASan+UBSan**.
  The scheduler oracle, including forced sequence wrap, also passed ASan+UBSan.
  The same compiler/dependencies were used; sanitizer build is RelWithDebInfo.
- A 1,000-connection test completed 60 simulated seconds and delivered 1,000
  messages, with no failures. This is simulation correctness, not network-scale
  throughput evidence.
- The compact screen passed ASan+UBSan self-check. Both token variants passed
  their independent API oracle; the integrated candidate passed six token and
  connection tests before rejection.

Verification logs are in `results/verification/`. The entire upstream test suite,
non-Mac targets, OOM fault injection and real network deployment were not run.

## Reproduction

1. Start from the exact upstream base above. Configure Release as below. Snapshot
   baseline binaries, core/log archives and `picoquic/*.h` into an `upstream`
   directory. Build a separate copy for each candidate using its retained patch;
   never mix variant headers and libraries.
2. Build `timer_bench.c` with `perf/build_timer.py` (add `--heap` only for the heap),
   `token_bench.c` with `--source`, or the screen command in `index_screen.md`.
   Use the build dependency archives from the matching configuration.
3. The configs record every argument, seed, population and iteration count. Their
   absolute paths identify this run. For another checkout, change only artifact
   prefixes and `cwd`; retain workload arguments. Execute, for example,
   `python3 perf/run_ab.py CONFIG NEW_OUTPUT.jsonl --pairs 12`.
4. Raw output and summaries are separate, and the runner refuses to overwrite
   samples. `--summary-only` recomputes the deterministic bootstrap from raw rows.
   Binary and source SHA-256 identities are recorded in `results/identities.json`.

```sh
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DPICOQUIC_FETCH_PTLS=ON -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DWITH_FUSION=OFF -DBUILD_PICO_SIM=OFF
cmake --build build-perf --target picoquic_ct picohttp_ct -j 8
```

For sanitizers, configure a separate directory with `RelWithDebInfo`,
`-DENABLE_ASAN=ON -DENABLE_UBSAN=ON` and the same remaining options.
Run the tests listed in the saved logs with `-S "$PWD" -n`; protocol checks used
`-p 10000000`. The stress invocation was `picoquic_ct -S "$PWD" -n -c 1 1000`.
Binaries/build directories remain local outside Git; code, patches, configurations,
raw samples, summaries and verification evidence are committed.
