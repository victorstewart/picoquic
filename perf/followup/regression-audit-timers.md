# Retained timer-heap regression audit

**Scope.** This is a read-only audit of retained indexed future-timer heap
commit `bc3793cb`. It excludes the rejected in-place future-update patch: its
30-million-operation monotonic control regressed 13.87% (95% interval
-14.65% to -9.82%), recorded in
`results/heap-update-confirmation.summary.json`. No builds or tests ran in
parallel with timing. This audit subsequently informed the finite cross-usage
validation requested by the user.

The heap owns future wakeups only. Due wakeups retain the ready FIFO. Existing
raw data is used only where the compared binaries and harness version match the
recorded identity: the corrected separately allocated fixture is
`perf/results/heap-separate.*`; native persistent and control evidence is in
`perf/followup/results/` and summarized in `perf/followup/RESULTS.md`.

## Caller and coverage matrix

| Scheduler use and representative callers | Timer mode exercised | Existing comparable evidence | Gap status |
| --- | --- | --- | --- |
| `sender.c`: packet preparation, send completion, pacing/exit wakeup, no-data `UINT64_MAX` reschedule, then `picoquic_get_earliest_cnx_to_wake` | Ready FIFO, future minimum, future update, ready/future transitions, no-data reschedule | Corrected `timer_bench` 1,000/10,000 cases: query, update, randomized, monotonic, ready-only, and mixed ready/future (`perf/results/heap-separate.summary.json`). Real loopback, multiconnection, multistream, and loss controls are in `perf/results/heap.summary.json`. | Steady state covered. Added real create/reschedule/delete churn; it includes cancellation and deletion in the complete lifecycle metric. |
| `streams.c` and `packet.c`: new stream data, reset/flow-control and received-packet work wake a connection immediately | Future-to-ready and FIFO dequeue | `timer-ready-only`, `timer-ready-mixed`, and `timer-ready-mixed-random` corrected fixture controls; persistent encrypted one/four-stream runs at 1,000/10,000 pairs (`followup/results/persistent-baseline.summary.json`). | Added `terminal-queues` reset/FIN control and scheduler churn. |
| `quicctx.c`: client/server creation, connection deletion, context free; heap reserve grows before a connection becomes schedulable | Initial allocation, reserve growth, removal of ready/future connections, teardown | `wake_queue` correctness test covers real creation beyond initial capacity, deletion, equal deadlines, ready/future moves, `UINT64_MAX`, and sequence wrap. Persistent setup is analyzed separately from message CPU. | Added `scheduler-churn` (initial capacity 1, 64 connections) and `context-lifecycle` protocol-test controls. |
| `sockloop.c` / `winsockloop.c`: external poll timeout via `picoquic_get_next_wake_delay` | Empty/idle context, future minimum, capped deadline | API correctness is indirect through timer oracle and normal loopback. | Added empty, future, ready, capped and four-context polling controls, calling the actual delay API. OS sleep and Windows execution are not measured. |
| `picohttp/picoquic_ns.c`, `cnxstress.c`, `qinqtest.c`, `warptest.c`, `mediatest.c`: select next wake among several QUIC contexts or paths | Multiple contexts, path/migration timing, concurrent future minima | Existing `perf_loopback_multiconn` is many connections in one context; persistent harness is also one context. | Added four-context polling plus connection stress and three active multipath protocol tests. QinQ/media/application-specific workloads are not separately timed. |

### Reused evidence and limits

- Corrected separate-allocation timer evidence covers the retained heap's direct
  scheduler operations at 1,000 and 10,000 connections. It supersedes the old
  contiguous-fixture ready-only result: production connections are individually
  allocated, while the old fixture changed object stride when the heap replaced
  the embedded splay node.
- Existing bulk, multiconnection, multistream and packet-loss loopback controls
  show no reproducible material retained-heap regression. The original
  `perf_loopback_loss` interval crosses zero. The follow-up persistent-loss
  fixture is explicitly invalid for performance conclusions because even the
  unchanged baseline can hit its simulated idle timeout; do not reuse it as a
  heap regression result.
- The persistent harness gives real encrypted in-process steady-state evidence,
  but excludes connection setup and priming from `cpu_ns_op`; it cannot answer
  allocation/reserve/delete cost.

## Disposition of added controls

`scheduler_controls.c` uses real contexts and connections with the same source
against both layouts. Churn includes allocation/growth, both reschedules and
deletion/teardown in one lifecycle measurement. The four individual polling
states are timed separately as well as together, so state-specific regressions
cannot be hidden by the aggregate. `test_batch.py` measures existing
self-validating protocol tests including setup. Release smoke and combined
ASan/UBSan checks passed; exact commands are in the cross-usage logs. Final
12-pair outcomes are reported in `RESULTS.md`.

This covers the shared scheduler APIs and representative protocol consumers.
It does not claim that every application, operating system or traffic mixture
has been benchmarked, nor that creation/deletion components are individually
cost-free.
