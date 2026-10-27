# Indexed future-timer heap experiment

## Scope

`timer_heap.patch` applies to pristine Picoquic commit
`1d81a730ebbc0f4598f8127a7f43260a71a9ac7d`. It changes only
`picoquic/quicctx.c` and `picoquic/picoquic_internal.h`.

The existing timer benchmark fixture directly initializes the replaced splay
members. It needs a heap-specific fixture branch when this patch is selected;
that harness adjustment is intentionally outside this experimental patch.

The patch replaces the future-timer splay tree with an indexed binary min-heap.
The ready-now wake list remains the existing FIFO: it is still used for timers
at or before the current QUIC time and for promotions from the heap.

## Semantics

- Heap comparison is `(next_wake_time, insertion_sequence)`, so connections
  with the same future deadline promote in FIFO order.
- Each future connection stores its heap index. Removal and deadline updates
  locate it directly, replace the removed slot with the last element, then
  sift in the needed direction.
- Root access provides the next future wake time. Promotion removes roots while
  their deadline is due, appending each connection to the unchanged ready FIFO.
- Context creation reserves the requested initial heap capacity. Before every
  call to `picoquic_create_cnx_internal()` allocates or registers a connection,
  the patch grows the heap to cover all live connections plus the new one.
  That function is the single internal connection-creation path (used by both
  the public API and inbound packet handling), and it can return `NULL` when
  `realloc` fails. Reinsertion therefore does not allocate: its capacity
  assertion follows from reserving for every live connection before it becomes
  schedulable, so an OOM event cannot drop or delay an existing wakeup.
- Context teardown deletes all connections first, then releases the heap.
  No splay node or parallel timer index remains in the normal scheduling path.

## Cost and portability

The context initially allocates `max_number_connections * sizeof(picoquic_cnx_t *)`
bytes and doubles that pointer array only while creating a connection. Each
connection replaces the embedded splay node with a
`size_t` heap index and a `uint64_t` sequence number. Heap operations are
O(log n); next-min access is O(1). The one exceptional sequence-wrap
renumbering is O(n^2), after 2^64 future-heap insertions, and does not allocate.

The patch uses ISO C integer and size types already used by Picoquic. Its
pointer-array allocation is guarded against `size_t` multiplication overflow.
It should be evaluated on every supported target because pointer size,
structure layout, cache behavior, and allocator cost affect whether it beats
the current splay tree.

## Applying and screening

From a clean checkout at the base commit:

```sh
git apply --check perf/timer_heap.patch
git apply perf/timer_heap.patch
```

The integrated patch passed native Release and ASan/UBSan checks and the
paired measurements recorded in `RESULTS.md`. The final branch uses this heap;
the standalone patch permits rebuilding the exact candidate from upstream.
