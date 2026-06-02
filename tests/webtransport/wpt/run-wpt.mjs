#!/usr/bin/env node

import { spawn } from "node:child_process";
import { createHash, X509Certificate } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(new URL("../../..", import.meta.url).pathname);
const DEFAULT_BATON = join(ROOT, "build", "pico_baton");
const DEFAULT_WEB_ROOT = join(ROOT, "tests", "webtransport", "browser");
const DEFAULT_PORT = 4433;
const DEFAULT_PATH = "/baton";

const EXPECTED_WPT_TESTS = [
  "constructor.https.sub.any.js",
  "connect.https.any.js",
  "close.https.any.js",
  "datagrams.https.any.js",
  "datagram-bad-chunk.https.any.js",
  "sendstream-bad-chunk.https.any.js",
  "streams-echo.https.any.js",
  "streams-close.https.any.js",
  "echo-large-bidirectional-streams.https.any.js",
  "server-certificate-hashes.https.any.js",
  "stats.https.any.js",
  "idlharness.https.sub.any.js",
  "csp-pass.https.window.js",
  "csp-fail.https.window.js",
  "back-forward-cache-*.js",
  "in-removed-iframe.https.html"
];

function usage() {
  console.error([
    "usage:",
    "  node tests/webtransport/wpt/run-wpt.mjs list [--wpt-root <path>] [--expected <path>] [--json]",
    "  node tests/webtransport/wpt/run-wpt.mjs run --wpt-root <path> --browser <name> [--test <path-or-pattern>] [--expected <path>] [--dry-run] [--wpt-arg <arg>...]",
    "  node tests/webtransport/wpt/run-wpt.mjs server-smoke [--baton-bin <path>] [--port <n>] [--web-root <path>]"
  ].join("\n"));
}

function takeOption(args, name, fallback = "") {
  const index = args.indexOf(name);
  if (index < 0) {
    return fallback;
  }
  if (index + 1 >= args.length) {
    throw new Error(`missing value for ${name}`);
  }
  const value = args[index + 1];
  args.splice(index, 2);
  return value;
}

function hasOption(args, name) {
  const index = args.indexOf(name);
  if (index < 0) {
    return false;
  }
  args.splice(index, 1);
  return true;
}

function takeOptions(args, name) {
  const values = [];
  for (let index = 0; index < args.length;) {
    if (args[index] !== name) {
      index++;
      continue;
    }
    if (index + 1 >= args.length) {
      throw new Error(`missing value for ${name}`);
    }
    values.push(args[index + 1]);
    args.splice(index, 2);
  }
  return values;
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

function runInteractive(command, args, cwd) {
  const child = spawn(command, args, {
    cwd,
    stdio: "inherit"
  });
  return new Promise((resolveRun, rejectRun) => {
    child.once("exit", (code, signal) => {
      if (code === 0) {
        resolveRun();
      } else {
        rejectRun(new Error(`${command} failed: code=${code} signal=${signal}`));
      }
    });
  });
}

function certHash(certPath) {
  const cert = new X509Certificate(readFileSync(certPath));
  return createHash("sha256").update(cert.raw).digest("base64url");
}

async function makeCertificate(workDir) {
  const certDir = join(workDir, "cert");
  const key = join(certDir, "key.pem");
  const cert = join(certDir, "cert.pem");

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

function waitForServer(child) {
  return new Promise((resolveReady, rejectReady) => {
    let settled = false;
    let output = "";
    const startupTimer = setTimeout(() => {
      finish(null);
    }, 750);
    const failTimer = setTimeout(() => {
      if (!settled) {
        finish(new Error(`pico_baton did not report readiness: ${output.trim()}`));
      }
    }, 5000);

    function finish(error) {
      if (!settled) {
        settled = true;
        clearTimeout(startupTimer);
        clearTimeout(failTimer);
        if (error) {
          rejectReady(error);
        } else {
          resolveReady(output);
        }
      }
    }

    function onData(data) {
      output = (output + data.toString()).slice(-8192);
      if (output.includes("Waiting for packets")) {
        finish(null);
      }
    }

    child.stdout.on("data", onData);
    child.stderr.on("data", onData);
    child.once("exit", (code, signal) => {
      finish(new Error(`pico_baton exited before readiness: code=${code} signal=${signal} ${output.trim()}`));
    });
  });
}

function listFiles(dir, prefix = "") {
  const files = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...listFiles(full, relative));
    } else {
      files.push(relative);
    }
  }
  return files;
}

function matchesExpected(file, expected) {
  if (expected.endsWith("*.js")) {
    return file.startsWith(expected.slice(0, -4)) && file.endsWith(".js");
  }
  return file === expected;
}

function discoverTests(wptRoot) {
  if (!wptRoot) {
    return { source: "builtin", tests: EXPECTED_WPT_TESTS };
  }

  const webtransportRoot = join(resolve(wptRoot), "webtransport");
  if (!existsSync(webtransportRoot)) {
    throw new Error(`WPT webtransport directory not found: ${webtransportRoot}`);
  }

  const discovered = listFiles(webtransportRoot)
    .filter((file) => EXPECTED_WPT_TESTS.some((expected) => matchesExpected(file, expected)))
    .sort();

  return { source: webtransportRoot, tests: discovered };
}

function resolveWptCommand(wptRoot) {
  const command = join(resolve(wptRoot), "wpt");
  if (!existsSync(command)) {
    throw new Error(`WPT command not found: ${command}`);
  }
  return command;
}

function expectedEntryMatchesTest(entryTest, discoveredTest) {
  if (entryTest.endsWith("*.js")) {
    return matchesExpected(discoveredTest, entryTest);
  }
  return entryTest === discoveredTest;
}

function selectTests(discoveredTests, selectors) {
  if (selectors.length === 0) {
    return discoveredTests;
  }

  const selected = [];
  for (const selector of selectors) {
    const matches = discoveredTests.filter((test) =>
      expectedEntryMatchesTest(selector, test) || matchesExpected(test, selector));
    if (matches.length === 0) {
      throw new Error(`selected WPT test does not match the target subset: ${selector}`);
    }
    for (const match of matches) {
      if (!selected.includes(match)) {
        selected.push(match);
      }
    }
  }
  return selected;
}

function loadExpectedManifest(expectedPath, discoveredTests) {
  const expected = JSON.parse(readFileSync(expectedPath, "utf8"));
  if (expected === null || typeof expected !== "object" || Array.isArray(expected)) {
    throw new Error(`expected manifest must be an object: ${expectedPath}`);
  }
  if (!Array.isArray(expected.expected)) {
    throw new Error(`expected manifest must contain an expected array: ${expectedPath}`);
  }

  for (const [index, entry] of expected.expected.entries()) {
    if (entry === null || typeof entry !== "object" || Array.isArray(entry)) {
      throw new Error(`expected[${index}] must be an object in ${expectedPath}`);
    }
    if (typeof entry.test !== "string" || entry.test.length === 0) {
      throw new Error(`expected[${index}].test must be a non-empty string in ${expectedPath}`);
    }
    if (typeof entry.status !== "string" || !["fail", "skip", "expected-fail"].includes(entry.status)) {
      throw new Error(`expected[${index}].status must be fail, skip, or expected-fail in ${expectedPath}`);
    }
    if (!discoveredTests.some((test) => expectedEntryMatchesTest(entry.test, test))) {
      throw new Error(`expected[${index}].test does not match the WPT subset: ${entry.test}`);
    }
  }

  return expected;
}

async function commandList(args) {
  const wptRoot = takeOption(args, "--wpt-root", process.env.PICOQUIC_WPT_ROOT || "");
  const expectedPath = takeOption(args, "--expected", "");
  const json = hasOption(args, "--json");
  if (args.length !== 0) {
    throw new Error(`unexpected list arguments: ${args.join(" ")}`);
  }

  const result = discoverTests(wptRoot);
  if (expectedPath) {
    result.expected = resolve(expectedPath);
    result.expectedManifest = loadExpectedManifest(result.expected, result.tests);
  }
  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`# source: ${result.source}`);
    if (result.expected) {
      console.log(`# expected: ${result.expected}`);
    }
    for (const test of result.tests) {
      console.log(test);
    }
  }
}

async function commandServerSmoke(args) {
  const baton = resolve(takeOption(args, "--baton-bin", process.env.PICO_BATON_BIN || DEFAULT_BATON));
  const webRoot = resolve(takeOption(args, "--web-root", process.env.PICOQUIC_WT_WEB_ROOT || DEFAULT_WEB_ROOT));
  const port = Number(takeOption(args, "--port", process.env.PICOQUIC_WT_PORT || String(DEFAULT_PORT)));
  if (args.length !== 0) {
    throw new Error(`unexpected server-smoke arguments: ${args.join(" ")}`);
  }
  if (!existsSync(baton)) {
    throw new Error(`pico_baton not found: ${baton}`);
  }
  if (!existsSync(webRoot)) {
    throw new Error(`web root not found: ${webRoot}`);
  }

  const workDir = mkdtempSync(join(tmpdir(), "picoquic-wpt-"));
  const cert = await makeCertificate(workDir);
  const server = spawn(baton, [
    "-p", String(port),
    "-c", cert.cert,
    "-k", cert.key,
    "-w", webRoot,
    DEFAULT_PATH
  ], { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });

  try {
    await waitForServer(server);
    console.log(JSON.stringify({
      ok: true,
      url: `https://localhost:${port}${DEFAULT_PATH}`,
      webRoot,
      certHash: cert.hash
    }, null, 2));
  } finally {
    server.kill("SIGTERM");
    rmSync(workDir, { recursive: true, force: true });
  }
}

async function commandRun(args) {
  const rawWptRoot = takeOption(args, "--wpt-root", process.env.PICOQUIC_WPT_ROOT || "");
  const browser = takeOption(args, "--browser", process.env.PICOQUIC_WPT_BROWSER || "");
  const expectedPath = takeOption(args, "--expected", "");
  const selectors = takeOptions(args, "--test");
  const wptArgs = takeOptions(args, "--wpt-arg");
  const dryRun = hasOption(args, "--dry-run");
  if (args.length !== 0) {
    throw new Error(`unexpected run arguments: ${args.join(" ")}`);
  }
  if (!rawWptRoot) {
    throw new Error("missing WPT checkout; pass --wpt-root or set PICOQUIC_WPT_ROOT");
  }
  const wptRoot = resolve(rawWptRoot);
  if (!existsSync(wptRoot)) {
    throw new Error(`WPT checkout not found: ${wptRoot}`);
  }
  if (!browser) {
    throw new Error("missing browser; pass --browser or set PICOQUIC_WPT_BROWSER");
  }

  const command = resolveWptCommand(wptRoot);
  const discovered = discoverTests(wptRoot);
  if (expectedPath) {
    loadExpectedManifest(resolve(expectedPath), discovered.tests);
  }
  const selected = selectTests(discovered.tests, selectors);
  const testArgs = selected.map((test) => `webtransport/${test}`);
  const runArgs = [
    "run",
    "--enable-webtransport-h3",
    ...wptArgs,
    browser,
    ...testArgs
  ];

  const result = {
    ok: true,
    dryRun,
    command,
    cwd: wptRoot,
    args: runArgs,
    tests: selected,
    server: "upstream-wpt-webtransport-h3"
  };
  if (expectedPath) {
    result.expected = resolve(expectedPath);
  }

  if (dryRun) {
    console.log(JSON.stringify(result, null, 2));
    return;
  }

  await runInteractive(command, runArgs, wptRoot);
  console.log(JSON.stringify(result, null, 2));
}

async function main() {
  const args = process.argv.slice(2);
  const command = args.shift();

  if (command === "list") {
    await commandList(args);
  } else if (command === "run") {
    await commandRun(args);
  } else if (command === "server-smoke") {
    await commandServerSmoke(args);
  } else {
    usage();
    process.exit(command ? 1 : 0);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
