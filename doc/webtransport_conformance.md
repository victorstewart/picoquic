# WebTransport Conformance Tests

This document tracks the staged WebTransport-over-HTTP/3 conformance suite for picoquic.
The protocol target is draft-ietf-webtrans-http3-15.

## Test Labels

CTest labels separate protocol behavior, browser compatibility behavior, and future
browser API coverage:

| Label | Purpose |
| --- | --- |
| `wt_strict` | Native tests for draft-15 WebTransport-over-HTTP/3 behavior. These tests must not depend on legacy browser tokens or settings except when asserting that strict mode rejects them. |
| `wt_compat` | Explicit browser compatibility tests. These cover known browser quirks such as Chrome versions that still use legacy WebTransport negotiation. |
| `wt_wire` | Native wire-protocol and parser tests for SETTINGS, stream prefixes, capsules, datagrams, reset codes, close behavior, and exact error handling. |
| `wt_wpt` | Web Platform Tests for the browser WebTransport API. This label is reserved until the WPT runner lands. |
| `wt_fuzz` | Fuzz and property-style tests for parsers and state-machine inputs. |

Run a label with CTest after configuring and building:

```sh
ctest --test-dir build -L wt_strict --output-on-failure
ctest --test-dir build -L wt_compat --output-on-failure
ctest --test-dir build -L wt_wire --output-on-failure
ctest --test-dir build -L wt_fuzz --output-on-failure
```

The `wt_wpt` label is represented by a disabled placeholder until the WebTransport
WPT adapter is added. Do not treat that placeholder as browser conformance evidence.

## Strict Versus Compatibility Mode

Strict mode is the draft-15 conformance target. Strict WebTransport tests should use
the native HTTP/3 WebTransport token `webtransport-h3`, require the draft-15 SETTINGS
and transport parameters, enforce CONNECT pseudo-header requirements, and assert exact
HTTP/3/WebTransport error behavior where the draft defines it.

Compatibility mode is only for browser behavior that is required for real
interoperability but does not match the strict draft-15 path. Compatibility tests must
be labeled `wt_compat`, must not weaken strict tests, and must describe the browser
or engine behavior being accommodated.

If WebTransport production code needs a browser-specific conditional branch or
workaround, document it with an inline code comment at the workaround site. The comment
must identify the browser/version or engine behavior, explain why the strict draft-15
path is not sufficient, and point to the compatibility test or artifact that proves the
exception is still needed.

## Commit Evidence

Every conformance commit should include the following in the commit message body:

```text
Conformance-Scope:
- Draft/API sections exercised:
- Strict mode or compatibility mode:
- Behavior added or fixed:

Tests:
- <command> => PASS|FAIL|EXPECTED-FAIL, <summary result>
- <command> => PASS|FAIL|EXPECTED-FAIL, <summary result>

Notes:
- Remaining known gaps:
- Browser/version caveats, if any:
```

Do not commit default-on failing tests. If a test is intentionally ahead of the
implementation, keep it behind an explicit non-default target or expected-failure
manifest.
