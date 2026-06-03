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

Summarize the validated stable browser support ledgers without launching a
browser:

```sh
node tests/webtransport/e2e/runners/run-browser.mjs support --json
```

Summarize the same validated ledgers by coverage tag:

```sh
node tests/webtransport/e2e/runners/run-browser.mjs coverage --json
```

Every scenario must declare a stable lowercase ID, title, `browser-baton`
runner configuration, non-empty coverage tags, and an explicit `expect.ok`
value. The runner rejects underspecified scenarios before launching a browser.
Set `certificateHashMode` to `wrong` for negative certificate-hash scenarios;
the default uses the generated server certificate hash. Wrong-hash scenarios
also disable Chrome's certificate-error bypass so the browser actually enforces
`serverCertificateHashes`.
Set `certificateHashAlgorithm` when a scenario needs to pass a non-default
algorithm into `serverCertificateHashes`; unsupported algorithms also disable
Chrome's certificate-error bypass so the scenario proves that the browser does
not silently establish a WebTransport session.
Positive baton scenarios may also require browser-side constructor subtests with
`protocolConstructorOk` and `urlConstructorOk`. The runners also record an
`optionsConstructor` diagnostic for `allowPooling: true` combined with
`serverCertificateHashes`; set `optionsConstructorOk` in a scenario expectation
and `PICOQUIC_WT_OPTIONS_CONSTRUCTOR_REQUIRED=1` only for browser/version lanes
where that API requirement is known to be implemented. Chrome `148.0.7778.181`
on macOS was observed to construct instead of throwing for that case, so the
portable core manifest records the diagnostic but does not gate picoquic server
interop on it yet.
Set `requireDatagram: false` on positive scenarios that should prove
`requireUnreliable` is only a constructor requirement knob; when picoquic
advertises datagram support, those scenarios may still assert exact datagram
send/receive payloads.
Use `datagramWritableOk` on positive scenarios that must prove
`datagrams.writable` rejects a non-`BufferSource` chunk with `TypeError` while
the main scenario still successfully sends and receives the exact expected
baton datagram payload, such as `datagramsReceived: [252]`.
Use `datagramReceiveMode: "length"` when a scenario needs to assert the exact
byte length of a non-baton server datagram through `datagramLengths`. Use
`datagramSendMode: "length"` with `datagramSendSize` when the browser should
send a deterministic fixed-size datagram instead of a baton payload.
Use `datagramSendCount` with fixed-size sends when the browser should write
multiple same-sized datagrams in one scenario.
For repeated same-size server datagrams, use `datagramLengthsMin` together
with `datagramLength`; the browser page waits for the minimum count before
finishing, and the runner verifies every observed length.
Use `datagramsReceivedMin` when a positive scenario exercises concurrent lanes
and only needs to prove browser datagram availability; HTTP/3 datagrams are
unreliable, so duplicated or dropped baton datagram echoes are not a stream
ordering failure. Set `sequenceOrderMatters: false` for concurrent baton stream
scenarios where each expected byte must arrive exactly once, but cross-stream
arrival order is intentionally not constrained.
Use `datagramWritableTestsInclude` and `streamWritableTestsInclude` to require
the exact bad-chunk diagnostic subtests that ran.
Use `postCloseOk` and `postCloseTestsInclude` on positive scenarios to require
browser-observable session termination behavior, such as rejecting new
bidirectional and unidirectional stream creation after `transport.closed`
settles.
Use `closeSessionOk` and `closeSessionTestsInclude` to require the dedicated
browser close diagnostic. That diagnostic opens a fresh WebTransport session,
calls `transport.close({ closeCode: 42, reason: "browser-close-test" })`, and
requires `transport.closed` to resolve. Pair it with
`server.browserCloseReceived` when a browser lane must prove that pico_baton
observed the corresponding `WT_CLOSE_SESSION` capsule and close reason.
Use `protocolConstructorTestsInclude`, `urlConstructorTestsInclude`, and
`optionsConstructorTestsInclude` the same way for constructor diagnostics.
Use `eventsInclude` for required browser-observable state-machine events such
as `ready`, stream receive/send labels, datagram labels, and `closed`.
Use `eventsExclude` for negative scenarios that must prove a failed handshake
never reached `ready`, clean `closed`, or app-data send states. Failed
handshake scenarios should also assert zero `readyMs`/`closedMs`, an empty
selected protocol, empty payload arrays, and `datagramsSent: 0`.
Origin-policy scenarios assert the browser-visible `ready` outcome and the
server-side `originMissing`/`originRejected` counters parsed from `pico_baton`
logs, so allow and deny routes are not inferred from client results alone.
Use `streamWritableOk` the same way for outgoing unidirectional and
bidirectional stream writers.
Positive scenarios also assert server-side close-session evidence from the
diagnostic `transport.close({ closeCode: 0, reason: "writable-bad-chunk-test" })`
path through `server.closeSessionReceivedMin` and
`server.writableBadChunkCloseReceived`. Browser expected-result files may
override these server expectations only with browser/version evidence.
The separate browser-close diagnostic asserts `server.browserCloseReceived`
when the browser/version lane is expected to deliver `browser-close-test`.
Use `server.batonDatagramsReceivedMin` and `server.zeroBatonReceivedMin` to
prove pico_baton observed browser-to-server datagram and terminal stream-baton
traffic, respectively; browser-side `sent`/`datagramsSent` values alone only
prove the JavaScript API accepted those writes.
Use `server.sizedDatagramsReceivedMin` and `server.datagramBytesReceivedMin`
when the server must prove that it observed browser-to-server datagrams with a
specific payload length.

Run the portable core scenario in Chrome:

```sh
cmake --build build -j$(sysctl -n hw.ncpu) --target pico_baton
CHROME_BIN="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
PICOQUIC_WT_CHROME_HEADLESS=old \
npx -y node@22 tests/webtransport/e2e/runners/run-browser.mjs --browser chrome
```

On Apple Silicon with an x64/Rosetta Node process, add
`PICOQUIC_WT_CHROME_ARCH=arm64` so the runner starts native Chrome.
Run the same Chromium-family adapter against Edge by selecting the `edge`
browser lane and pointing `CHROME_BIN` at Microsoft Edge:

```sh
CHROME_BIN="/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge" \
PICOQUIC_WT_CHROME_HEADLESS=old \
npx -y node@22 tests/webtransport/e2e/runners/run-browser.mjs --browser edge
```

Run Firefox stable through geckodriver:

```sh
GECKO_DRIVER_BIN=/path/to/geckodriver \
FIREFOX_BIN=/path/to/firefox \
npx -y node@22 tests/webtransport/e2e/runners/run-browser.mjs --browser firefox
```

Expected-result files live under `tests/webtransport/e2e/expected/`. They are
loaded automatically by browser name when present. `status: "skip"` entries
skip a whole scenario; `status: "pass"` entries merge browser-specific
assertion overrides into a normally running scenario. Every expected-result
entry must include browser/version, platform, category, reason, and evidence.
Within a `pass` override, an inherited assertion value of `null` omits only
that exact assertion for the documented browser/version.
Expected-result files are validated against the manifest with `list --expected`:
stale scenario IDs, duplicate entries, unsupported statuses, and malformed
pass/skip entries are test failures.
The `support` command validates the core scenario manifest plus every stable
browser expected-result file and reports each scenario as `claimed-pass`,
`expected-pass`, or `skip` for Chrome, Edge, Firefox, and Safari.
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
