# WebTransport Fuzz Corpus

This directory stores persisted corpus inputs for WebTransport-adjacent HTTP/3
parser fuzz smoke tests.

`h3zero_wt_fuzz_corpus` replays these seeds through the same QPACK and H3 DATA
stream parsers exercised by the generated `wt_fuzz` tests. The CI native
WebTransport sanitizer lane runs the full `wt_fuzz` label so corpus regressions
are checked under ASAN/UBSAN.
