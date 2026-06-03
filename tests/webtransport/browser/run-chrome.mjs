#!/usr/bin/env node

import { spawn } from "node:child_process";
import { createHash, X509Certificate } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const ROOT = resolve(new URL("../../..", import.meta.url).pathname);
const BATON = process.env.PICO_BATON_BIN || join(ROOT, "build", "pico_baton");
const WEB_ROOT = process.env.PICOQUIC_WT_WEB_ROOT || join(ROOT, "tests", "webtransport", "browser");
const PORT = Number(process.env.PICOQUIC_WT_PORT || 4433);
const PROTOCOL = process.env.PICOQUIC_WT_PROTOCOL || "devious-baton-00";
const REQUIRE_DATAGRAM = process.env.PICOQUIC_WT_REQUIRE_DATAGRAM !== "0";
const USE_BYOB = process.env.PICOQUIC_WT_USE_BYOB !== "0";
const DATAGRAM_RECEIVE_MODE = process.env.PICOQUIC_WT_DATAGRAM_RECEIVE_MODE || "baton";
const DATAGRAM_RECEIVE_MIN = parseOptionalIntegerEnv("PICOQUIC_WT_DATAGRAM_RECEIVE_MIN");
const DATAGRAM_SEND_MODE = process.env.PICOQUIC_WT_DATAGRAM_SEND_MODE || "baton";
const DATAGRAM_SEND_SIZE = parseOptionalIntegerEnv("PICOQUIC_WT_DATAGRAM_SEND_SIZE");
const DATAGRAM_SEND_COUNT = parseOptionalIntegerEnv("PICOQUIC_WT_DATAGRAM_SEND_COUNT");
const STREAM_MODE = process.env.PICOQUIC_WT_STREAM_MODE || "baton";
const STREAM_SIZE = parseOptionalIntegerEnv("PICOQUIC_WT_STREAM_SIZE") || 0;
const STREAM_COUNT = parseOptionalIntegerEnv("PICOQUIC_WT_STREAM_COUNT") || 1;
const EXPECT_OK = process.env.PICOQUIC_WT_EXPECT_OK !== "0";
const RUN_PROTOCOL_CONSTRUCTOR = process.env.PICOQUIC_WT_PROTOCOL_CONSTRUCTOR !== "0";
const CERT_HASH_ALG = process.env.PICOQUIC_WT_CERT_HASH_ALG || "sha-256";
const EXPECT_RECEIVED = parseIntegerArrayEnv("PICOQUIC_WT_EXPECT_RECEIVED", [251, 253, 255]);
const EXPECT_SENT = parseIntegerArrayEnv("PICOQUIC_WT_EXPECT_SENT", [252, 254, 0]);
const EXPECT_DATAGRAMS_RECEIVED =
  parseIntegerArrayEnv("PICOQUIC_WT_EXPECT_DATAGRAMS_RECEIVED", null);
const EXPECT_DATAGRAM_LENGTHS =
  parseNonNegativeIntegerArrayEnv("PICOQUIC_WT_EXPECT_DATAGRAM_LENGTHS", null);
const EXPECT_DATAGRAMS_SENT = parseOptionalIntegerEnv("PICOQUIC_WT_EXPECT_DATAGRAMS_SENT");
const EXPECT_ORDERED = process.env.PICOQUIC_WT_EXPECT_ORDERED !== "0";
/* W3C WebTransport requires allowPooling+serverCertificateHashes to throw, but
 * Chrome 148.0.7778.181 constructed instead during local validation. Record the
 * diagnostic by default and let browser/version-specific lanes opt into gating.
 */
const REQUIRE_OPTIONS_CONSTRUCTOR = process.env.PICOQUIC_WT_OPTIONS_CONSTRUCTOR_REQUIRED === "1";
const INCLUDE_SERVER_SUMMARY = process.env.PICOQUIC_WT_INCLUDE_SERVER_SUMMARY === "1";
const SERVER_OUTPUT_LIMIT = INCLUDE_SERVER_SUMMARY ? 262144 : 32768;
const SERVER_SUMMARY_TRACE_LIMIT = 131072;
const SERVER_STREAM_TRACE_LIMIT = 65536;
const SERVER_SUMMARY_WAIT_MS = Number(process.env.PICOQUIC_WT_SERVER_SUMMARY_WAIT_MS || 2000);
const TIMEOUT_MS = Number(process.env.PICOQUIC_WT_TIMEOUT_MS || 30000);
const CDP_PORT = Number(process.env.PICOQUIC_WT_CDP_PORT || 9223);
const CDP_TIMEOUT_MS = Number(process.env.PICOQUIC_WT_CDP_TIMEOUT_MS || 30000);
/* Chrome 148 x64 under Rosetta on Apple Silicon was observed to stay alive but
 * never expose the DevTools endpoint with --headless=new. Keep the modern
 * default, but let local/CI runs select --headless=old when that startup
 * behavior is encountered.
 */
const CHROME_HEADLESS = process.env.PICOQUIC_WT_CHROME_HEADLESS || "new";
const CHROME_ARCH = process.env.PICOQUIC_WT_CHROME_ARCH || "";
const CHROME_ARCHES = new Set(["arm64", "x86_64"]);
const CHROME_IGNORE_CERT_ERRORS = process.env.PICOQUIC_WT_IGNORE_CERT_ERRORS !== "0";
const WT_URL = process.env.PICOQUIC_WT_URL ||
  `https://localhost:${PORT}/baton?version=0&baton=251&count=1`;
const PAGE_URL = process.env.PICOQUIC_WT_PAGE_URL ||
  pathToFileURL(join(WEB_ROOT, "index.html")).href;

const chromeNames = [
  process.env.CHROME_BIN,
  "google-chrome",
  "google-chrome-stable",
  "chromium",
  "chromium-browser",
  "chrome"
].filter(Boolean);

function sleep(ms) {
  return new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
}

function parseIntegerArrayEnv(name, fallback) {
  const value = process.env[name];
  if (!value) {
    return fallback;
  }
  const parsed = JSON.parse(value);
  if (!Array.isArray(parsed) || !parsed.every((entry) =>
    Number.isInteger(entry) && entry >= 0 && entry <= 255)) {
    throw new Error(`${name} must be a JSON array of baton byte values`);
  }
  return parsed;
}

function parseNonNegativeIntegerArrayEnv(name, fallback) {
  const value = process.env[name];
  if (!value) {
    return fallback;
  }
  const parsed = JSON.parse(value);
  if (!Array.isArray(parsed) || !parsed.every((entry) =>
    Number.isInteger(entry) && entry >= 0)) {
    throw new Error(`${name} must be a JSON array of non-negative integers`);
  }
  return parsed;
}

function parseOptionalIntegerEnv(name) {
  const value = process.env[name];
  if (!value) {
    return null;
  }
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`${name} must be a non-negative integer`);
  }
  return parsed;
}

function equalExpectedBatonArray(actual, expected) {
  if (!Array.isArray(actual)) {
    return false;
  }
  if (EXPECT_ORDERED) {
    return equalArray(actual, expected);
  }
  return equalArray([...actual].sort((a, b) => a - b),
    [...expected].sort((a, b) => a - b));
}

function terminateProcess(child, signal = "SIGTERM", timeoutMs = 3000) {
  if (!child || child.exitCode !== null || child.signalCode !== null) {
    return Promise.resolve();
  }
  return new Promise((resolveTerminate) => {
    let settled = false;
    const done = () => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        resolveTerminate();
      }
    };
    const timer = setTimeout(() => {
      try {
        child.kill("SIGKILL");
      } catch (_) {}
      done();
    }, timeoutMs);
    child.once("exit", done);
    try {
      child.kill(signal);
    } catch (_) {
      done();
    }
  });
}

async function removeTree(path) {
  for (let attempt = 0; attempt < 5; attempt++) {
    try {
      rmSync(path, { recursive: true, force: true });
      return;
    } catch (error) {
      if (!["ENOTEMPTY", "EBUSY", "EPERM"].includes(error.code) || attempt === 4) {
        throw error;
      }
      await sleep(100 * (attempt + 1));
    }
  }
}

function findOnPath(name) {
  if (name.includes("/") && existsSync(name)) {
    return name;
  }
  const path = process.env.PATH || "";
  for (const dir of path.split(":")) {
    const candidate = join(dir, name);
    if (existsSync(candidate)) {
      return candidate;
    }
  }
  return "";
}

function findChrome() {
  for (const name of chromeNames) {
    const found = findOnPath(name);
    if (found) {
      return found;
    }
  }
  return "";
}

function assertFile(path, label) {
  if (!existsSync(path)) {
    throw new Error(`${label} not found: ${path}`);
  }
}

function runChecked(command, args) {
  const child = spawn(command, args, { stdio: ["ignore", "pipe", "pipe"] });
  let stderr = "";
  return new Promise((resolveRun, rejectRun) => {
    child.stderr.on("data", (data) => {
      stderr += data.toString();
    });
    child.once("exit", (code, signal) => {
      if (code === 0) {
        resolveRun();
      } else {
        rejectRun(new Error(`${command} failed: code=${code} signal=${signal} ${stderr.trim()}`));
      }
    });
  });
}

async function getCertificateConfig(workDir) {
  const envCert = process.env.PICOQUIC_WT_CERT;
  const envKey = process.env.PICOQUIC_WT_KEY;
  if (envCert || envKey) {
    const cert = envCert || join(ROOT, "certs", "cert.pem");
    const key = envKey || join(ROOT, "certs", "key.pem");
    assertFile(cert, "certificate");
    assertFile(key, "private key");
    return {
      cert,
      key,
      hash: process.env.PICOQUIC_WT_CERT_HASH || certHash(cert)
    };
  }

  const certDir = join(workDir, "cert");
  const key = join(certDir, "key.pem");
  const cert = join(certDir, "cert.pem");
  rmSync(certDir, { recursive: true, force: true });
  mkdirSync(certDir, { recursive: true });
  await runChecked("openssl", [
    "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key
  ]);
  await runChecked("openssl", [
    "req", "-new", "-x509",
    "-key", key,
    "-out", cert,
    "-days", "13",
    "-subj", "/CN=localhost",
    "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
    "-addext", "keyUsage=digitalSignature",
    "-addext", "extendedKeyUsage=serverAuth"
  ]);

  return { cert, key, hash: process.env.PICOQUIC_WT_CERT_HASH || certHash(cert) };
}

function certHash(certPath) {
  const cert = new X509Certificate(readFileSync(certPath));
  return createHash("sha256").update(cert.raw).digest("base64url");
}

function buildPageUrl(certificateHash) {
  const url = new URL(PAGE_URL);
  url.searchParams.set("autorun", "1");
  url.searchParams.set("timeoutMs", String(TIMEOUT_MS));
  url.searchParams.set("url", WT_URL);
  url.searchParams.set("protocol", PROTOCOL);
  url.searchParams.set("certHash", certificateHash);
  url.searchParams.set("certHashAlg", CERT_HASH_ALG);
  if (!REQUIRE_DATAGRAM) {
    url.searchParams.set("requireDatagram", "0");
  }
  if (!USE_BYOB) {
    url.searchParams.set("useByob", "0");
  }
  if (DATAGRAM_RECEIVE_MODE !== "baton") {
    url.searchParams.set("datagramReceiveMode", DATAGRAM_RECEIVE_MODE);
  }
  if (DATAGRAM_RECEIVE_MIN !== null) {
    url.searchParams.set("datagramReceiveMin", String(DATAGRAM_RECEIVE_MIN));
  }
  if (DATAGRAM_SEND_MODE !== "baton") {
    url.searchParams.set("datagramSendMode", DATAGRAM_SEND_MODE);
  }
  if (DATAGRAM_SEND_SIZE !== null) {
    url.searchParams.set("datagramSendSize", String(DATAGRAM_SEND_SIZE));
  }
  if (DATAGRAM_SEND_COUNT !== null) {
    url.searchParams.set("datagramSendCount", String(DATAGRAM_SEND_COUNT));
  }
  if (STREAM_MODE !== "baton") {
    url.searchParams.set("streamMode", STREAM_MODE);
    url.searchParams.set("streamSize", String(STREAM_SIZE));
    url.searchParams.set("streamCount", String(STREAM_COUNT));
  }
  return url.href;
}

function waitForServer(child) {
  return new Promise((resolveReady, rejectReady) => {
    let settled = false;
    const startupTimer = setTimeout(() => {
      if (!settled) {
        settled = true;
        clearTimeout(failTimer);
        resolveReady();
      }
    }, 750);
    const failTimer = setTimeout(() => {
      if (!settled) {
        settled = true;
        clearTimeout(startupTimer);
        rejectReady(new Error("pico_baton did not report readiness"));
      }
    }, 5000);

    function onData(data) {
      if (!settled && data.toString().includes("Waiting for packets")) {
        settled = true;
        clearTimeout(startupTimer);
        clearTimeout(failTimer);
        resolveReady();
      }
    }

    child.stdout.on("data", onData);
    child.stderr.on("data", onData);
    child.once("exit", (code, signal) => {
      if (!settled) {
        settled = true;
        clearTimeout(startupTimer);
        clearTimeout(failTimer);
        rejectReady(new Error(`pico_baton exited before readiness: code=${code} signal=${signal}`));
      }
    });
  });
}

async function waitForCdpEndpoint() {
  const endpoint = `http://127.0.0.1:${CDP_PORT}`;
  const deadline = Date.now() + CDP_TIMEOUT_MS;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${endpoint}/json/version`);
      if (response.ok) {
        return endpoint;
      }
    } catch (_) {}
    await sleep(100);
  }
  throw new Error("Chrome DevTools endpoint did not become ready");
}

async function readChromeBrowserInfo(endpoint) {
  const response = await fetch(`${endpoint}/json/version`);
  if (!response.ok) {
    throw new Error(`Chrome DevTools version query failed: HTTP ${response.status}`);
  }
  const version = await response.json();
  const product = version.Browser || "";
  const slash = product.indexOf("/");
  return {
    browserName: slash > 0 ? product.slice(0, slash) : (product || "Chrome"),
    browserVersion: slash >= 0 ? product.slice(slash + 1) : "",
    product,
    protocolVersion: version["Protocol-Version"] || "",
    userAgent: version["User-Agent"] || "",
    platformName: process.platform
  };
}

async function newTarget(endpoint) {
  const targetUrl = `${endpoint}/json/new?about%3Ablank`;
  let response = await fetch(targetUrl, { method: "PUT" });
  if (!response.ok) {
    response = await fetch(targetUrl);
  }
  if (!response.ok) {
    throw new Error(`cannot create Chrome target: HTTP ${response.status}`);
  }
  const target = await response.json();
  if (!target.webSocketDebuggerUrl) {
    throw new Error("Chrome target did not include a websocket URL");
  }
  return target.webSocketDebuggerUrl;
}

function connectCdp(url) {
  return new Promise((resolveSocket, rejectSocket) => {
    const socket = new WebSocket(url);
    const pending = new Map();
    let nextId = 1;

    socket.addEventListener("open", () => {
      resolveSocket({
        close() {
          socket.close();
        },
        send(method, params = {}) {
          const id = nextId++;
          socket.send(JSON.stringify({ id, method, params }));
          return new Promise((resolveCommand, rejectCommand) => {
            pending.set(id, { resolveCommand, rejectCommand });
          });
        }
      });
    }, { once: true });

    socket.addEventListener("error", () => {
      rejectSocket(new Error("Chrome DevTools websocket failed"));
    }, { once: true });

    socket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (!message.id || !pending.has(message.id)) {
        return;
      }
      const pendingCommand = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) {
        pendingCommand.rejectCommand(new Error(message.error.message));
      } else {
        pendingCommand.resolveCommand(message.result);
      }
    });
  });
}

async function waitForHarness(cdp) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const probe = await cdp.send("Runtime.evaluate", {
      expression: "Boolean(window.__picoquicWebTransportResult)",
      returnByValue: true
    });
    if (probe.result && probe.result.value) {
      return;
    }
    await sleep(100);
  }
  const diagnostic = await cdp.send("Runtime.evaluate", {
    expression: "({ href: location.href, readyState: document.readyState, title: document.title, body: document.body ? document.body.innerText.slice(0, 500) : '' })",
    returnByValue: true
  });
  throw new Error(`browser harness did not start: ${JSON.stringify(diagnostic.result.value)}`);
}

async function readHarnessResult(cdp) {
  const expression =
    "Promise.race([" +
    "window.__picoquicWebTransportResult," +
    `new Promise((_, reject) => setTimeout(() => reject(new Error('timeout after ${TIMEOUT_MS} ms')), ${TIMEOUT_MS}))` +
    "])";
  const result = await cdp.send("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text || "browser harness failed");
  }
  return result.result.value;
}

function equalArray(actual, expected) {
  return Array.isArray(actual) &&
    actual.length === expected.length &&
    actual.every((value, index) => value === expected[index]);
}

function assertHarnessResult(result) {
  if (!EXPECT_OK) {
    if (!result || result.ok !== false || !result.error) {
      throw new Error(`browser harness unexpectedly succeeded: ${JSON.stringify(result)}`);
    }
    return;
  }
  if (!result || result.ok !== true) {
    throw new Error(`browser harness failed: ${JSON.stringify(result)}`);
  }
  if (result.protocol !== PROTOCOL) {
    throw new Error(`unexpected protocol: ${result.protocol}`);
  }
  if (result.requireDatagram !== REQUIRE_DATAGRAM ||
    result.constructorRequireUnreliable !== REQUIRE_DATAGRAM) {
    throw new Error(`unexpected datagram requirement mode: ${JSON.stringify({
      requireDatagram: result.requireDatagram,
      constructorRequireUnreliable: result.constructorRequireUnreliable,
      expected: REQUIRE_DATAGRAM
    })}`);
  }
  if (result.useByob !== USE_BYOB) {
    throw new Error(`unexpected stream reader mode: ${JSON.stringify({
      useByob: result.useByob,
      expected: USE_BYOB
    })}`);
  }
  if ((result.streamMode || "baton") !== STREAM_MODE) {
    throw new Error(`unexpected stream mode: ${JSON.stringify({
      streamMode: result.streamMode,
      expected: STREAM_MODE
    })}`);
  }
  if (STREAM_MODE !== "baton") {
    if (result.streamSize !== STREAM_SIZE || result.streamCount !== STREAM_COUNT) {
      throw new Error(`unexpected stream parameters: ${JSON.stringify({
        streamSize: result.streamSize,
        streamCount: result.streamCount,
        expectedSize: STREAM_SIZE,
        expectedCount: STREAM_COUNT
      })}`);
    }
    return;
  }
  if (result.datagramReceiveMode !== DATAGRAM_RECEIVE_MODE) {
    throw new Error(`unexpected datagram receive mode: ${JSON.stringify({
      datagramReceiveMode: result.datagramReceiveMode,
      expected: DATAGRAM_RECEIVE_MODE
    })}`);
  }
  if ((result.datagramSendMode || "baton") !== DATAGRAM_SEND_MODE) {
    throw new Error(`unexpected datagram send mode: ${JSON.stringify({
      datagramSendMode: result.datagramSendMode,
      expected: DATAGRAM_SEND_MODE
    })}`);
  }
  if (!equalExpectedBatonArray(result.received, EXPECT_RECEIVED)) {
    throw new Error(`unexpected received baton sequence: ${JSON.stringify(result.received)}`);
  }
  if (!equalExpectedBatonArray(result.sent, EXPECT_SENT)) {
    throw new Error(`unexpected sent baton sequence: ${JSON.stringify(result.sent)}`);
  }
  if (EXPECT_DATAGRAMS_RECEIVED &&
    !equalArray(result.datagramsReceived, EXPECT_DATAGRAMS_RECEIVED)) {
    throw new Error(`unexpected received datagram sequence: ${JSON.stringify(result.datagramsReceived)}`);
  }
  if (EXPECT_DATAGRAM_LENGTHS &&
    !equalArray(result.datagramLengths, EXPECT_DATAGRAM_LENGTHS)) {
    throw new Error(`unexpected datagram lengths: ${JSON.stringify(result.datagramLengths)}`);
  }
  if (EXPECT_DATAGRAMS_SENT !== null && result.datagramsSent !== EXPECT_DATAGRAMS_SENT) {
    throw new Error(`unexpected datagram sent count: ${JSON.stringify(result.datagramsSent)}`);
  }
  if (REQUIRE_DATAGRAM &&
    (!Array.isArray(result.datagramsReceived) || result.datagramsReceived.length === 0) &&
    (!Array.isArray(result.datagramLengths) || result.datagramLengths.length === 0)) {
    throw new Error("no WebTransport datagram received");
  }
}

async function readProtocolConstructorResult(cdp, certificateHash) {
  const options = JSON.stringify({
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL
  });
  const result = await cdp.send("Runtime.evaluate", {
    expression: `window.picoquicWebTransportBaton.runProtocolConstructorTests(${options})`,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text ||
      "browser protocol constructor tests failed");
  }
  return result.result.value;
}

async function readUrlConstructorResult(cdp, certificateHash) {
  const options = JSON.stringify({
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL
  });
  const result = await cdp.send("Runtime.evaluate", {
    expression: `window.picoquicWebTransportBaton.runUrlConstructorTests(${options})`,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text ||
      "browser URL constructor tests failed");
  }
  return result.result.value;
}

async function readOptionsConstructorResult(cdp, certificateHash) {
  const options = JSON.stringify({
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL
  });
  const result = await cdp.send("Runtime.evaluate", {
    expression: `window.picoquicWebTransportBaton.runOptionsConstructorTests(${options})`,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text ||
      "browser options constructor tests failed");
  }
  return result.result.value;
}

async function readWritableBadChunkResult(cdp, certificateHash) {
  const options = JSON.stringify({
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL,
    requireDatagram: REQUIRE_DATAGRAM
  });
  const result = await cdp.send("Runtime.evaluate", {
    expression: `window.picoquicWebTransportBaton.runWritableBadChunkTests(${options})`,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text ||
      "browser writable bad chunk tests failed");
  }
  return result.result.value;
}

async function readCloseSessionResult(cdp, certificateHash) {
  const options = JSON.stringify({
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL,
    requireDatagram: REQUIRE_DATAGRAM
  });
  const result = await cdp.send("Runtime.evaluate", {
    expression: `window.picoquicWebTransportBaton.runCloseSessionTests(${options})`,
    awaitPromise: true,
    returnByValue: true
  });

  if (result.exceptionDetails) {
    const text = result.exceptionDetails.exception &&
      result.exceptionDetails.exception.description;
    throw new Error(text || result.exceptionDetails.text ||
      "browser close-session tests failed");
  }
  return result.result.value;
}

function assertProtocolConstructorResult(result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser protocol constructor tests failed: ${JSON.stringify(result)}`);
  }
}

function assertUrlConstructorResult(result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser URL constructor tests failed: ${JSON.stringify(result)}`);
  }
}

function assertOptionsConstructorResult(result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser options constructor tests failed: ${JSON.stringify(result)}`);
  }
}

function assertDatagramWritableResult(result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser datagram writable tests failed: ${JSON.stringify(result)}`);
  }
}

function assertStreamWritableResult(result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser stream writable tests failed: ${JSON.stringify(result)}`);
  }
}

function countMatches(text, pattern) {
  const matches = text.match(pattern);
  return matches ? matches.length : 0;
}

function sumMatches(text, pattern) {
  let total = 0;
  for (const match of text.matchAll(pattern)) {
    total += Number(match[1]);
  }
  return total;
}

function serverOutputHasBrowserClose(output) {
  return /error: 2a \(browser-close-test\)/.test(output);
}

async function waitForServerOutput(predicate, getOutput, timeoutMs = SERVER_SUMMARY_WAIT_MS) {
  const boundedTimeoutMs = Number.isFinite(timeoutMs) && timeoutMs > 0 ? timeoutMs : 0;
  const deadline = Date.now() + boundedTimeoutMs;

  while (!predicate(getOutput())) {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      break;
    }
    await sleep(Math.min(25, remaining));
  }
}

function summarizeServerOutput(output) {
  return {
    bytesCaptured: output.length,
    waitingForPackets: output.includes("Waiting for packets"),
    connectAccepted: countMatches(output, /Connect accepted on stream/g),
    optionalProtocolAccepted: countMatches(output,
      /Accepting optional-protocol WebTransport CONNECT/g),
    packetsReceived: countMatches(output, /Receiving packet type/g),
    packetsSent: countMatches(output, /Sending packet type/g),
    h3ControlFrames: countMatches(output, /H3 control frame/g),
    originMissing: countMatches(output, /Missing WebTransport CONNECT origin/g),
    originRejected: countMatches(output, /WebTransport CONNECT origin rejected/g),
    batonParameterRejected: countMatches(output,
      /Rejecting malformed baton WebTransport CONNECT parameters/g),
    closeSessionReceived: countMatches(output,
      /Received web transport session capsule, type: 0x[0-9a-f]+ \(close session\)/g),
    emptyDatagramsReceived: countMatches(output,
      /Received empty WebTransport datagram on stream/g),
    batonDatagramsReceived: countMatches(output,
      /Received baton WebTransport datagram on stream/g),
    sizedDatagramsReceived: countMatches(output,
      /Received sized WebTransport datagram on stream/g),
    datagramBytesReceived: sumMatches(output,
      /Received (?:baton|sized) WebTransport datagram on stream: [0-9]+, length: ([0-9]+)/g),
    zeroBatonReceived: countMatches(output, /All ZERO baton on stream/g),
    streamTestFinReceived: countMatches(output,
      /WebTransport stream test received FIN on stream/g),
    streamTestFinSent: countMatches(output,
      /WebTransport stream test sent FIN on stream/g),
    streamTestBytesReceived: sumMatches(output,
      /WebTransport stream test received FIN on stream: [0-9]+, bytes: ([0-9]+)/g),
    streamTestBytesSent: sumMatches(output,
      /WebTransport stream test sent FIN on stream: [0-9]+, bytes: ([0-9]+)/g),
    resetStreamReceived: countMatches(output,
      /Received WebTransport RESET_STREAM on stream/g),
    stopSendingReceived: countMatches(output,
      /Received WebTransport STOP_SENDING on stream/g),
    resetStreamAppError123: countMatches(output,
      /Received WebTransport RESET_STREAM on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    stopSendingAppError123: countMatches(output,
      /Received WebTransport STOP_SENDING on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    writableBadChunkCloseReceived:
      output.includes("error: 0 (writable-bad-chunk-test)"),
    browserCloseReceived: serverOutputHasBrowserClose(output)
  };
}

function expectedStreamServerSummary() {
  const bytes = STREAM_SIZE * STREAM_COUNT;
  if (STREAM_MODE === "client-bidi-echo" ||
    STREAM_MODE === "client-uni-reply") {
    return {
      bytesReceived: bytes,
      bytesSent: bytes,
      finReceived: STREAM_COUNT,
      finSent: STREAM_COUNT
    };
  }
  if (STREAM_MODE === "server-uni") {
    return {
      bytesReceived: 0,
      bytesSent: bytes,
      finReceived: 0,
      finSent: STREAM_COUNT
    };
  }
  if (STREAM_MODE === "server-bidi") {
    return {
      bytesReceived: 0,
      bytesSent: bytes,
      finReceived: STREAM_COUNT,
      finSent: STREAM_COUNT
    };
  }
  return null;
}

function expectedResetServerSummary() {
  if (STREAM_MODE === "browser-abort-bidi" ||
    STREAM_MODE === "browser-abort-uni") {
    return {
      resetStreamReceived: 1,
      resetStreamAppError123: 1,
      stopSendingReceived: 0,
      stopSendingAppError123: 0
    };
  }
  if (STREAM_MODE === "browser-cancel-incoming-bidi" ||
    STREAM_MODE === "browser-cancel-incoming-uni") {
    return {
      resetStreamReceived: 0,
      resetStreamAppError123: 0,
      stopSendingReceived: 1,
      stopSendingAppError123: 1
    };
  }
  return null;
}

function serverOutputHasResetSummary(output) {
  const expected = expectedResetServerSummary();
  if (!expected) {
    return true;
  }
  const summary = summarizeServerOutput(output);
  return summary.resetStreamReceived >= expected.resetStreamReceived &&
    summary.resetStreamAppError123 >= expected.resetStreamAppError123 &&
    summary.stopSendingReceived >= expected.stopSendingReceived &&
    summary.stopSendingAppError123 >= expected.stopSendingAppError123;
}

function serverOutputHasStreamTestSummary(output) {
  const expected = expectedStreamServerSummary();
  if (!expected) {
    return true;
  }
  const summary = summarizeServerOutput(output);
  return summary.streamTestBytesReceived >= expected.bytesReceived &&
    summary.streamTestBytesSent >= expected.bytesSent &&
    summary.streamTestFinReceived >= expected.finReceived &&
    summary.streamTestFinSent >= expected.finSent;
}

function mergeStreamTestSummary(summary, streamSummary) {
  if (!streamSummary) {
    return summary;
  }
  for (const name of ["streamTestBytesReceived", "streamTestBytesSent",
    "streamTestFinReceived", "streamTestFinSent"]) {
    summary[name] = streamSummary[name] || 0;
  }
  return summary;
}

function isServerSummaryLine(line) {
  return line.includes("Waiting for packets") ||
    line.includes("Connect accepted on stream") ||
    line.includes("Accepting optional-protocol WebTransport CONNECT") ||
    line.includes("H3 control frame") ||
    line.includes("Missing WebTransport CONNECT origin") ||
    line.includes("WebTransport CONNECT origin rejected") ||
    line.includes("Rejecting malformed baton WebTransport CONNECT parameters") ||
    line.includes("Received web transport session capsule") ||
    line.includes("Received empty WebTransport datagram on stream") ||
    line.includes("Received baton WebTransport datagram on stream") ||
    line.includes("Received sized WebTransport datagram on stream") ||
    line.includes("All ZERO baton on stream") ||
    line.includes("WebTransport stream test ") ||
    line.includes("Received WebTransport RESET_STREAM") ||
    line.includes("Received WebTransport STOP_SENDING") ||
    line.includes("error: 0 (writable-bad-chunk-test)") ||
    line.includes("error: 2a (browser-close-test)");
}

function makeServerOutputRecorder() {
  let output = "";
  let summaryTrace = "";
  let streamTrace = "";
  let lineBuffer = "";
  let packetReceivedCaptured = false;
  let packetSentCaptured = false;

  return {
    append(data) {
      const text = data.toString();
      output = (output + text).slice(-SERVER_OUTPUT_LIMIT);

      lineBuffer += text;
      const lines = lineBuffer.split(/\r?\n/);
      lineBuffer = lines.pop() || "";
      for (const line of lines) {
        let keepSummaryLine = isServerSummaryLine(line);
        if (!keepSummaryLine && line.includes("Receiving packet type")) {
          keepSummaryLine = !packetReceivedCaptured;
          packetReceivedCaptured = true;
        }
        if (!keepSummaryLine && line.includes("Sending packet type")) {
          keepSummaryLine = !packetSentCaptured;
          packetSentCaptured = true;
        }
        if (keepSummaryLine) {
          summaryTrace = (summaryTrace + line + "\n").slice(-SERVER_SUMMARY_TRACE_LIMIT);
        }
        if (line.includes("WebTransport stream test ")) {
          streamTrace = (streamTrace + line + "\n").slice(-SERVER_STREAM_TRACE_LIMIT);
        }
      }
    },
    output() {
      return output;
    },
    summaryTrace() {
      const pending = isServerSummaryLine(lineBuffer) ||
        lineBuffer.includes("Receiving packet type") ||
        lineBuffer.includes("Sending packet type") ? `${lineBuffer}\n` : "";
      return summaryTrace + pending;
    },
    streamTrace() {
      const pending = lineBuffer.includes("WebTransport stream test ") ?
        `${lineBuffer}\n` : "";
      return streamTrace + pending;
    }
  };
}

async function main() {
  assertFile(BATON, "pico_baton");

  const chrome = findChrome();
  if (!chrome) {
    throw new Error("No Chrome/Chromium binary found. Set CHROME_BIN to run this test.");
  }

  const profile = mkdtempSync(join(tmpdir(), "picoquic-wt-chrome-"));
  const certConfig = await getCertificateConfig(profile);
  const targetUrl = buildPageUrl(certConfig.hash);
  const serverArgs = [
    "-p", String(PORT),
    "-c", certConfig.cert,
    "-k", certConfig.key,
    "-w", WEB_ROOT,
    "/baton"
  ];
  if (process.env.PICOQUIC_WT_SERVER_LOG || INCLUDE_SERVER_SUMMARY) {
    const serverLog = process.env.PICOQUIC_WT_SERVER_LOG || "1";
    const logTarget = serverLog === "1" ? "-" : serverLog;
    serverArgs.splice(serverArgs.length - 1, 0, "-l", logTarget, "-L");
  }
  const server = spawn(BATON, serverArgs, { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
  const serverOutput = makeServerOutputRecorder();
  server.stdout.on("data", (data) => serverOutput.append(data));
  server.stderr.on("data", (data) => serverOutput.append(data));

  let chromeProcess = null;
  let cdp = null;
  let browserInfo = {};
  try {
    await waitForServer(server);

    const chromeArgs = [
      `--headless=${CHROME_HEADLESS}`,
      "--no-first-run",
      "--disable-background-networking",
      "--disable-dev-shm-usage",
      "--disable-gpu",
      "--enable-quic",
      ...(CHROME_IGNORE_CERT_ERRORS ? ["--ignore-certificate-errors"] : []),
      `--origin-to-force-quic-on=localhost:${PORT}`,
      `--remote-debugging-port=${CDP_PORT}`,
      `--user-data-dir=${profile}`,
      "about:blank"
    ];
    if (typeof process.getuid === "function" && process.getuid() === 0) {
      chromeArgs.splice(1, 0, "--no-sandbox");
    }
    if (process.env.PICOQUIC_WT_NETLOG) {
      chromeArgs.splice(chromeArgs.length - 1, 0,
        `--log-net-log=${process.env.PICOQUIC_WT_NETLOG}`,
        "--net-log-capture-mode=Everything");
    }
    if (CHROME_ARCH && !CHROME_ARCHES.has(CHROME_ARCH)) {
      throw new Error(`Unsupported PICOQUIC_WT_CHROME_ARCH=${JSON.stringify(CHROME_ARCH)}`);
    }
    const chromeCommand = CHROME_ARCH ? "/usr/bin/arch" : chrome;
    const chromeCommandArgs = CHROME_ARCH ? [`-${CHROME_ARCH}`, chrome, ...chromeArgs] : chromeArgs;
    chromeProcess = spawn(chromeCommand, chromeCommandArgs, { stdio: ["ignore", "ignore", "pipe"] });
    let chromeStderr = "";
    chromeProcess.stderr.on("data", (data) => {
      chromeStderr = (chromeStderr + data.toString()).slice(-4096);
    });

    let endpoint = "";
    try {
      endpoint = await waitForCdpEndpoint();
      browserInfo = await readChromeBrowserInfo(endpoint);
    } catch (error) {
      const stderr = chromeStderr.trim();
      throw new Error(stderr ? `${error.message}: ${stderr}` : error.message);
    }
    cdp = await connectCdp(await newTarget(endpoint));
    await cdp.send("Page.enable");
    await cdp.send("Runtime.enable");
    await cdp.send("Page.navigate", { url: targetUrl });
    await waitForHarness(cdp);
    const result = await readHarnessResult(cdp);
    result.browser = browserInfo;
    let streamServerSummary = null;
    if (INCLUDE_SERVER_SUMMARY && STREAM_MODE !== "baton") {
      await waitForServerOutput(serverOutputHasStreamTestSummary,
        () => serverOutput.streamTrace());
      await waitForServerOutput(serverOutputHasResetSummary,
        () => serverOutput.summaryTrace());
      streamServerSummary = summarizeServerOutput(serverOutput.streamTrace());
    }
    if (INCLUDE_SERVER_SUMMARY) {
      result.server = mergeStreamTestSummary(summarizeServerOutput(serverOutput.summaryTrace()),
        streamServerSummary);
    }
    assertHarnessResult(result);
    if (RUN_PROTOCOL_CONSTRUCTOR && EXPECT_OK && STREAM_MODE === "baton") {
      result.protocolConstructor = await readProtocolConstructorResult(cdp, certConfig.hash);
      assertProtocolConstructorResult(result.protocolConstructor);
      result.urlConstructor = await readUrlConstructorResult(cdp, certConfig.hash);
      assertUrlConstructorResult(result.urlConstructor);
      result.optionsConstructor = await readOptionsConstructorResult(cdp, certConfig.hash);
      if (REQUIRE_OPTIONS_CONSTRUCTOR) {
        assertOptionsConstructorResult(result.optionsConstructor);
      }
      const writableBadChunk = await readWritableBadChunkResult(cdp, certConfig.hash);
      result.datagramWritable = writableBadChunk.datagramWritable;
      assertDatagramWritableResult(result.datagramWritable);
      result.streamWritable = writableBadChunk.streamWritable;
      assertStreamWritableResult(result.streamWritable);
      result.closeSession = await readCloseSessionResult(cdp, certConfig.hash);
      if (INCLUDE_SERVER_SUMMARY && result.closeSession && result.closeSession.ok === true) {
        /* GitHub run 26838267520 showed Chrome job 79137977278 and Edge job
         * 79137977123 completing the browser close diagnostic before Node had
         * received pico_baton's close-capsule log. The Chrome rerun job
         * 79138790636 reproduced it. Keep the server assertion, but wait
         * briefly for the expected trace line before summarizing.
        */
        await waitForServerOutput(serverOutputHasBrowserClose,
          () => serverOutput.summaryTrace());
      }
    }
    if (INCLUDE_SERVER_SUMMARY) {
      result.server = mergeStreamTestSummary(summarizeServerOutput(serverOutput.summaryTrace()),
        streamServerSummary);
    }
    console.log(JSON.stringify(result, null, 2));
  } catch (error) {
    const output = [
      serverOutput.output().trim(),
      serverOutput.summaryTrace().trim() ?
        `server summary trace:\n${serverOutput.summaryTrace().trim()}` : "",
      serverOutput.streamTrace().trim() ?
        `server stream trace:\n${serverOutput.streamTrace().trim()}` : ""
    ].filter(Boolean).join("\n");
    if (output) {
      throw new Error(`${error.message}\nserver output:\n${output}`);
    }
    throw error;
  } finally {
    if (cdp) {
      cdp.close();
    }
    await terminateProcess(chromeProcess);
    await terminateProcess(server);
    await removeTree(profile);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
