#!/usr/bin/env node

import { spawn } from "node:child_process";
import { createHash, X509Certificate } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { extname, join, resolve, sep } from "node:path";

const ROOT = resolve(new URL("../../..", import.meta.url).pathname);
const BATON = process.env.PICO_BATON_BIN || join(ROOT, "build", "pico_baton");
const WEB_ROOT = process.env.PICOQUIC_WT_WEB_ROOT ||
  join(ROOT, "tests", "webtransport", "browser");
const PORT = Number(process.env.PICOQUIC_WT_PORT || 4433);
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
/* Firefox 151.0.2 in GitHub Actions accepted invalid protocols constructor
 * inputs instead of throwing; see run 26801732536. Keep recording the
 * diagnostic, but let the Firefox expected-results manifest classify it as a
 * browser API gap while the lane exercises picoquic's data path.
 */
const REQUIRE_PROTOCOL_CONSTRUCTOR =
  process.env.PICOQUIC_WT_PROTOCOL_CONSTRUCTOR_REQUIRED !== "0";
const REQUIRE_OPTIONS_CONSTRUCTOR = process.env.PICOQUIC_WT_OPTIONS_CONSTRUCTOR_REQUIRED === "1";
/* Firefox 151.0.2 in GitHub Actions can reject the bidirectional writable
 * bad-chunk diagnostic setup with InvalidStateError after multiple sequential
 * WebTransport sessions; see run 26802934161. Keep recording the diagnostic,
 * but let the expected-results manifest classify the Firefox core E2E gap.
 */
const REQUIRE_STREAM_WRITABLE = process.env.PICOQUIC_WT_STREAM_WRITABLE_REQUIRED !== "0";
const INCLUDE_SERVER_SUMMARY = process.env.PICOQUIC_WT_INCLUDE_SERVER_SUMMARY === "1";
const SERVER_OUTPUT_LIMIT = INCLUDE_SERVER_SUMMARY ? 262144 : 32768;
const SERVER_SUMMARY_TRACE_LIMIT = 131072;
const SERVER_STREAM_TRACE_LIMIT = 65536;
const SERVER_SUMMARY_WAIT_MS = Number(process.env.PICOQUIC_WT_SERVER_SUMMARY_WAIT_MS || 2000);
const TIMEOUT_MS = Number(process.env.PICOQUIC_WT_TIMEOUT_MS || 30000);
const GECKO_DRIVER_PORT = Number(process.env.PICOQUIC_WT_GECKO_DRIVER_PORT || 9445);
const HARNESS_PORT = Number(process.env.PICOQUIC_WT_HARNESS_PORT || 8081);
const FIREFOX_HEADLESS = process.env.PICOQUIC_WT_FIREFOX_HEADLESS !== "0";
/* Firefox 151.0.2 in GitHub Actions did not provide a usable
 * WT-Available-Protocols value to pico_baton after CONNECT reached the server;
 * see run 26801289699. This opt-in keeps the Firefox lane on the data path
 * without claiming WT-Protocol selection for Firefox.
 */
const FIREFOX_PROTOCOL_OPTIONAL = process.env.PICOQUIC_WT_FIREFOX_PROTOCOL_OPTIONAL === "1";
/* Firefox 151.0.2 in GitHub Actions rejected the local self-signed
 * WebTransport endpoint before CONNECT when relying only on
 * serverCertificateHashes; see run 26801130235. Keep this WebDriver-only
 * certificate bypass opt-in isolated to Firefox lanes so the browser can still
 * exercise picoquic's WebTransport data path. The Firefox expected-results
 * manifest skips the wrong-hash scenario while this browser path is in use.
 */
const FIREFOX_ACCEPT_INSECURE_CERTS =
  process.env.PICOQUIC_WT_FIREFOX_ACCEPT_INSECURE_CERTS === "1";
const FIREFOX_BIN = process.env.FIREFOX_BIN || "";
const RAW_WT_URL = process.env.PICOQUIC_WT_URL ||
  `https://localhost:${PORT}/baton?version=0&baton=251&count=1`;
const WT_URL = firefoxWtUrl(RAW_WT_URL);
const REQUESTED_PROTOCOL = process.env.PICOQUIC_WT_PROTOCOL || "devious-baton-00";
const PROTOCOL = FIREFOX_PROTOCOL_OPTIONAL ? "" : REQUESTED_PROTOCOL;
const REQUIRE_PROTOCOL = process.env.PICOQUIC_WT_REQUIRE_PROTOCOL !== "0";
const PAGE_URL = process.env.PICOQUIC_WT_PAGE_URL || "";

const geckoDriverNames = [
  process.env.GECKO_DRIVER_BIN,
  process.env.GECKODRIVER_BIN,
  "geckodriver"
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

function findGeckoDriver() {
  for (const name of geckoDriverNames) {
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

function useFirefoxProtocolOptionalEndpoint() {
  /* Firefox 151.0.3 CI still needs the protocol=optional endpoint to exercise
   * stream-mode data paths. Keep strict negative/core rows unchanged when they
   * are expected to reject, but let expected stream-mode browser gaps still
   * reach pico_baton so server FIN/byte assertions remain meaningful.
   */
  return FIREFOX_PROTOCOL_OPTIONAL && (EXPECT_OK || STREAM_MODE !== "baton");
}

function firefoxWtUrl(rawUrl) {
  if (!useFirefoxProtocolOptionalEndpoint()) {
    return rawUrl;
  }
  const url = new URL(rawUrl);
  url.searchParams.set("protocol", "optional");
  return url.href;
}

function buildPageUrl(pageUrl, certificateHash) {
  const url = new URL(pageUrl);
  url.searchParams.set("autorun", "1");
  url.searchParams.set("timeoutMs", String(TIMEOUT_MS));
  url.searchParams.set("url", WT_URL);
  url.searchParams.set("protocol", REQUESTED_PROTOCOL);
  if (!REQUIRE_PROTOCOL || useFirefoxProtocolOptionalEndpoint()) {
    url.searchParams.set("requireProtocol", "0");
  }
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

function contentType(filePath) {
  switch (extname(filePath)) {
  case ".html":
    return "text/html; charset=utf-8";
  case ".js":
    return "text/javascript; charset=utf-8";
  case ".css":
    return "text/css; charset=utf-8";
  default:
    return "application/octet-stream";
  }
}

function startHarnessServer() {
  const root = resolve(WEB_ROOT);
  const server = createServer((request, response) => {
    try {
      const requestUrl = new URL(request.url || "/", "http://127.0.0.1");
      let pathname = decodeURIComponent(requestUrl.pathname);
      if (pathname === "/") {
        pathname = "/index.html";
      }
      const filePath = resolve(root, `.${pathname}`);
      if (filePath !== root && !filePath.startsWith(root + sep)) {
        response.writeHead(403);
        response.end("forbidden");
        return;
      }
      if (!existsSync(filePath)) {
        response.writeHead(404);
        response.end("not found");
        return;
      }
      response.writeHead(200, {
        "content-type": contentType(filePath),
        "cache-control": "no-store"
      });
      response.end(readFileSync(filePath));
    } catch (error) {
      response.writeHead(500);
      response.end(error && error.message ? error.message : String(error));
    }
  });

  return new Promise((resolveServer, rejectServer) => {
    server.once("error", rejectServer);
    server.listen(HARNESS_PORT, "127.0.0.1", () => {
      server.off("error", rejectServer);
      resolveServer({
        close() {
          server.close();
        },
        url: `http://127.0.0.1:${HARNESS_PORT}/index.html`
      });
    });
  });
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

async function waitForDriver(endpoint, childOutput) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${endpoint}/status`);
      if (response.ok) {
        return;
      }
    } catch (_) {}
    await sleep(100);
  }
  throw new Error(`geckodriver did not become ready: ${childOutput()}`);
}

async function webdriver(endpoint, method, path, body) {
  const response = await fetch(`${endpoint}${path}`, {
    method,
    headers: body === undefined ? undefined : { "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  const text = await response.text();
  let parsed = null;
  try {
    parsed = text ? JSON.parse(text) : null;
  } catch (_) {
    parsed = text;
  }

  if (!response.ok) {
    const value = parsed && parsed.value ? parsed.value : parsed;
    const message = value && value.message ? value.message : JSON.stringify(value);
    throw new Error(`WebDriver ${method} ${path} failed: HTTP ${response.status}: ${message}`);
  }
  return parsed ? parsed.value : null;
}

async function newFirefoxSession(endpoint) {
  const firefoxOptions = {
    args: FIREFOX_HEADLESS ? ["-headless"] : []
  };
  if (FIREFOX_BIN) {
    assertFile(FIREFOX_BIN, "Firefox binary");
    firefoxOptions.binary = FIREFOX_BIN;
  }

  const value = await webdriver(endpoint, "POST", "/session", {
    capabilities: {
      alwaysMatch: {
        browserName: "firefox",
        acceptInsecureCerts: FIREFOX_ACCEPT_INSECURE_CERTS,
        "moz:firefoxOptions": firefoxOptions
      }
    }
  });
  if (!value || !value.sessionId) {
    throw new Error(`Firefox session response did not include sessionId: ${JSON.stringify(value)}`);
  }
  return value;
}

async function executeScript(endpoint, sessionId, script, args = []) {
  return webdriver(endpoint, "POST", `/session/${sessionId}/execute/sync`, {
    script,
    args
  });
}

async function executeAsyncScript(endpoint, sessionId, script, args = []) {
  return webdriver(endpoint, "POST", `/session/${sessionId}/execute/async`, {
    script,
    args
  });
}

async function waitForHarness(endpoint, sessionId) {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const isReady = await executeScript(endpoint, sessionId,
      "return Boolean(window.__picoquicWebTransportResult);");
    if (isReady) {
      return;
    }
    await sleep(100);
  }
  const diagnostic = await executeScript(endpoint, sessionId,
    "return { href: location.href, readyState: document.readyState, " +
    "title: document.title, body: document.body ? document.body.innerText.slice(0, 500) : '' };");
  throw new Error(`browser harness did not start: ${JSON.stringify(diagnostic)}`);
}

async function readHarnessResult(endpoint, sessionId) {
  const wrapped = await executeAsyncScript(endpoint, sessionId, `
    const done = arguments[arguments.length - 1];
    function errorText(error) {
      if (error && typeof error === "object") {
        return (error.name ? error.name + ": " : "") + (error.message || String(error));
      }
      return String(error);
    }
    Promise.race([
      window.__picoquicWebTransportResult,
      new Promise((_, reject) => setTimeout(() =>
        reject(new Error("timeout after ${TIMEOUT_MS} ms")), ${TIMEOUT_MS}))
    ]).then(
      (value) => done({ ok: true, value }),
      (error) => done({ ok: false, error: errorText(error) })
    );
  `);

  if (!wrapped || wrapped.ok !== true) {
    throw new Error((wrapped && wrapped.error) || "browser harness failed");
  }
  return wrapped.value;
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
  if (REQUIRE_PROTOCOL && result.protocol !== PROTOCOL) {
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

async function readDiagnostic(endpoint, sessionId, functionName, certificateHash, extraOptions = {}) {
  const wrapped = await executeAsyncScript(endpoint, sessionId, `
    const done = arguments[arguments.length - 1];
    const functionName = arguments[0];
    const options = arguments[1];
    function errorText(error) {
      if (error && typeof error === "object") {
        return (error.name ? error.name + ": " : "") + (error.message || String(error));
      }
      return String(error);
    }
    Promise.resolve(
      window.picoquicWebTransportBaton[functionName](options)
    ).then(
      (value) => done({ ok: true, value }),
      (error) => done({ ok: false, error: errorText(error) })
    );
  `, [functionName, {
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL,
    ...extraOptions
  }]);

  if (!wrapped || wrapped.ok !== true) {
    throw new Error((wrapped && wrapped.error) ||
      `browser ${functionName} diagnostic failed`);
  }
  return wrapped.value;
}

function assertDiagnosticResult(name, result) {
  if (!result || result.ok !== true) {
    throw new Error(`browser ${name} diagnostic failed: ${JSON.stringify(result)}`);
  }
}

function summarizeCapabilities(capabilities) {
  if (!capabilities || typeof capabilities !== "object") {
    return {};
  }
  return {
    browserName: capabilities.browserName || "",
    browserVersion: capabilities.browserVersion || "",
    platformName: capabilities.platformName || "",
    geckoDriverVersion: capabilities["moz:geckodriverVersion"] || ""
  };
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
    closeSessionReceivedError42: countMatches(output,
      /Received web transport session capsule, type: 0x[0-9a-f]+ \(close session\), error: 2a /g),
    closeSessionSent: countMatches(output,
      /Sent WebTransport close session on stream/g),
    closeSessionSentError42: countMatches(output,
      /Sent WebTransport close session on stream: [0-9]+, error: 42/g),
    drainSessionSent: countMatches(output,
      /Sent WebTransport drain session on stream/g),
    controlFinNoCapsuleSent: countMatches(output,
      /Sent WebTransport control FIN without close capsule on stream/g),
    lifecycleTriggerReceived: countMatches(output,
      /Received WebTransport lifecycle trigger on stream/g),
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
    resetStreamSent: countMatches(output,
      /Sent WebTransport RESET_STREAM on stream/g),
    stopSendingSent: countMatches(output,
      /Sent WebTransport STOP_SENDING on stream/g),
    resetStreamAppError123: countMatches(output,
      /Received WebTransport RESET_STREAM on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    stopSendingAppError123: countMatches(output,
      /Received WebTransport STOP_SENDING on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    resetStreamSentAppError123: countMatches(output,
      /Sent WebTransport RESET_STREAM on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    stopSendingSentAppError123: countMatches(output,
      /Sent WebTransport STOP_SENDING on stream: [0-9]+, h3_error: [0-9]+, app_error: 123/g),
    writableBadChunkCloseReceived:
      output.includes("error: 0 (writable-bad-chunk-test)"),
    browserCloseReceived:
      /error: 2a \(browser-close-test\)/.test(output)
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
      stopSendingAppError123: 0,
      resetStreamSent: 0,
      resetStreamSentAppError123: 0,
      stopSendingSent: 0,
      stopSendingSentAppError123: 0
    };
  }
  if (STREAM_MODE === "browser-cancel-incoming-bidi" ||
    STREAM_MODE === "browser-cancel-incoming-uni") {
    return {
      resetStreamReceived: 0,
      resetStreamAppError123: 0,
      stopSendingReceived: 1,
      stopSendingAppError123: 1,
      resetStreamSent: 0,
      resetStreamSentAppError123: 0,
      stopSendingSent: 0,
      stopSendingSentAppError123: 0
    };
  }
  if (STREAM_MODE === "server-reset-bidi" ||
    STREAM_MODE === "server-reset-uni") {
    return {
      resetStreamReceived: 0,
      resetStreamAppError123: 0,
      stopSendingReceived: 0,
      stopSendingAppError123: 0,
      resetStreamSent: 1,
      resetStreamSentAppError123: 1,
      stopSendingSent: 0,
      stopSendingSentAppError123: 0
    };
  }
  if (STREAM_MODE === "server-stop-bidi" ||
    STREAM_MODE === "server-stop-uni") {
    return {
      resetStreamReceived: 0,
      resetStreamAppError123: 0,
      stopSendingReceived: 0,
      stopSendingAppError123: 0,
      resetStreamSent: 0,
      resetStreamSentAppError123: 0,
      stopSendingSent: 1,
      stopSendingSentAppError123: 1
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
  for (const [name, value] of Object.entries(expected)) {
    if (summary[name] < value) {
      return false;
    }
  }
  return true;
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
    line.includes("Sent WebTransport close session on stream") ||
    line.includes("Sent WebTransport drain session on stream") ||
    line.includes("Sent WebTransport control FIN without close capsule") ||
    line.includes("Received WebTransport lifecycle trigger on stream") ||
    line.includes("Received empty WebTransport datagram on stream") ||
    line.includes("Received baton WebTransport datagram on stream") ||
    line.includes("Received sized WebTransport datagram on stream") ||
    line.includes("All ZERO baton on stream") ||
    line.includes("WebTransport stream test ") ||
    line.includes("Received WebTransport RESET_STREAM") ||
    line.includes("Received WebTransport STOP_SENDING") ||
    line.includes("Sent WebTransport RESET_STREAM") ||
    line.includes("Sent WebTransport STOP_SENDING") ||
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

  const geckoDriver = findGeckoDriver();
  if (!geckoDriver) {
    throw new Error("No geckodriver binary found. Set GECKO_DRIVER_BIN to run this test.");
  }

  const workDir = mkdtempSync(join(tmpdir(), "picoquic-wt-firefox-"));
  const certConfig = await getCertificateConfig(workDir);
  const harness = PAGE_URL ? null : await startHarnessServer();
  const targetUrl = buildPageUrl(PAGE_URL || harness.url, certConfig.hash);
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

  const driverEnv = {
    ...process.env,
    MOZ_HEADLESS: FIREFOX_HEADLESS ? "1" : (process.env.MOZ_HEADLESS || "")
  };
  const driver = spawn(geckoDriver, ["--port", String(GECKO_DRIVER_PORT)], {
    env: driverEnv,
    stdio: ["ignore", "pipe", "pipe"]
  });
  let driverOutput = "";
  function appendDriverOutput(data) {
    driverOutput = (driverOutput + data.toString()).slice(-8192);
  }
  driver.stdout.on("data", appendDriverOutput);
  driver.stderr.on("data", appendDriverOutput);

  const endpoint = `http://127.0.0.1:${GECKO_DRIVER_PORT}`;
  let sessionId = "";
  let browserCapabilities = {};
  try {
    await waitForServer(server);
    await waitForDriver(endpoint, () => driverOutput.trim());
    const session = await newFirefoxSession(endpoint);
    sessionId = session.sessionId;
    browserCapabilities = summarizeCapabilities(session.capabilities);
    await webdriver(endpoint, "POST", `/session/${sessionId}/timeouts`, {
      script: TIMEOUT_MS + 5000,
      pageLoad: TIMEOUT_MS + 5000
    });
    await webdriver(endpoint, "POST", `/session/${sessionId}/url`, {
      url: targetUrl
    });
    await waitForHarness(endpoint, sessionId);
    const result = await readHarnessResult(endpoint, sessionId);
    result.browser = browserCapabilities;
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
      result.protocolConstructor = await readDiagnostic(endpoint, sessionId,
        "runProtocolConstructorTests", certConfig.hash);
      if (REQUIRE_PROTOCOL_CONSTRUCTOR) {
        assertDiagnosticResult("protocolConstructor", result.protocolConstructor);
      }
      result.urlConstructor = await readDiagnostic(endpoint, sessionId,
        "runUrlConstructorTests", certConfig.hash);
      assertDiagnosticResult("urlConstructor", result.urlConstructor);
      result.optionsConstructor = await readDiagnostic(endpoint, sessionId,
        "runOptionsConstructorTests", certConfig.hash);
      if (REQUIRE_OPTIONS_CONSTRUCTOR) {
        assertDiagnosticResult("optionsConstructor", result.optionsConstructor);
      }
      const writableBadChunk = await readDiagnostic(endpoint, sessionId,
        "runWritableBadChunkTests", certConfig.hash, {
          requireDatagram: REQUIRE_DATAGRAM
        });
      result.datagramWritable = writableBadChunk.datagramWritable;
      assertDiagnosticResult("datagramWritable", result.datagramWritable);
      result.streamWritable = writableBadChunk.streamWritable;
      if (REQUIRE_STREAM_WRITABLE) {
        assertDiagnosticResult("streamWritable", result.streamWritable);
      }
      result.closeSession = await readDiagnostic(endpoint, sessionId,
        "runCloseSessionTests", certConfig.hash, {
          requireDatagram: REQUIRE_DATAGRAM
        });
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
    const driverText = driverOutput.trim();
    const context = `page url: ${targetUrl}\nwt url: ${WT_URL}`;
    const details = [
      error.message,
      `context:\n${context}`,
      Object.keys(browserCapabilities).length > 0 ?
        `browser capabilities:\n${JSON.stringify(browserCapabilities, null, 2)}` : "",
      output ? `server output:\n${output}` : "",
      driverText ? `geckodriver output:\n${driverText}` : ""
    ].filter(Boolean).join("\n");
    throw new Error(details);
  } finally {
    if (sessionId) {
      try {
        await webdriver(endpoint, "DELETE", `/session/${sessionId}`);
      } catch (_) {}
    }
    await terminateProcess(driver);
    await terminateProcess(server);
    if (harness) {
      harness.close();
    }
    await removeTree(workDir);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
