# Persistent-connection follow-up experiments

Authorized scope: confirm the retained heap under many persistent connections,
then evaluate exactly three mechanisms: a send-queue tail pointer, in-place
future-heap deadline updates, and metadata-only packet initialization.
Baseline for new mechanisms: `bc3793cb` (the retained indexed heap). The persistent
workload also compares the pinned upstream `1d81a730` against that baseline.
No crypto/provider, ACK container, hash table, or additional mechanism is added.

Run natively on the same M2 Max Mac and compiler/dependency configuration as the
first sweep. Keep production candidates isolated; use serial 12-pair alternating
A/B timing with a discarded warm-up per variant. Do not compile, profile, or run
sanitizers during timing. Record full commands, raw output, source identities,
paired median reductions and bootstrap intervals with `../run_ab.py`.

Workloads: real encrypted in-process protocol with 1,000 and 10,000 persistent
connections, one/four streams, periodic and bursty small messages, plus relevant
existing bulk/loss controls. Connection establishment is outside the timed message
region. A simulated network clock allows idle time without wall-clock sleeping;
any simulated latency is reported explicitly and is not real network p99 latency.
Per-message CPU, delivered content/count, latency, and memory are evaluated.
Additional mechanism workloads are queued writes at depth 1/8/64/256, actual
scheduler operations, and packet acquire/recycle with protocol correctness.

Keep a candidate only with >=5% median CPU reduction in a relevant named workload,
a 95% paired bootstrap interval above zero, matching delivered content/operation
checksums, no reproducible >5% material regression in relevant performance controls,
and acceptable lifecycle/memory cost. Protocol evidence may establish correctness
and lack of regression without establishing a throughput gain. Do not claim
unmeasured speedups. Existing packet/wire comparability tolerance remains 0.001%
for nondeterministic handshake variation; application content is exact.

For metadata-only packet initialization, payload poisoning and sent-byte checks
are required in addition to sanitizers; ASan/UBSan do not detect uninitialized
payload reads. Queue tests cover drain/refill, partial writes, reset and FIN;
heap tests preserve tie order, wrap and transitions between ready/future states.

Keep each accepted mechanism and its proof in a separate commit. Rejected
patches and results remain experimental evidence. Finish with one scoped diff
review and a report of observed results; no external push is part of this phase.

## Cross-usage regression gate

The user's subsequent request explicitly adds regression checks across other
callers of the changed structures. This is a finite validation extension, not
another optimization sweep. Source audits map queue, packet and timer ownership
in `regression-audit-{queue,packet,timers}.md`. Reuse valid existing controls and
setup/RSS samples; add the single-stream and multistream active-send controls,
short/large DATAGRAM, multipath, direct receive, reset/FIN, MTU, context lifecycle,
and empty/future/ready/capped/multiple-context scheduler polling and churn.

Use the same 12-pair protocol. Inspect each control independently; a gain in one
path cannot cancel a regression in another. Investigate a repeatable slowdown
even when below the original 5% material-regression threshold. These checks can
establish results for their named workloads; they cannot prove zero slowdown on
every possible machine, traffic pattern or application.

## Measurement audit addendum

The initial persistent loss fixture intermittently reaches the configured
600-second simulated idle timeout on the unchanged baseline. Its failed and
partial runs are preserved and are not performance evidence. Existing
`perf_loopback_loss` comparisons provide the loss regression control. Extra close
reason diagnostics change only failure reporting; no timeout or scheduling
workaround was applied to force the fixture to pass.

The short 1,000-connection unchanged-binary A/A comparison reported an apparent
6.3% CPU gain. Therefore the initial short persistent result is exploratory,
even though its original bootstrap interval was positive. Longer 1,000-connection
timings and an independent 10,000-connection A/A check are required before making
a protocol speedup claim. Persistent packet counts also vary with normal crypto
randomness. Preserve the exact application checks, report the A/A packet spread,
and analyze CPU per packet as a sensitivity check. The original 0.001% wire and
packet tolerance continues to apply to the existing bulk benchmark; it is not
silently relaxed in those configurations.

The first two-pattern payload-poison check accidentally reused the A5 object for
the 5A build. Its JSON is explicitly marked invalid. The corrected build forces
recompilation, requires four distinct binary hashes, and repeats all wire
comparisons. Neither deterministic crypto nor payload poisoning is enabled in
the performance binaries or retained production code.

## Snapshot reproduction

The sibling `../perf-artifacts/` directories hold immutable binaries, archives,
and matching `include/` headers. Their source mapping is:

| Snapshot | Production source |
| --- | --- |
| `upstream` | `1d81a730` |
| `followup-base` | `bc3793cb` |
| `send-queue-tail` | base plus `send_queue_tail.patch` |
| `heap-update` | base plus `heap_update.patch` |
| `packet-init` | base plus `packet_init.patch` |
| `followup-combined` | base plus queue-tail and packet-init patches |

Configure an isolated checkout at the required revision with the same native
Release configuration, apply only the indicated patch, and copy its core/log
archives and headers together. `build.py` links a standalone harness against
one explicit snapshot. The timer harness also uses `../build_timer.py`.
The existing project configuration uses `PICOQUIC_FETCH_PTLS=ON`,
`OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`, `PTLS_WITH_FUSION=OFF`,
and `BUILD_PICO_SIM=OFF`. Sanitizer builds enable both `ENABLE_ASAN` and
`ENABLE_UBSAN` and are excluded from timing.

The first persistent binaries use the source preceding the close-diagnostic
addition. Reverse `persistent_diagnostics.patch` against `persistent_bench.c`
to reproduce that source; its hash is recorded in `results/identities.json`.
The `_diagnostic` binaries use the checked-in source. Only failure reporting
changed. Never enable `PERSISTENT_DETERMINISTIC` for a performance binary.

Run a saved `*-config.json` with `../run_ab.py CONFIG NEW_OUTPUT.jsonl --pairs 12`.
The runner refuses to overwrite prior samples. Configurations contain the exact
paths on this Mac; adjust them if reproducing in a different checkout.
After all timing ends, run `summarize_completed.py` and `analyze_controls.py` to
derive summaries. Read `completion-status.json`, equivalence mismatches, and the
failure logs before interpreting any percentage.
