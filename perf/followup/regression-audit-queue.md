# Send-queue tail regression audit

This is a bounded source and evidence audit for the retained queue-tail patch.
It does not add a mechanism or request repeated measurements already present in
the saved experiment data.

| Usage | Relevant implementation | Tail interaction | Existing evidence | Performance control status |
| --- | --- | --- | --- | --- |
| Application queued writes | `picoquic_add_to_stream_with_ctx` in `streams.c`; STREAM drain in `frames.c` | Append changes from a list walk to tail link; final drain clears the tail. | Queue A/B covers 1,000 connections at depths 1/8/64/256 and checks payload/node order; persistent four-stream and bulk controls cover real formatting. | Covered. Depth 1 is the small-queue no-regression control. |
| Partial drain, refill, FIN, reset | STREAM formatter and `picoquic_enforce_reset_stream_frame` in `frames.c`; `picoquic_clear_stream` in `streams.c` | A partially consumed head retains the tail; empty drain, clear, and reset must null it. | Queue oracle exercises partial-drain append, empty-to-refill, FIN, reset, and candidate tail invariants. | Added the repeated `terminal-queues` protocol-test control; it includes setup and teardown. |
| Active application sender | `stream->is_active` callback path in `picoquic_format_stream_frame` (`frames.c`) | Active callbacks bypass `send_queue`; only the added stream-field footprint can affect them. | `perf_loopback`, `_multiconn`, `_loss`, and `_multistream` drive the quicperf callbacks: setup calls `picoquic_mark_active_stream` (`picohttp/quicperf.c:773`, `:954`, `:1174`, `:1459`), which supplies output through `picoquic_provide_stream_data_buffer` (`:1271`, `:1339`, `:1391`). The saved loopback comparisons therefore exercise active send. | Covered; no redundant active-sender benchmark. |
| Direct receive | `direct_receive_fn` path in `picoquic_stream_network_input` (`frames.c`) | Receive-only path never appends or drains `send_queue`; only connection/stream footprint is shared. | Stream/connection correctness suite exercises receive logic; active loopback already measures the same added-field footprint on the send side. | Added a repeated `direct_receive` test control for the user's cross-usage request. |
| TLS CRYPTO queue and handshake state | Private TLS append in `tls_api.c`; CRYPTO drain/readiness in `frames.c` and `sender.c`; client-start guards in `quicctx.c`/`qmux.c` | TLS queues use the same field and must null the tail on final CRYPTO drain. | Queue oracle performs TLS empty-to-refill formatting. Persistent raw runs include real TLS setup and report `setup_cpu_ns` and RSS; setup is excluded from the primary message metric. | Saved setup analysis shows no resolved regression: queue-only -0.48% [-1.32, 0.30], combined +0.06% [-0.53, 0.38]. No duplicate handshake run needed. |
| Loss and retransmission | Packet retransmission/data-repeat paths in `frames.c`/`sender.c` use stored packet frames, not a send-queue append. | Tail is not consulted after frame data has entered retransmission state. | `stream_retransmit_copy` correctness and existing bulk-loss control; persistent-loss timing is invalid for both variants because of the documented idle timeout. | Bulk-loss control is sufficient for ordinary loss. No separate tail mechanism control needed. |

## Disposition of added controls

Use the existing self-validating `reset_loop_test`, `reset_after_fin_received`
and `ready_to_zfin` as the finite terminal-queue control. This measures whole
test-process CPU including setup and teardown; it does not isolate the extra
tail-nulling store. The depth-one append case and queue oracle remain separate
evidence. Saved TLS setup and RSS analysis avoids repeating the same startup
work. Final measured outcomes and limitations are in `RESULTS.md`, with raw
samples in `results/cross-usage-remaining.jsonl`.
