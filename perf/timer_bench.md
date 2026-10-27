# Timer wake-list benchmark

`timer_bench.c` exercises picoquic's actual connection wake-list functions:
`picoquic_reinsert_by_wake_time`, `picoquic_get_earliest_cnx_to_wake`, and
`picoquic_get_next_wake_time`. Its fixtures are zeroed `picoquic_quic_t` and
`picoquic_cnx_t` objects with the production wake-list fields initialized by the
API itself. The fixture keeps a pointer array and allocates every `picoquic_cnx_t`
separately with `calloc(sizeof(picoquic_cnx_t))`, matching production's
per-connection allocation bins rather than packing connections contiguously. It
uses simulated time and releases every ready-FIFO and future-timer entry before
freeing the individual objects.

`timer_bench_contiguous.c` preserves the v2 fixture used by `cached-min-v2`
and `heap` results, which used a contiguous connection array. The earlier v1
run is exploratory; its original binary is retained in the local artifacts. Use
`timer_bench.c` for all confirmation measurements.

Build after configuring the normal Release build. The helper follows the static
library/dependency order from CMake. The final heap requires `--heap` to select
its fixture fields:

```sh
python3 perf/build_timer.py --heap --output build-perf/timer_bench
```

For upstream or cached-minimum snapshots, omit `--heap` and match both headers
and libraries to the variant:

```sh
python3 perf/build_timer.py --include ../perf-artifacts/upstream/include \
  --libdir ../perf-artifacts/upstream --output ../perf-artifacts/upstream/timer_bench-separate
```

Run the non-timed correctness suite first:

```sh
build-perf/timer_bench --correctness
```

It checks future-deadline ordering, deadline cutoffs, FIFO order for equal
deadlines, min deletion, promotion from the future index, ready/future moves,
rescheduling a timer to `UINT64_MAX`, and cleanup. It also runs a
2,048-step deterministic randomized oracle with duplicate deadlines and random
max-wake cutoffs. A UINT64_MAX deadline remains indexed; this is not cancellation. This harness
does not test real connection deletion; the registered `wake_queue` test does. A `correctness=ok` result is the only expected success
output.

Timed invocation is:

```text
timer_bench <query|query-static|query-random-static|update|ready-only|ready-mixed|ready-mixed-random|monotonic|randomized> <size> <iterations> <seed>
```

`query` performs wake lookup plus next-time lookup on a spread of future timers,
with one randomized timer update per 32 queries. `query-static` has the same
monotonic spread without updates; `query-random-static` uses a randomized initial
spread without updates. `update` performs one randomized reinsert and one query
per iteration. `ready-only` moves every connection to the ready FIFO before
timing, so no future-index lookup remains, then queries and reinserts the FIFO
head. `monotonic` moves a rotating entry to strictly increasing future deadlines,
which exposes minimum-index behavior under monotonic insertion. `randomized` is the
representative control: it performs a random future-time reinsert followed by a
query. `ready-mixed` keeps eight connections ready and the rest at ordered future
deadlines; `ready-mixed-random` randomizes those future deadlines. Setup happens
before timing.

Use both 1,000 and 10,000 future connections, with the 12 paired AB/BA samples and discarded warm-ups defined in
`README.md`; use the recorded configs for exact work counts.
For example:

```sh
build-perf/timer_bench query 10000 5000000 0xdecafbad
build-perf/timer_bench query-static 10000 5000000 0xdecafbad
build-perf/timer_bench query-random-static 10000 5000000 0xdecafbad
build-perf/timer_bench update 10000 5000000 0xdecafbad
build-perf/timer_bench ready-only 10000 5000000 0xdecafbad
build-perf/timer_bench monotonic 10000 5000000 0xdecafbad
build-perf/timer_bench randomized 10000 5000000 0xdecafbad
```

The result includes wall and process CPU elapsed time and their nanoseconds per
operation, `sizeof_cnx`, the requested bytes for each connection allocation,
the total requested bytes across connection objects, and the pointer-array
request size. It also includes a stable-ID checksum so an optimizer cannot
discard the calls without deriving it from object addresses. Compare medians of
paired baseline/candidate results; verify checksums and run the correctness mode
for every candidate before considering a timing difference.
