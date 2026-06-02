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

The adapter intentionally does not import or run the full WPT suite yet. Later
commits should add browser launchers and strict versus compatibility-mode WPT
execution gates. Expected-result manifests live under
`tests/webtransport/wpt/expected/`; keep entries tied to tests in the listed WPT
target subset so stale browser caveats fail fast.
