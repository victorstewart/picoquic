# Persistent-connection native loopback benchmark

`persistent_bench.c` is a standalone native Picoquic harness for the
many-persistent-connections case. It uses real client and server connections in
one simulated-time Picoquic context. It creates all TLS connections before its
message measurement begins, then retains them for periodic and bursty writes on
one or four client-initiated bidirectional streams.

Every application message has a fixed 32-byte header containing a magic value,
client index, stream index, per-stream sequence, and simulated enqueue time.
The server callback incrementally validates the header and deterministic payload
across arbitrary STREAM-frame chunk boundaries. A malformed, missing, repeated,
or out-of-order message makes the process fail. The packet loop routes only the
packet returned by `picoquic_prepare_next_packet_ex`; it does not scan clients
per packet. Readiness and delivery are callback counters. Every round has a
finite packet-step limit.

## Build

Build against a matching heap build with the existing helper:

```sh
python3 perf/build_timer.py --heap \
  --source perf/followup/persistent_bench.c \
  --output build-perf/persistent_bench
```

The source includes only the public `picoquic.h` API and does not access the
wake-tree or heap fields, so use the same source for upstream and cached-min
comparison archives by changing the helper include/library arguments and
omitting `--heap` for those headers.

By default the process resolves certificates from the current repository:
`certs/cert.pem`, `certs/key.pem`, and `certs/test-ca.crt` as the trust store.
The client SNI is `test.example.com`, matching the repository test certificate.
Override these paths with `PICOQUIC_BENCH_CERT`, `PICOQUIC_BENCH_KEY`, and
`PICOQUIC_BENCH_ROOT`.

## Invocation

```text
persistent_bench <population> <streams:1|4> <rounds> <payload_bytes> <burst> <loss:0|1> <seed>
```

`population` is deliberately bounded to `1000` or `10000`. On every eighth
round, `burst` distinct persistent clients enqueue one message; intervening
rounds enqueue `ceil(burst / 16)` messages. Each message rotates across the
selected connection's one or four long-lived streams. The simulated clock then
moves through a 20 ms idle interval. `loss=1` drops a deterministic one-in-64
packet selection; loss is disabled during crypto setup.

For a finite smoke case:

```sh
build-perf/persistent_bench 1000 1 1 32 1 0 0xdecafbad
```

The process reports separate `setup_cpu_ns`/`setup_wall_ns` and message-only
`cpu_ns_op`/`wall_ns_op`, plus validated delivery counts and bytes, checksum,
packet count, RSS high-water mark, and `simulated_p99_delivery_us`.
`simulated_p99_delivery_us` is based on the in-process simulated clock, which
advances while packet-loop work runs; it is not a native network latency
measurement.

If a peer closes, the harness reports the public Picoquic local, remote, and
application close reasons together with the simulated timestamp. This keeps a
transport close distinguishable from a payload-validation failure.

All client connections must reach `picoquic_callback_ready` before priming.
Before message timing begins, the harness sends and validates one warm message on
every selected stream of every persistent connection, then processes protocol
timers through a bounded 20 ms simulated idle interval. Warm messages advance
per-stream sequence state but do not contribute to the measured delivery count,
latencies, checksum, or message timing. The loss RNG is separate from the
workload-selection RNG, and the reported `seed` is the original command-line
value.

## Finite smoke evidence

The following smoke commands compiled with the build command above and passed
payload/sequence validation. They are setup-and-single-message checks, not
throughput samples:

```text
$ build-perf/persistent_bench 1000 1 1 32 1 0 0xdecafbad
population=1000 streams=1 rounds=1 payload_bytes=32 burst=1 loss=0 seed=3737844653 setup_cpu_ns=913453000 setup_wall_ns=915549000 cpu_ns_op=10000.000 wall_ns_op=9000.000 checksum=1245540515841 delivered_messages=1 delivered_bytes=32 simulated_p99_delivery_us=1 rss_max_bytes=91553792 packets=12013

$ build-perf/persistent_bench 10000 1 1 32 1 0 0xdecafbad
population=10000 streams=1 rounds=1 payload_bytes=32 burst=1 loss=0 seed=3737844653 setup_cpu_ns=9611056000 setup_wall_ns=9828428000 cpu_ns_op=41783000.000 wall_ns_op=48172000.000 checksum=14130442403841 delivered_messages=1 delivered_bytes=32 simulated_p99_delivery_us=20000 rss_max_bytes=823066624 packets=120101

$ ../perf-artifacts/followup-base/persistent_bench 1000 1 2048 64 64 1 0xdecafbad
population=1000 streams=1 rounds=2048 payload_bytes=64 burst=64 loss=1 seed=3737844653 setup_cpu_ns=912544000 setup_wall_ns=915036000 cpu_ns_op=4692.977 wall_ns_op=4699.474 checksum=50979956133761491 delivered_messages=23552 delivered_bytes=1507328 simulated_p99_delivery_us=12038 rss_max_bytes=90390528 packets=67659 measured_packets=55650
```
