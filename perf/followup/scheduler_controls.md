# Scheduler regression controls

`scheduler_controls.c` adds a finite cross-codebase regression gate for the
retained future-timer heap. It uses real Picoquic contexts and client
connections; it never constructs scheduler nodes or accesses heap fields.
Therefore the same source can be built with upstream, the retained heap, and
other compatible candidate archives.

Build with the existing snapshot helper, choosing matching headers and archives.
No layout-specific define is needed:

```sh
python3 perf/followup/build.py perf/followup/scheduler_controls.c \
  ../perf-artifacts/followup-base build-perf/scheduler_controls
```

The command line is:

```text
scheduler_controls <poll[-empty|-future|-ready|-capped]|churn> <iterations> <batch:1..64>
```

`poll` creates four independent contexts before timing. They cover: an empty
context with a capped delay, a future wake at 700 us, a ready FIFO head, and a
future wake capped at 100 us. Setup validates those exact
`picoquic_get_next_wake_delay` results. The timed loop only polls them, so
context and connection creation are excluded. `batch` is accepted for a stable
CLI but does not affect this mode.
The four `poll-*` forms measure each state separately so that an improvement in
one state cannot mask a regression in another. A checksum validates every
returned delay, including the context selector, against its expected value.

`churn` creates a new context with initial capacity one on each iteration, then
creates `batch` real connections. Each connection is moved future-to-ready,
future-to-`UINT64_MAX`, or future-to-future before deletion. The harness checks
that the connection count, earliest wake and next wake are empty after every
batch, then frees the context. Its metric includes context creation, reserve
growth, connection creation, rescheduling, deletion and teardown.

Output includes `operations`, process CPU and wall time, nanoseconds per
operation, and a deterministic checksum. This is a regression control, not a
claim of an optimization win. Run identical serial A/B pairs with the same
source, command, build flags, dependency archives and batch size.
