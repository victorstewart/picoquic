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

Every scenario must declare a stable lowercase ID, title, `browser-baton`
runner configuration, non-empty coverage tags, and an explicit `expect.ok`
value. The runner rejects underspecified scenarios before launching a browser.
Set `certificateHashMode` to `wrong` for negative certificate-hash scenarios;
the default uses the generated server certificate hash. Wrong-hash scenarios
also disable Chrome's certificate-error bypass so the browser actually enforces
`serverCertificateHashes`.
Positive baton scenarios may also require browser-side constructor subtests with
`protocolConstructorOk` and `urlConstructorOk`. The runners also record an
`optionsConstructor` diagnostic for `allowPooling: true` combined with
`serverCertificateHashes`; set `optionsConstructorOk` in a scenario expectation
and `PICOQUIC_WT_OPTIONS_CONSTRUCTOR_REQUIRED=1` only for browser/version lanes
where that API requirement is known to be implemented. Chrome `148.0.7778.181`
on macOS was observed to construct instead of throwing for that case, so the
portable core manifest records the diagnostic but does not gate picoquic server
interop on it yet.
Use `datagramWritableOk` on positive scenarios that must prove
`datagrams.writable` rejects a non-`BufferSource` chunk with `TypeError` while
the main scenario still successfully sends the expected baton datagram.
Use `streamWritableOk` the same way for outgoing unidirectional and
bidirectional stream writers.

Run the portable core scenario in Chrome:

```sh
cmake --build build -j$(sysctl -n hw.ncpu) --target pico_baton
CHROME_BIN="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
PICOQUIC_WT_CHROME_HEADLESS=old \
npx -y node@22 tests/webtransport/e2e/runners/run-browser.mjs --browser chrome
```

Expected-result files live under `tests/webtransport/e2e/expected/`. They are
loaded automatically by browser name when present. `status: "skip"` entries
skip a whole scenario; `status: "pass"` entries merge browser-specific
assertion overrides into a normally running scenario. Every expected-result
entry must include browser/version, platform, category, reason, and evidence.
Expected-result files are validated against the manifest with `list --expected`:
stale scenario IDs, duplicate entries, unsupported statuses, and malformed
pass/skip entries are test failures.
Run output includes browser metadata so support tables can cite exact browser
versions from CI artifacts.

Safari execution requires Safari WebDriver remote automation:

```sh
sudo safaridriver --enable
node tests/webtransport/e2e/runners/run-browser.mjs --browser safari
```

Do not use these initial E2E scenarios as a full browser-support claim. A
README support row needs the full browser conformance gate, the relevant WPT
subset, native protocol evidence, and links to the exact artifacts.
