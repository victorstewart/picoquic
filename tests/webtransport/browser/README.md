# picoquic WebTransport Browser Test

This directory contains a static browser harness for the `pico_baton`
WebTransport sample. It exercises the browser WebTransport constructor, session
ready/closed promises, incoming unidirectional streams, incoming bidirectional
streams, outgoing unidirectional streams, outgoing bidirectional streams, and
datagrams. By default it also offers the `devious-baton-00` protocol so the
server's `WT-Protocol` selection path is covered.

## Serve

```sh
cmake --build build -j$(nproc) --target pico_baton
./build/pico_baton -p 4433 -c certs/cert.pem -k certs/key.pem -w tests/webtransport/browser /baton
```

Then open:

```text
https://localhost:4433/index.html?autorun=1
```

The default WebTransport endpoint is:

```text
https://localhost:4433/baton?version=0&baton=251&count=1
```

Starting at baton `251` keeps the test short while forcing the server datagram
path: the browser receives `251`, sends `252`, expects a datagram carrying
`252`, receives `253`, sends `254`, receives `255`, sends `0`, and expects the
server to close the session cleanly.

## Certificates

Browsers require WebTransport over a secure context. For local testing, trust
the certificate used by `pico_baton`, launch the browser with a local testing
certificate override, or serve this page from another trusted origin and pass a
certificate hash:

```text
https://trusted.example/index.html?url=https%3A%2F%2Flocalhost%3A4433%2Fbaton%3Fversion%3D0%26baton%3D251%26count%3D1&certHash=<base64url-sha256-cert-hash>&autorun=1
```

The harness passes `allowPooling: false`, `requireUnreliable: true`, and, when
provided, `serverCertificateHashes: [{ algorithm: "sha-256", value }]`.
Certificate hash authentication requires a browser-acceptable X.509v3 leaf
certificate, normally P-256 ECDSA, with a validity period under two weeks. A
local cert/hash pair can be made with:

```sh
mkdir -p /tmp/picoquic-wt-cert
openssl ecparam -name prime256v1 -genkey -noout -out /tmp/picoquic-wt-cert/key.pem
openssl req -new -x509 -key /tmp/picoquic-wt-cert/key.pem -out /tmp/picoquic-wt-cert/cert.pem -days 13 -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' -addext 'keyUsage=digitalSignature' -addext 'extendedKeyUsage=serverAuth'
openssl x509 -in /tmp/picoquic-wt-cert/cert.pem -outform der | openssl dgst -sha256 -binary | openssl base64 -A | tr '+/' '-_' | tr -d '='
```

## Chrome

Use a Chrome or Chromium build with WebTransport enabled and a certificate path
that the browser accepts. The dependency-free runner starts `pico_baton`,
launches Chrome/Chromium through the DevTools protocol, and validates the
result:

```sh
node tests/webtransport/browser/run-chrome.mjs
```

Set `CHROME_BIN=/path/to/chrome` if the browser is not on `PATH`. One manual
command shape is:

```sh
google-chrome --user-data-dir=/tmp/picoquic-wt-chrome --ignore-certificate-errors --origin-to-force-quic-on=localhost:4433 'https://localhost:4433/index.html?autorun=1'
```

Pass criteria: the page state is `pass`, selected protocol is
`devious-baton-00`, received batons are `[251,253,255]`, sent batons are
`[252,254,0]`, and at least one received datagram is recorded.

## Safari

Run the same URL in Safari after trusting the `pico_baton` certificate:

```text
https://localhost:4433/index.html?autorun=1
```

Pass criteria are the same as Chrome.
