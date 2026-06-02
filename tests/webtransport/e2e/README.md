# WebTransport Browser E2E

This directory contains manifest-driven browser E2E scenarios for picoquic's
WebTransport-over-HTTP/3 server surface. The runner currently wraps the existing
`pico_baton` browser harness; later scenarios should add server-side traces,
more endpoint classes, expected-failure manifests, WPT integration, and
additional browser adapters.

List the core scenarios:

```sh
node tests/webtransport/e2e/runners/run-browser.mjs list
```

Run the portable core scenario in Chrome:

```sh
cmake --build build -j$(sysctl -n hw.ncpu) --target pico_baton
CHROME_BIN="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
PICOQUIC_WT_CHROME_HEADLESS=old \
npx -y node@22 tests/webtransport/e2e/runners/run-browser.mjs --browser chrome
```

Expected-result files live under `tests/webtransport/e2e/expected/`. They are
loaded automatically by browser name when present, and every skipped scenario
must include browser/version, platform, category, reason, and evidence.
Expected-result files are validated against the manifest: stale scenario IDs
and duplicate entries are test failures.

Safari execution requires Safari WebDriver remote automation:

```sh
sudo safaridriver --enable
node tests/webtransport/e2e/runners/run-browser.mjs --browser safari
```

Do not use these initial E2E scenarios as a full browser-support claim. A
README support row needs the full browser conformance gate, the relevant WPT
subset, native protocol evidence, and links to the exact artifacts.
