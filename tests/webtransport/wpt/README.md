# WebTransport WPT Adapter

This directory contains the skeleton used to connect picoquic WebTransport
tests to the Web Platform Tests `webtransport/` suite.

List the initial target subset without a WPT checkout:

```sh
node tests/webtransport/wpt/run-wpt.mjs list
```

List matching tests from a local WPT checkout:

```sh
node tests/webtransport/wpt/run-wpt.mjs list --wpt-root /path/to/wpt
```

Validate an expected-result manifest against the current target subset:

```sh
node tests/webtransport/wpt/run-wpt.mjs list \
  --expected tests/webtransport/wpt/expected/chrome-stable.json
```

Smoke-test the pico_baton lifecycle adapter:

```sh
cmake --build build -j$(sysctl -n hw.ncpu) --target pico_baton
node tests/webtransport/wpt/run-wpt.mjs server-smoke
```

Dry-run the selected upstream WPT invocation without launching a browser:

```sh
node tests/webtransport/wpt/run-wpt.mjs run \
  --wpt-root /path/to/wpt \
  --browser chrome \
  --test constructor.https.sub.any.js \
  --dry-run
```

Run the selected upstream WPT subset with WPT's own WebTransport-over-H3
server enabled:

```sh
node tests/webtransport/wpt/run-wpt.mjs run \
  --wpt-root /path/to/wpt \
  --browser chrome \
  --expected tests/webtransport/wpt/expected/chrome-stable.json
```

The `run` command is opt-in and uses WPT's upstream WebTransport server via
`wpt run --enable-webtransport-h3`. It is a browser API/WPT gate, not yet a
picoquic-server WPT conformance claim. Later commits should add a
WPT-compatible picoquic server/handler shim so the same target subset can be
run against picoquic directly.

Expected-result manifests live under
`tests/webtransport/wpt/expected/`; keep entries tied to tests in the listed WPT
target subset so stale browser caveats fail fast.
