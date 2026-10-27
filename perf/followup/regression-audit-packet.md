# Packet initialization ownership and regression audit

Scope: read-only audit of the metadata-only initialization change in the
current working tree. The change affects the pooled packet type only:
`picoquic_create_packet()` and `picoquic_recycle_packet()` in `sender.c`.
The payload member is last in `st_picoquic_packet_t`; the intended initialization
boundary is `offsetof(struct st_picoquic_packet_t, bytes)` in
`picoquic_internal.h`.

## Caller and ownership matrix

| Owner/path | Packet bytes may be read | Release/recycle path | Current evidence |
| --- | --- | --- | --- |
| Normal stream, ACK, crypto and control formatting | Formatter writes frames and padding before protection; AEAD/logging use the declared packet length | Sent packets enter retransmit queues; dequeue/recycle is in `sender.c` | persistent 1/4-stream, bulk, multiconnection, exact-wire poison |
| Initial, handshake, 0-RTT and 1-RTT protection | Header is constructed, required PN sample padding is explicitly written, then AEAD reads the declared payload | Normal retransmit lifecycle | exact-wire poison and selected protocol tests |
| Loss/data-repeat retransmission | Existing completed packet payload is parsed/copy-read only up to `packet->length` | `picoquic_dequeue_retransmit_packet` / spurious-detection cleanup recycle in `sender.c` | loss scenario and selected tests |
| ACK-trap placeholder | Allocated at `sender.c` and used as metadata-only queue state | Recycled through normal pending/retransmit cleanup | metadata poison and pool correctness cases |
| Coalesced/batched send | Each completed packet is protected before segment assembly; abandoned zero-length packet is recycled | `sender.c` packet assembly cleanup | bulk and multiconnection scenarios |
| Packet pool pressure | Fresh allocation and pooled reuse share the same metadata reset boundary | pool cap/free in `picoquic_recycle_packet` | six pool cases, including depths 1/64/1024 and payloads 64/1200 |

`frames.c` also recycles packet objects associated with data-repeat cleanup.
No other production allocator for `picoquic_packet_t` was found. Stateless
receive packets and simulated-link packets are different types and are outside
this ownership change.

## Existing evidence against the risk

The six direct pool cases establish metadata reset and intended payload-write
work for fresh and recycled packets. Persistent one/four-stream, bulk,
multiconnection, and loss scenarios cover the main lifecycle paths above.
The two-pattern exact-wire poison comparison directly detects payload bytes
that alter emitted datagrams. Sixteen selected Release plus ASan/UBSan tests
provide protocol behavior and memory-safety checks, while the wire comparison
covers the uninitialized-read class that those sanitizers do not generally
detect.

## Added cross-usage controls

These three controls add distinct packet formatting paths. Release smoke tests
passed against upstream and the combined build; the combined ASan/UBSan build
also passed. Timing uses the same 12-pair protocol; final outcomes are in
`RESULTS.md` and raw samples in `results/cross-usage-remaining.jsonl`.

1. QUIC DATAGRAM end-to-end cases with 100-byte and 1,300-byte frames. They
   exercises `picoquic_provide_datagram_buffer` and the datagram formatter,
   which writes directly into a packet payload buffer rather than the normal
   stream queue path. `quicperf_datagram` and `quicperf_datagram_multiflow`
   cover one and four flows respectively.
2. Active multipath send/probe cases: `quicperf_multipath_race`,
   `quicperf_multipath_race_delayed` and `quicperf_multipath_settled`.
   Multipath selects the MP AEAD path and has dedicated probe scheduling.
3. `mtu_discovery` and `mtu_max`. These validate the explicit
   zero-padding path in `picoquic_prepare_mtu_probe` and packet protection at
   the maximum practical send size.

These are whole test-process CPU controls, including setup and teardown, for
cross-codebase regression confidence. They are not isolated formatter timings or
evidence of a further packet-pool optimization.
