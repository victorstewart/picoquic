# Compact token-index experiment

`token_compact.patch` is a complete patch against pristine upstream
`1d81a730ebbc0f4598f8127a7f43260a71a9ac7d`. It replaces only the token-reuse
splay index with a lazy two-level directory of 32-entry pages. Leaves pack full
64-bit expiry and hash keys beside stable token-object pointers. Token objects
lose their unused 24-byte intrusive links; pages and directory add overhead.

Insertion acquires a new leaf and directory capacity before splitting any live
page. Allocation failure rejects that admission and preserves existing tokens.
The existing admission cap, duplicate counter, length rule, and strict expiry
cutoff remain. Candidate allocation is still performed before duplicate lookup,
so this trial does not mix in a separate allocation-elimination optimization.
Clearing frees expired token objects and pages; context destruction also frees
entries whose expiry is UINT64_MAX. The directory is retained until context free.

The standalone `token_bench.c` calls real token APIs. Admission modes include
allocation, duplicate checks, and expiry; `lookup` and `hot` preload a population
outside timing. The independent correctness mode checks cap rejection, repeated
keys, equal-hash/different-expiry keys, strict clear boundaries, UINT64_MAX,
2,048 shuffled entries, and clear/reuse. Existing Picoquic token tests are also
run. This experiment is not automatically selected for production.

Build with the helper (matching source/header/library snapshot is required):

```sh
python3 perf/build_timer.py --source perf/token_bench.c --output build-perf/token_bench
build-perf/token_bench --correctness
build-perf/token_bench admit-shuffled 10000 100
build-perf/token_bench lookup 10000 1000000
build-perf/token_bench hot 10000 1000000
```

`work` means batch count for admission modes, lookup count otherwise. CPU and
wall nanoseconds per operation are reported; batch clearing is amortized into
the admission/check count. All comparisons require equal checksums.
