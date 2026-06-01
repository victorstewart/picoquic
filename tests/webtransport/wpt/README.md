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

Smoke-test the pico_baton lifecycle adapter:

```sh
cmake --build build -j$(sysctl -n hw.ncpu) --target pico_baton
node tests/webtransport/wpt/run-wpt.mjs server-smoke
```

The adapter intentionally does not import or run the full WPT suite yet. Later
commits should add browser launchers, expected-failure manifests, and strict
versus compatibility-mode WPT gates.
