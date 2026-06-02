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
const PROTOCOL = process.env.PICOQUIC_WT_PROTOCOL || "devious-baton-00";
const REQUIRE_DATAGRAM = process.env.PICOQUIC_WT_REQUIRE_DATAGRAM !== "0";
const USE_BYOB = process.env.PICOQUIC_WT_USE_BYOB !== "0";
const EXPECT_OK = process.env.PICOQUIC_WT_EXPECT_OK !== "0";
const RUN_PROTOCOL_CONSTRUCTOR = process.env.PICOQUIC_WT_PROTOCOL_CONSTRUCTOR !== "0";
const INCLUDE_SERVER_SUMMARY = process.env.PICOQUIC_WT_INCLUDE_SERVER_SUMMARY === "1";
const SERVER_OUTPUT_LIMIT = INCLUDE_SERVER_SUMMARY ? 262144 : 32768;
const TIMEOUT_MS = Number(process.env.PICOQUIC_WT_TIMEOUT_MS || 30000);
const SAFARI_DRIVER_PORT = Number(process.env.PICOQUIC_WT_SAFARI_DRIVER_PORT || 9444);
const HARNESS_PORT = Number(process.env.PICOQUIC_WT_HARNESS_PORT || 8080);
const WT_URL = process.env.PICOQUIC_WT_URL ||
  `https://localhost:${PORT}/baton?version=0&baton=251&count=1`;
const PAGE_URL = process.env.PICOQUIC_WT_PAGE_URL || "";

const safariDriverNames = [
  process.env.SAFARI_DRIVER_BIN,
  "safaridriver",
  "/usr/bin/safaridriver",
  "/System/Cryptexes/App/usr/bin/safaridriver"
].filter(Boolean);

function sleep(ms) {
  return new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
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

function findSafariDriver() {
  for (const name of safariDriverNames) {
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

  return { cert, key, hash: certHash(cert) };
}

function certHash(certPath) {
  const cert = new X509Certificate(readFileSync(certPath));
  return createHash("sha256").update(cert.raw).digest("base64url");
}

function buildPageUrl(pageUrl, certificateHash) {
  const url = new URL(pageUrl);
  url.searchParams.set("autorun", "1");
  url.searchParams.set("timeoutMs", String(TIMEOUT_MS));
  url.searchParams.set("url", WT_URL);
  url.searchParams.set("protocol", PROTOCOL);
  url.searchParams.set("certHash", certificateHash);
  if (!REQUIRE_DATAGRAM) {
    url.searchParams.set("requireDatagram", "0");
  }
  if (!USE_BYOB) {
    url.searchParams.set("useByob", "0");
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
  throw new Error(`safaridriver did not become ready: ${childOutput()}`);
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

async function newSafariSession(endpoint) {
  try {
    const value = await webdriver(endpoint, "POST", "/session", {
      capabilities: {
        alwaysMatch: {
          browserName: "safari"
        }
      }
    });
    if (!value || !value.sessionId) {
      throw new Error(`Safari session response did not include sessionId: ${JSON.stringify(value)}`);
    }
    return value;
  } catch (error) {
    if (/Allow remote automation|remote automation/i.test(error.message)) {
      throw new Error(
        `${error.message}\nEnable Safari Settings > Developer > Allow Remote Automation, ` +
        "or run `sudo safaridriver --enable`, then rerun this command.");
    }
    throw error;
  }
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
  if (!equalArray(result.received, [251, 253, 255])) {
    throw new Error(`unexpected received baton sequence: ${JSON.stringify(result.received)}`);
  }
  if (!equalArray(result.sent, [252, 254, 0])) {
    throw new Error(`unexpected sent baton sequence: ${JSON.stringify(result.sent)}`);
  }
  if (REQUIRE_DATAGRAM && (!Array.isArray(result.datagramsReceived) ||
    result.datagramsReceived.length === 0)) {
    throw new Error("no WebTransport datagram received");
  }
}

async function readProtocolConstructorResult(endpoint, sessionId, certificateHash) {
  const wrapped = await executeAsyncScript(endpoint, sessionId, `
    const done = arguments[arguments.length - 1];
    const options = arguments[0];
    function errorText(error) {
      if (error && typeof error === "object") {
        return (error.name ? error.name + ": " : "") + (error.message || String(error));
      }
      return String(error);
    }
    Promise.resolve(
      window.picoquicWebTransportBaton.runProtocolConstructorTests(options)
    ).then(
      (value) => done({ ok: true, value }),
      (error) => done({ ok: false, error: errorText(error) })
    );
  `, [{
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL
  }]);

  if (!wrapped || wrapped.ok !== true) {
    throw new Error((wrapped && wrapped.error) ||
      "browser protocol constructor tests failed");
  }
  return wrapped.value;
}

async function readUrlConstructorResult(endpoint, sessionId, certificateHash) {
  const wrapped = await executeAsyncScript(endpoint, sessionId, `
    const done = arguments[arguments.length - 1];
    const options = arguments[0];
    function errorText(error) {
      if (error && typeof error === "object") {
        return (error.name ? error.name + ": " : "") + (error.message || String(error));
      }
      return String(error);
    }
    Promise.resolve(
      window.picoquicWebTransportBaton.runUrlConstructorTests(options)
    ).then(
      (value) => done({ ok: true, value }),
      (error) => done({ ok: false, error: errorText(error) })
    );
  `, [{
    url: WT_URL,
    certificateHash,
    protocol: PROTOCOL
  }]);

  if (!wrapped || wrapped.ok !== true) {
    throw new Error((wrapped && wrapped.error) ||
      "browser URL constructor tests failed");
  }
  return wrapped.value;
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

function summarizeCapabilities(capabilities) {
  if (!capabilities || typeof capabilities !== "object") {
    return {};
  }
  return {
    browserName: capabilities.browserName || "",
    browserVersion: capabilities.browserVersion || "",
    platformName: capabilities.platformName || "",
    safariUseSimulator: capabilities.safari_useSimulator || capabilities["safari:useSimulator"] || false
  };
}

function countMatches(text, pattern) {
  const matches = text.match(pattern);
  return matches ? matches.length : 0;
}

function summarizeServerOutput(output) {
  return {
    bytesCaptured: output.length,
    waitingForPackets: output.includes("Waiting for packets"),
    connectAccepted: countMatches(output, /Connect accepted on stream/g),
    packetsReceived: countMatches(output, /Receiving packet type/g),
    packetsSent: countMatches(output, /Sending packet type/g),
    h3ControlFrames: countMatches(output, /H3 control frame/g)
  };
}

async function main() {
  assertFile(BATON, "pico_baton");

  const safariDriver = findSafariDriver();
  if (!safariDriver) {
    throw new Error("No safaridriver binary found. Set SAFARI_DRIVER_BIN to run this test.");
  }

  const workDir = mkdtempSync(join(tmpdir(), "picoquic-wt-safari-"));
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
  let serverOutput = "";
  function appendServerOutput(data) {
    serverOutput = (serverOutput + data.toString()).slice(-SERVER_OUTPUT_LIMIT);
  }
  server.stdout.on("data", appendServerOutput);
  server.stderr.on("data", appendServerOutput);

  const driver = spawn(safariDriver, ["-p", String(SAFARI_DRIVER_PORT)], {
    stdio: ["ignore", "pipe", "pipe"]
  });
  let driverOutput = "";
  function appendDriverOutput(data) {
    driverOutput = (driverOutput + data.toString()).slice(-8192);
  }
  driver.stdout.on("data", appendDriverOutput);
  driver.stderr.on("data", appendDriverOutput);

  const endpoint = `http://127.0.0.1:${SAFARI_DRIVER_PORT}`;
  let sessionId = "";
  let browserCapabilities = {};
  try {
    await waitForServer(server);
    await waitForDriver(endpoint, () => driverOutput.trim());
    const session = await newSafariSession(endpoint);
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
    if (INCLUDE_SERVER_SUMMARY) {
      result.server = summarizeServerOutput(serverOutput);
    }
    assertHarnessResult(result);
    if (RUN_PROTOCOL_CONSTRUCTOR && EXPECT_OK) {
      result.protocolConstructor = await readProtocolConstructorResult(endpoint,
        sessionId, certConfig.hash);
      assertProtocolConstructorResult(result.protocolConstructor);
      result.urlConstructor = await readUrlConstructorResult(endpoint,
        sessionId, certConfig.hash);
      assertUrlConstructorResult(result.urlConstructor);
    }
    console.log(JSON.stringify(result, null, 2));
  } catch (error) {
    const output = serverOutput.trim();
    const driverText = driverOutput.trim();
    const context = `page url: ${targetUrl}\nwt url: ${WT_URL}`;
    const details = [
      error.message,
      `context:\n${context}`,
      Object.keys(browserCapabilities).length > 0 ?
        `browser capabilities:\n${JSON.stringify(browserCapabilities, null, 2)}` : "",
      output ? `server output:\n${output}` : "",
      driverText ? `safaridriver output:\n${driverText}` : ""
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
