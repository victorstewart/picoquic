#!/usr/bin/env node

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(new URL("../../..", import.meta.url).pathname);
const BATON = process.env.PICO_BATON_BIN || join(ROOT, "build", "pico_baton");
const CERT = process.env.PICOQUIC_WT_CERT || join(ROOT, "certs", "cert.pem");
const KEY = process.env.PICOQUIC_WT_KEY || join(ROOT, "certs", "key.pem");
const WEB_ROOT = process.env.PICOQUIC_WT_WEB_ROOT || join(ROOT, "tests", "webtransport", "browser");
const PORT = Number(process.env.PICOQUIC_WT_PORT || 4433);
const PROTOCOL = process.env.PICOQUIC_WT_PROTOCOL || "devious-baton-00";
const REQUIRE_DATAGRAM = process.env.PICOQUIC_WT_REQUIRE_DATAGRAM !== "0";
const TIMEOUT_MS = Number(process.env.PICOQUIC_WT_TIMEOUT_MS || 30000);
const CDP_PORT = Number(process.env.PICOQUIC_WT_CDP_PORT || 9223);
const TARGET_URL =
  `https://localhost:${PORT}/index.html?autorun=1&timeoutMs=${TIMEOUT_MS}` +
  `&protocol=${encodeURIComponent(PROTOCOL)}` +
  (REQUIRE_DATAGRAM ? "" : "&requireDatagram=0");

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
  const deadline = Date.now() + 10000;
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

async function newTarget(endpoint, url) {
  const targetUrl = `${endpoint}/json/new?${encodeURIComponent(url)}`;
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
  throw new Error("browser harness did not start");
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
  if (!result || result.ok !== true) {
    throw new Error(`browser harness failed: ${JSON.stringify(result)}`);
  }
  if (result.protocol !== PROTOCOL) {
    throw new Error(`unexpected protocol: ${result.protocol}`);
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

async function main() {
  assertFile(BATON, "pico_baton");
  assertFile(CERT, "certificate");
  assertFile(KEY, "private key");

  const chrome = findChrome();
  if (!chrome) {
    throw new Error("No Chrome/Chromium binary found. Set CHROME_BIN to run this test.");
  }

  const profile = mkdtempSync(join(tmpdir(), "picoquic-wt-chrome-"));
  const server = spawn(BATON, [
    "-p", String(PORT),
    "-c", CERT,
    "-k", KEY,
    "-w", WEB_ROOT,
    "/baton"
  ], { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });

  let chromeProcess = null;
  let cdp = null;
  try {
    await waitForServer(server);

    const chromeArgs = [
      "--headless=new",
      "--no-first-run",
      "--disable-background-networking",
      "--disable-dev-shm-usage",
      "--disable-gpu",
      "--enable-quic",
      "--ignore-certificate-errors",
      `--origin-to-force-quic-on=localhost:${PORT}`,
      `--remote-debugging-port=${CDP_PORT}`,
      `--user-data-dir=${profile}`,
      "about:blank"
    ];
    if (typeof process.getuid === "function" && process.getuid() === 0) {
      chromeArgs.splice(1, 0, "--no-sandbox");
    }
    chromeProcess = spawn(chrome, chromeArgs, { stdio: ["ignore", "ignore", "pipe"] });
    let chromeStderr = "";
    chromeProcess.stderr.on("data", (data) => {
      chromeStderr = (chromeStderr + data.toString()).slice(-4096);
    });

    let endpoint = "";
    try {
      endpoint = await waitForCdpEndpoint();
    } catch (error) {
      const stderr = chromeStderr.trim();
      throw new Error(stderr ? `${error.message}: ${stderr}` : error.message);
    }
    cdp = await connectCdp(await newTarget(endpoint, TARGET_URL));
    await cdp.send("Runtime.enable");
    await waitForHarness(cdp);
    const result = await readHarnessResult(cdp);
    assertHarnessResult(result);
    console.log(JSON.stringify(result, null, 2));
  } finally {
    if (cdp) {
      cdp.close();
    }
    if (chromeProcess) {
      chromeProcess.kill("SIGTERM");
    }
    server.kill("SIGTERM");
    rmSync(profile, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
