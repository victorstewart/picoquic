#!/usr/bin/env node

import { spawn } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(new URL("../../../..", import.meta.url).pathname);
const DEFAULT_MANIFEST = join(ROOT, "tests", "webtransport", "e2e", "manifests", "core.json");
const DEFAULT_EXPECTED_DIR = join(ROOT, "tests", "webtransport", "e2e", "expected");
const DEFAULT_PORT = Number(process.env.PICOQUIC_WT_PORT || 4433);
const BROWSER_RUNNERS = {
  chrome: join(ROOT, "tests", "webtransport", "browser", "run-chrome.mjs"),
  safari: join(ROOT, "tests", "webtransport", "browser", "run-safari.mjs")
};
const SCENARIO_ID_RE = /^[a-z0-9]+(?:[._-][a-z0-9]+)*$/;
const SCENARIO_FIELDS = new Set([
  "id",
  "title",
  "runner",
  "wtUrl",
  "protocol",
  "requireDatagram",
  "useByob",
  "timeoutMs",
  "coverage",
  "expect"
]);

function isObject(value) {
  return value && typeof value === "object" && !Array.isArray(value);
}

function requireString(value, name) {
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`invalid manifest field ${name}: expected non-empty string`);
  }
}

function requireBoolean(value, name) {
  if (typeof value !== "boolean") {
    throw new Error(`invalid manifest field ${name}: expected boolean`);
  }
}

function requirePositiveInteger(value, name) {
  if (!Number.isInteger(value) || value <= 0) {
    throw new Error(`invalid manifest field ${name}: expected positive integer`);
  }
}

function rejectUnknownFields(object, allowed, name) {
  for (const field of Object.keys(object)) {
    if (!allowed.has(field)) {
      throw new Error(`invalid manifest field ${name}.${field}: unknown field`);
    }
  }
}

function validateScenario(scenario, path) {
  if (!isObject(scenario)) {
    throw new Error(`invalid scenario in ${path}: expected object`);
  }
  rejectUnknownFields(scenario, SCENARIO_FIELDS, "scenario");
  requireString(scenario.id, "scenario.id");
  if (!SCENARIO_ID_RE.test(scenario.id)) {
    throw new Error(`invalid manifest field scenario.id: ${scenario.id}`);
  }
  requireString(scenario.title, `${scenario.id}.title`);
  requireString(scenario.runner, `${scenario.id}.runner`);
  if (scenario.runner !== "browser-baton") {
    throw new Error(`invalid manifest field ${scenario.id}.runner: unsupported runner ${scenario.runner}`);
  }
  requireString(scenario.wtUrl, `${scenario.id}.wtUrl`);
  if (Object.prototype.hasOwnProperty.call(scenario, "protocol")) {
    requireString(scenario.protocol, `${scenario.id}.protocol`);
  }
  for (const field of ["requireDatagram", "useByob"]) {
    if (Object.prototype.hasOwnProperty.call(scenario, field)) {
      requireBoolean(scenario[field], `${scenario.id}.${field}`);
    }
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "timeoutMs")) {
    requirePositiveInteger(scenario.timeoutMs, `${scenario.id}.timeoutMs`);
  }
  if (!Array.isArray(scenario.coverage) || scenario.coverage.length === 0) {
    throw new Error(`invalid manifest field ${scenario.id}.coverage: expected non-empty string array`);
  }
  for (const [index, tag] of scenario.coverage.entries()) {
    requireString(tag, `${scenario.id}.coverage[${index}]`);
  }
  if (!isObject(scenario.expect)) {
    throw new Error(`invalid manifest field ${scenario.id}.expect: expected object`);
  }
  requireBoolean(scenario.expect.ok, `${scenario.id}.expect.ok`);
}

function usage() {
  console.error([
    "usage:",
    "  node tests/webtransport/e2e/runners/run-browser.mjs list [--manifest <path>] [--json]",
    "  node tests/webtransport/e2e/runners/run-browser.mjs --browser <chrome|safari> [--manifest <path>] [--expected <path>] [--no-expected] [--scenario <id>] [--json]"
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

function loadManifest(path) {
  if (!existsSync(path)) {
    throw new Error(`manifest not found: ${path}`);
  }
  const manifest = JSON.parse(readFileSync(path, "utf8"));
  if (!manifest || !Array.isArray(manifest.scenarios)) {
    throw new Error(`manifest has no scenarios array: ${path}`);
  }
  const scenarioIds = new Set();
  for (const scenario of manifest.scenarios) {
    validateScenario(scenario, path);
    if (scenarioIds.has(scenario.id)) {
      throw new Error(`duplicate scenario id in ${path}: ${scenario.id}`);
    }
    scenarioIds.add(scenario.id);
  }
  return manifest;
}

function defaultExpectedPath(browser) {
  return join(DEFAULT_EXPECTED_DIR, `${browser}-stable.json`);
}

function manifestScenarioIds(manifest) {
  return new Set(manifest.scenarios.map((scenario) => scenario.id));
}

function loadExpected(path, manifest) {
  if (!path || !existsSync(path)) {
    return { path: "", entries: new Map() };
  }

  const expected = JSON.parse(readFileSync(path, "utf8"));
  if (!expected || !Array.isArray(expected.expected)) {
    throw new Error(`expected-results file has no expected array: ${path}`);
  }
  const entries = new Map();
  const scenarioIds = manifestScenarioIds(manifest);
  for (const entry of expected.expected || []) {
    if (!entry.scenario || !entry.status || !entry.category ||
      !entry.reason || !entry.evidence) {
      throw new Error(`invalid expected-result entry in ${path}`);
    }
    if (!scenarioIds.has(entry.scenario)) {
      throw new Error(`expected-result entry references unknown scenario in ${path}: ${entry.scenario}`);
    }
    if (entries.has(entry.scenario)) {
      throw new Error(`duplicate expected-result scenario in ${path}: ${entry.scenario}`);
    }
    entries.set(entry.scenario, entry);
  }
  return { path, entries };
}

function renderTemplate(value, vars) {
  if (typeof value === "string") {
    return value.replace(/\{port\}/g, String(vars.port));
  }
  if (Array.isArray(value)) {
    return value.map((entry) => renderTemplate(entry, vars));
  }
  if (value && typeof value === "object") {
    const rendered = {};
    for (const [key, entry] of Object.entries(value)) {
      rendered[key] = renderTemplate(entry, vars);
    }
    return rendered;
  }
  return value;
}

function selectedScenarios(manifest, scenarioId) {
  if (!scenarioId) {
    return manifest.scenarios;
  }
  const scenarios = manifest.scenarios.filter((scenario) => scenario.id === scenarioId);
  if (scenarios.length === 0) {
    throw new Error(`scenario not found: ${scenarioId}`);
  }
  return scenarios;
}

function firstBrowserInfo(results, browser) {
  for (const result of results) {
    if (result.result && result.result.browser) {
      return result.result.browser;
    }
    if (result.expected && (result.expected.browserVersion || result.expected.platform)) {
      return {
        browserName: browser,
        browserVersion: result.expected.browserVersion || "",
        platformName: result.expected.platform || ""
      };
    }
  }
  return {};
}

function parseJsonOutput(output) {
  const trimmed = output.trim();
  for (let index = 0; index < trimmed.length; index++) {
    if (trimmed[index] !== "{") {
      continue;
    }
    try {
      return JSON.parse(trimmed.slice(index));
    } catch (_) {}
  }
  throw new Error(`runner did not emit JSON: ${trimmed.slice(-2048)}`);
}

function assertArrayEquals(id, name, actual, expected) {
  if (!Array.isArray(actual) || actual.length !== expected.length ||
    actual.some((value, index) => value !== expected[index])) {
    throw new Error(`${id}: expected ${name} ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function assertScenarioResult(id, result, expect) {
  if (result.ok !== expect.ok) {
    throw new Error(`${id}: expected ok=${expect.ok}, got ${result.ok}`);
  }
  for (const name of ["url", "protocol", "requireDatagram", "constructorRequireUnreliable", "useByob", "datagramsSent"]) {
    if (Object.prototype.hasOwnProperty.call(expect, name) && result[name] !== expect[name]) {
      throw new Error(`${id}: expected ${name}=${JSON.stringify(expect[name])}, got ${JSON.stringify(result[name])}`);
    }
  }
  if (expect.received) {
    assertArrayEquals(id, "received", result.received, expect.received);
  }
  if (expect.sent) {
    assertArrayEquals(id, "sent", result.sent, expect.sent);
  }
  if (expect.datagramsReceivedMin !== undefined &&
    (!Array.isArray(result.datagramsReceived) ||
      result.datagramsReceived.length < expect.datagramsReceivedMin)) {
    throw new Error(`${id}: expected at least ${expect.datagramsReceivedMin} datagrams, got ${JSON.stringify(result.datagramsReceived)}`);
  }
  if (expect.errorIncludes !== undefined &&
    (!result.error || !result.error.includes(expect.errorIncludes))) {
    throw new Error(`${id}: expected error containing ${JSON.stringify(expect.errorIncludes)}, got ${JSON.stringify(result.error)}`);
  }
  if (expect.protocolConstructorOk !== undefined &&
    (!result.protocolConstructor ||
      result.protocolConstructor.ok !== expect.protocolConstructorOk)) {
    throw new Error(`${id}: protocol constructor check failed`);
  }
  if (expect.urlConstructorOk !== undefined &&
    (!result.urlConstructor ||
      result.urlConstructor.ok !== expect.urlConstructorOk)) {
    throw new Error(`${id}: URL constructor check failed`);
  }
  if (expect.server) {
    if (!result.server) {
      throw new Error(`${id}: missing server summary`);
    }
    for (const [name, minimum] of Object.entries(expect.server)) {
      if (name.endsWith("Min")) {
        const actualName = name.slice(0, -3);
        const actual = result.server[actualName];
        if (typeof actual !== "number" || actual < minimum) {
          throw new Error(`${id}: expected server.${actualName} >= ${minimum}, got ${actual}`);
        }
      } else if (result.server[name] !== minimum) {
        throw new Error(`${id}: expected server.${name}=${JSON.stringify(minimum)}, got ${JSON.stringify(result.server[name])}`);
      }
    }
  }
}

function runChild(command, args, env) {
  const child = spawn(command, args, {
    cwd: ROOT,
    env,
    stdio: ["ignore", "pipe", "pipe"]
  });
  let stdout = "";
  let stderr = "";
  child.stdout.on("data", (data) => {
    stdout += data.toString();
  });
  child.stderr.on("data", (data) => {
    stderr += data.toString();
  });

  return new Promise((resolveRun, rejectRun) => {
    child.once("exit", (code, signal) => {
      if (code === 0) {
        resolveRun({ stdout, stderr });
      } else {
        rejectRun(new Error(`${args[0]} failed: code=${code} signal=${signal}\n${stdout}\n${stderr}`));
      }
    });
  });
}

async function runScenario(browser, scenario, vars) {
  const expectedEntry = vars.expected.entries.get(scenario.id);
  if (expectedEntry && expectedEntry.status === "skip") {
    return {
      id: scenario.id,
      title: scenario.title || "",
      status: "skip",
      coverage: scenario.coverage || [],
      expected: {
        category: expectedEntry.category,
        browserVersion: expectedEntry.browserVersion || "",
        platform: expectedEntry.platform || "",
        reason: expectedEntry.reason,
        evidence: expectedEntry.evidence
      }
    };
  }
  if (expectedEntry) {
    throw new Error(`${scenario.id}: unsupported expected status ${expectedEntry.status}`);
  }

  const runner = BROWSER_RUNNERS[browser];
  if (!runner) {
    throw new Error(`unsupported browser: ${browser}`);
  }
  const rendered = renderTemplate(scenario, vars);
  const env = {
    ...process.env,
    PICOQUIC_WT_URL: rendered.wtUrl,
    PICOQUIC_WT_PROTOCOL: rendered.protocol || "devious-baton-00",
    PICOQUIC_WT_REQUIRE_DATAGRAM: rendered.requireDatagram === false ? "0" : "1",
    PICOQUIC_WT_USE_BYOB: rendered.useByob === false ? "0" : "1",
    PICOQUIC_WT_EXPECT_OK: rendered.expect && rendered.expect.ok === false ? "0" : "1",
    PICOQUIC_WT_PROTOCOL_CONSTRUCTOR: rendered.expect && rendered.expect.ok === false ? "0" : "1",
    PICOQUIC_WT_INCLUDE_SERVER_SUMMARY: "1",
    PICOQUIC_WT_TIMEOUT_MS: String(rendered.timeoutMs || 30000),
    PICOQUIC_WT_PORT: String(vars.port)
  };
  const run = await runChild(process.execPath, [runner], env);
  const result = parseJsonOutput(run.stdout);
  assertScenarioResult(rendered.id, result, rendered.expect || {});
  return {
    id: rendered.id,
    title: rendered.title || "",
    status: "pass",
    coverage: rendered.coverage || [],
    result
  };
}

function commandList(args) {
  const manifestPath = resolve(takeOption(args, "--manifest", DEFAULT_MANIFEST));
  const json = hasOption(args, "--json");
  if (args.length !== 0) {
    throw new Error(`unexpected list arguments: ${args.join(" ")}`);
  }
  const manifest = loadManifest(manifestPath);
  const result = {
    suite: manifest.suite || "",
    description: manifest.description || "",
    scenarios: manifest.scenarios.map((scenario) => ({
      id: scenario.id,
      title: scenario.title || "",
      runner: scenario.runner,
      coverage: scenario.coverage || []
    }))
  };
  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`# suite: ${result.suite}`);
    for (const scenario of result.scenarios) {
      console.log(`${scenario.id}\t${scenario.runner}\t${scenario.title}`);
    }
  }
}

async function commandRun(args) {
  const browser = takeOption(args, "--browser", process.env.PICOQUIC_WT_BROWSER || "");
  const manifestPath = resolve(takeOption(args, "--manifest", DEFAULT_MANIFEST));
  const scenarioId = takeOption(args, "--scenario", "");
  const noExpected = hasOption(args, "--no-expected");
  const expectedPath = noExpected ? "" : resolve(takeOption(args, "--expected",
    browser ? defaultExpectedPath(browser) : ""));
  const json = hasOption(args, "--json");
  if (!browser) {
    throw new Error("missing --browser");
  }
  if (args.length !== 0) {
    throw new Error(`unexpected run arguments: ${args.join(" ")}`);
  }

  const manifest = loadManifest(manifestPath);
  const expected = loadExpected(expectedPath, manifest);
  const vars = { port: DEFAULT_PORT, expected };
  const scenarios = selectedScenarios(manifest, scenarioId);
  const results = [];
  for (const scenario of scenarios) {
    results.push(await runScenario(browser, scenario, vars));
  }
  const summary = {
    ok: true,
    browser,
    browserInfo: firstBrowserInfo(results, browser),
    suite: manifest.suite || "",
    manifest: manifestPath,
    expected: expected.path,
    scenarios: results
  };
  if (json) {
    console.log(JSON.stringify(summary, null, 2));
  } else {
    console.log(JSON.stringify(summary, null, 2));
  }
}

async function main() {
  const args = process.argv.slice(2);
  if (args[0] === "list") {
    args.shift();
    commandList(args);
  } else if (args.length === 0 || args.includes("--help")) {
    usage();
    process.exit(args.includes("--help") ? 0 : 1);
  } else {
    await commandRun(args);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
