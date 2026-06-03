#!/usr/bin/env node

import { spawn } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(new URL("../../../..", import.meta.url).pathname);
const DEFAULT_MANIFEST = join(ROOT, "tests", "webtransport", "e2e", "manifests", "core.json");
const DEFAULT_EXPECTED_DIR = join(ROOT, "tests", "webtransport", "e2e", "expected");
const DEFAULT_PORT = Number(process.env.PICOQUIC_WT_PORT || 4433);
const CHILD_TIMEOUT_OVERRIDE_MS =
  Number(process.env.PICOQUIC_WT_E2E_CHILD_TIMEOUT_MS || 0);
const WRONG_CERT_HASH = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const BROWSER_RUNNERS = {
  chrome: join(ROOT, "tests", "webtransport", "browser", "run-chrome.mjs"),
  edge: join(ROOT, "tests", "webtransport", "browser", "run-chrome.mjs"),
  firefox: join(ROOT, "tests", "webtransport", "browser", "run-firefox.mjs"),
  safari: join(ROOT, "tests", "webtransport", "browser", "run-safari.mjs")
};
const SCENARIO_ID_RE = /^[a-z0-9]+(?:[._-][a-z0-9]+)*$/;
const EXPECTED_STATUSES = new Set(["skip", "pass"]);
const EXPECTED_RESULT_FIELDS = new Set(["browser", "channel", "platform", "expected"]);
const EXPECTED_ENTRY_FIELDS = new Set([
  "scenario",
  "status",
  "category",
  "browserVersion",
  "platform",
  "reason",
  "evidence",
  "expect"
]);
const EXPECT_OVERRIDE_FIELDS = new Set([
  "ok",
  "url",
  "protocol",
  "requireDatagram",
  "constructorRequireUnreliable",
  "useByob",
  "readyMs",
  "closedMs",
  "datagramsSent",
  "datagramReceiveMode",
  "datagramSendMode",
  "datagramSendSize",
  "datagramSendCount",
  "streamMode",
  "streamSize",
  "streamCount",
  "streamBytesSent",
  "streamBytesReceived",
  "streamFinSent",
  "streamFinReceived",
  "received",
  "sent",
  "sentVariants",
  "datagramsReceived",
  "datagramLengths",
  "datagramLengthsMin",
  "datagramLength",
  "eventsInclude",
  "eventsExclude",
  "datagramsReceivedMin",
  "errorIncludes",
  "protocolConstructorOk",
  "protocolConstructorTestsInclude",
  "urlConstructorOk",
  "urlConstructorTestsInclude",
  "optionsConstructorOk",
  "optionsConstructorTestsInclude",
  "datagramWritableOk",
  "datagramWritableTestsInclude",
  "streamWritableOk",
  "streamWritableTestsInclude",
  "postCloseOk",
  "postCloseTestsInclude",
  "postCloseDatagramOk",
  "postCloseDatagramTestsInclude",
  "closeSessionOk",
  "closeSessionTestsInclude",
  "sessionDiagnosticsOk",
  "sessionDiagnosticsTestsInclude",
  "server",
  "sequenceOrderMatters"
]);
const EXPECT_BOOLEAN_FIELDS = new Set([
  "ok",
  "requireDatagram",
  "constructorRequireUnreliable",
  "useByob",
  "protocolConstructorOk",
  "urlConstructorOk",
  "optionsConstructorOk",
  "datagramWritableOk",
  "streamWritableOk",
  "postCloseOk",
  "postCloseDatagramOk",
  "closeSessionOk",
  "sessionDiagnosticsOk",
  "sequenceOrderMatters"
]);
const EXPECT_STRING_FIELDS = new Set([
  "url",
  "protocol",
  "errorIncludes"
]);
const EXPECT_MODE_FIELDS = new Set([
  "datagramReceiveMode",
  "datagramSendMode"
]);
const EXPECT_COUNTER_FIELDS = new Set([
  "readyMs",
  "closedMs",
  "datagramsSent",
  "datagramSendSize",
  "datagramSendCount",
  "datagramLengthsMin",
  "datagramLength",
  "datagramsReceivedMin",
  "streamSize",
  "streamCount",
  "streamBytesSent",
  "streamBytesReceived",
  "streamFinSent",
  "streamFinReceived"
]);
const EXPECT_NUMBER_ARRAY_FIELDS = new Set([
  "received",
  "sent",
  "datagramsReceived",
  "datagramLengths"
]);
const EXPECT_STRING_ARRAY_FIELDS = new Set([
  "eventsInclude",
  "eventsExclude",
  "protocolConstructorTestsInclude",
  "urlConstructorTestsInclude",
  "optionsConstructorTestsInclude",
  "datagramWritableTestsInclude",
  "streamWritableTestsInclude",
  "postCloseTestsInclude",
  "postCloseDatagramTestsInclude",
  "closeSessionTestsInclude",
  "sessionDiagnosticsTestsInclude"
]);
const EXPECT_SERVER_COUNTER_FIELD_NAMES = [
  "packetsReceived",
  "packetsSent",
  "connectAccepted",
  "optionalProtocolAccepted",
  "originMissing",
  "originRejected",
  "closeSessionReceived",
  "emptyDatagramsReceived",
  "batonDatagramsReceived",
  "sizedDatagramsReceived",
  "datagramBytesReceived",
  "zeroBatonReceived",
  "streamTestFinReceived",
  "streamTestFinSent",
  "streamTestBytesReceived",
  "streamTestBytesSent",
  "resetStreamReceived",
  "stopSendingReceived",
  "resetStreamAppError123",
  "stopSendingAppError123"
];
const EXPECT_SERVER_COUNTER_FIELDS =
  new Set(EXPECT_SERVER_COUNTER_FIELD_NAMES);
const EXPECT_SERVER_COUNTER_MIN_FIELDS = new Set(
  EXPECT_SERVER_COUNTER_FIELD_NAMES.map((name) => `${name}Min`));
const EXPECT_SERVER_BOOLEAN_FIELDS = new Set([
  "writableBadChunkCloseReceived",
  "browserCloseReceived"
]);
const EXPECT_SERVER_FIELDS = new Set([
  ...EXPECT_SERVER_COUNTER_FIELDS,
  ...EXPECT_SERVER_COUNTER_MIN_FIELDS,
  ...EXPECT_SERVER_BOOLEAN_FIELDS
]);
const SCENARIO_FIELDS = new Set([
  "id",
  "title",
  "runner",
  "wtUrl",
  "protocol",
  "requireDatagram",
  "useByob",
  "datagramReceiveMode",
  "datagramSendMode",
  "datagramSendSize",
  "datagramSendCount",
  "streamMode",
  "streamSize",
  "streamCount",
  "timeoutMs",
  "certificateHashMode",
  "certificateHashAlgorithm",
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

function requireNonNegativeIntegerOrNull(value, name, path) {
  if (value !== null && (!Number.isInteger(value) || value < 0)) {
    throw new Error(`invalid manifest field ${name}: expected non-negative integer or null in ${path}`);
  }
}

function requireBooleanOrNull(value, name, path) {
  if (value !== null && typeof value !== "boolean") {
    throw new Error(`invalid manifest field ${name}: expected boolean or null in ${path}`);
  }
}

function requireStringValue(value, name, path, allowEmpty = false) {
  if (typeof value !== "string" || (!allowEmpty && value.length === 0)) {
    throw new Error(`invalid manifest field ${name}: expected string in ${path}`);
  }
}

function requireStringArray(value, name, path) {
  if (!Array.isArray(value)) {
    throw new Error(`invalid manifest field ${name}: expected string array in ${path}`);
  }
  for (const [index, entry] of value.entries()) {
    requireStringValue(entry, `${name}[${index}]`, path);
  }
}

function requireNonNegativeIntegerArray(value, name, path) {
  if (!Array.isArray(value)) {
    throw new Error(`invalid manifest field ${name}: expected non-negative integer array in ${path}`);
  }
  for (const [index, entry] of value.entries()) {
    requireNonNegativeIntegerOrNull(entry, `${name}[${index}]`, path);
    if (entry === null) {
      throw new Error(`invalid manifest field ${name}[${index}]: expected non-negative integer in ${path}`);
    }
  }
}

function requireNonNegativeIntegerArrayArray(value, name, path) {
  if (!Array.isArray(value) || value.length === 0) {
    throw new Error(`invalid manifest field ${name}: expected non-empty array of arrays in ${path}`);
  }
  for (const [index, entry] of value.entries()) {
    requireNonNegativeIntegerArray(entry, `${name}[${index}]`, path);
  }
}

function requireDatagramMode(value, name, path) {
  if (!["baton", "empty", "length"].includes(value)) {
    throw new Error(`invalid manifest field ${name}: unsupported mode ${value} in ${path}`);
  }
}

function requireStreamMode(value, name, path) {
  if (!["baton", "client-bidi-echo", "client-uni-reply",
    "server-uni", "server-bidi", "browser-abort-bidi",
    "browser-abort-uni", "browser-cancel-incoming-bidi",
    "browser-cancel-incoming-uni"].includes(value)) {
    throw new Error(`invalid manifest field ${name}: unsupported mode ${value} in ${path}`);
  }
}

function rejectUnknownFields(object, allowed, name) {
  for (const field of Object.keys(object)) {
    if (!allowed.has(field)) {
      throw new Error(`invalid manifest field ${name}.${field}: unknown field`);
    }
  }
}

function validateServerExpectation(server, path, name) {
  if (!isObject(server)) {
    throw new Error(`invalid manifest field ${name}: expected object in ${path}`);
  }
  rejectUnknownFields(server, EXPECT_SERVER_FIELDS, name);
  for (const [field, value] of Object.entries(server)) {
    const fieldName = `${name}.${field}`;
    if (EXPECT_SERVER_COUNTER_FIELDS.has(field) ||
      EXPECT_SERVER_COUNTER_MIN_FIELDS.has(field)) {
      requireNonNegativeIntegerOrNull(value, fieldName, path);
    } else {
      requireBooleanOrNull(value, fieldName, path);
    }
  }
}

function validateScenarioExpect(expect, path, scenarioId) {
  validateExpectation(expect, path, `${scenarioId}.expect`, true, false);
}

function validateExpectation(expect, path, name, requireOk, allowNull) {
  rejectUnknownFields(expect, EXPECT_OVERRIDE_FIELDS, name);
  if (requireOk) {
    requireBoolean(expect.ok, `${name}.ok`);
  }
  for (const [field, value] of Object.entries(expect)) {
    const fieldName = `${name}.${field}`;
    if (field === "server") {
      validateServerExpectation(value, path, fieldName);
    } else if (field === "ok") {
      requireBoolean(value, fieldName);
    } else if (value === null && allowNull) {
      continue;
    } else if (EXPECT_BOOLEAN_FIELDS.has(field)) {
      requireBoolean(value, fieldName);
    } else if (EXPECT_STRING_FIELDS.has(field)) {
      requireStringValue(value, fieldName, path, field === "protocol");
    } else if (EXPECT_MODE_FIELDS.has(field)) {
      requireDatagramMode(value, fieldName, path);
    } else if (field === "streamMode") {
      requireStreamMode(value, fieldName, path);
    } else if (EXPECT_COUNTER_FIELDS.has(field)) {
      requireNonNegativeIntegerOrNull(value, fieldName, path);
    } else if (EXPECT_NUMBER_ARRAY_FIELDS.has(field)) {
      requireNonNegativeIntegerArray(value, fieldName, path);
    } else if (field === "sentVariants") {
      requireNonNegativeIntegerArrayArray(value, fieldName, path);
    } else if (EXPECT_STRING_ARRAY_FIELDS.has(field)) {
      requireStringArray(value, fieldName, path);
    } else {
      throw new Error(`invalid manifest field ${fieldName}: no value validator`);
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
  if (Object.prototype.hasOwnProperty.call(scenario, "datagramReceiveMode") &&
    !["baton", "empty", "length"].includes(scenario.datagramReceiveMode)) {
    throw new Error(`invalid manifest field ${scenario.id}.datagramReceiveMode: unsupported mode ${scenario.datagramReceiveMode}`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "datagramSendMode") &&
    !["baton", "empty", "length"].includes(scenario.datagramSendMode)) {
    throw new Error(`invalid manifest field ${scenario.id}.datagramSendMode: unsupported mode ${scenario.datagramSendMode}`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "datagramSendSize")) {
    requirePositiveInteger(scenario.datagramSendSize, `${scenario.id}.datagramSendSize`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "datagramSendCount")) {
    requirePositiveInteger(scenario.datagramSendCount, `${scenario.id}.datagramSendCount`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "streamMode")) {
    requireStreamMode(scenario.streamMode, `${scenario.id}.streamMode`, path);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "streamSize")) {
    requireNonNegativeIntegerOrNull(scenario.streamSize, `${scenario.id}.streamSize`, path);
    if (scenario.streamSize === null) {
      throw new Error(`invalid manifest field ${scenario.id}.streamSize: expected non-negative integer in ${path}`);
    }
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "streamCount")) {
    requirePositiveInteger(scenario.streamCount, `${scenario.id}.streamCount`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "timeoutMs")) {
    requirePositiveInteger(scenario.timeoutMs, `${scenario.id}.timeoutMs`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "certificateHashMode") &&
    !["generated", "wrong"].includes(scenario.certificateHashMode)) {
    throw new Error(`invalid manifest field ${scenario.id}.certificateHashMode: unsupported mode ${scenario.certificateHashMode}`);
  }
  if (Object.prototype.hasOwnProperty.call(scenario, "certificateHashAlgorithm")) {
    requireString(scenario.certificateHashAlgorithm, `${scenario.id}.certificateHashAlgorithm`);
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
  validateScenarioExpect(scenario.expect, path, scenario.id);
}

function usage() {
  console.error([
    "usage:",
    "  node tests/webtransport/e2e/runners/run-browser.mjs list [--manifest <path>] [--expected <path>] [--json]",
    "  node tests/webtransport/e2e/runners/run-browser.mjs support [--manifest <path>] [--expected-dir <path>] [--json]",
    "  node tests/webtransport/e2e/runners/run-browser.mjs coverage [--manifest <path>] [--expected-dir <path>] [--json]",
    "  node tests/webtransport/e2e/runners/run-browser.mjs --browser <chrome|edge|firefox|safari> [--manifest <path>] [--expected <path>] [--no-expected] [--scenario <id>] [--json]"
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

function requireExpectedString(object, name, path, owner = "expected-results file") {
  if (typeof object[name] !== "string" || object[name].length === 0) {
    throw new Error(`${owner} must contain a non-empty ${name}: ${path}`);
  }
}

function validateExpectedEntry(entry, path, index) {
  if (!isObject(entry)) {
    throw new Error(`expected[${index}] must be an object in ${path}`);
  }
  rejectUnknownFields(entry, EXPECTED_ENTRY_FIELDS, `expected[${index}]`);
  for (const field of ["scenario", "status", "category", "browserVersion",
    "platform", "reason", "evidence"]) {
    requireExpectedString(entry, field, path, `expected[${index}]`);
  }
  if (!EXPECTED_STATUSES.has(entry.status)) {
    throw new Error(`invalid expected-result status in ${path}: ${entry.status}`);
  }
  if (entry.status === "pass") {
    if (!isObject(entry.expect) || Object.keys(entry.expect).length === 0) {
      throw new Error(`expected-result pass entry has no expect overrides in ${path}: ${entry.scenario}`);
    }
    validateExpectation(entry.expect, path, `expected[${index}].expect`,
      false, true);
  } else if (Object.prototype.hasOwnProperty.call(entry, "expect")) {
    throw new Error(`expected-result skip entry must not include expect in ${path}: ${entry.scenario}`);
  }
}

function loadExpected(path, manifest) {
  if (!path) {
    return { path: "", entries: new Map() };
  }
  if (!existsSync(path)) {
    throw new Error(`expected-results file not found: ${path}`);
  }

  const expected = JSON.parse(readFileSync(path, "utf8"));
  if (!isObject(expected)) {
    throw new Error(`expected-results file must be an object: ${path}`);
  }
  rejectUnknownFields(expected, EXPECTED_RESULT_FIELDS, "expected-results");
  requireExpectedString(expected, "browser", path);
  requireExpectedString(expected, "channel", path);
  requireExpectedString(expected, "platform", path);
  if (!Array.isArray(expected.expected)) {
    throw new Error(`expected-results file has no expected array: ${path}`);
  }
  const entries = new Map();
  const scenarioIds = manifestScenarioIds(manifest);
  for (const [index, entry] of expected.expected.entries()) {
    validateExpectedEntry(entry, path, index);
    if (!scenarioIds.has(entry.scenario)) {
      throw new Error(`expected-result entry references unknown scenario in ${path}: ${entry.scenario}`);
    }
    if (entries.has(entry.scenario)) {
      throw new Error(`duplicate expected-result scenario in ${path}: ${entry.scenario}`);
    }
    entries.set(entry.scenario, entry);
  }
  return {
    path,
    browser: expected.browser,
    channel: expected.channel,
    platform: expected.platform,
    entries
  };
}

function requireExpectedBrowser(expected, browser) {
  if (expected.path && expected.browser !== browser) {
    throw new Error(`expected-results browser ${expected.browser} does not match selected browser ${browser}: ${expected.path}`);
  }
}

function mergeExpect(base, override) {
  const merged = { ...(base || {}), ...(override || {}) };
  if ((base && base.server) || (override && override.server)) {
    merged.server = { ...((base && base.server) || {}), ...((override && override.server) || {}) };
  }
  return merged;
}

function expectedMetadata(entry) {
  const metadata = {
    status: entry.status,
    category: entry.category,
    browserVersion: entry.browserVersion || "",
    platform: entry.platform || "",
    reason: entry.reason,
    evidence: entry.evidence
  };
  if (entry.expect) {
    metadata.expect = entry.expect;
  }
  return metadata;
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

function assertArrayEquals(id, name, actual, expected, orderMatters = true) {
  if (!arrayEquals(actual, expected, orderMatters)) {
    throw new Error(`${id}: expected ${name} ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function arrayEquals(actual, expected, orderMatters = true) {
  const actualArray = orderMatters ? actual : Array.isArray(actual) ?
    [...actual].sort((a, b) => a - b) : actual;
  const expectedArray = orderMatters ? expected : [...expected].sort((a, b) => a - b);

  return Array.isArray(actualArray) && actualArray.length === expectedArray.length &&
    !actualArray.some((value, index) => value !== expectedArray[index]);
}

function assertArrayEqualsAny(id, name, actual, variants, orderMatters = true) {
  for (const expected of variants) {
    if (arrayEquals(actual, expected, orderMatters)) {
      return;
    }
  }
  throw new Error(`${id}: expected ${name} to match one of ${JSON.stringify(variants)}, got ${JSON.stringify(actual)}`);
}

function assertDiagnosticTestsInclude(id, name, diagnostic, expectedNames) {
  const tests = diagnostic && Array.isArray(diagnostic.tests) ?
    diagnostic.tests.map((entry) => entry && entry.name) : [];
  for (const expectedName of expectedNames) {
    if (!tests.includes(expectedName)) {
      throw new Error(`${id}: expected ${name} test ${JSON.stringify(expectedName)}, got ${JSON.stringify(tests)}`);
    }
  }
}

function hasExpectedValue(expect, name) {
  return Object.prototype.hasOwnProperty.call(expect, name) && expect[name] !== null;
}

function assertScenarioResult(id, result, expect) {
  const sequenceOrderMatters = expect.sequenceOrderMatters !== false;

  if (result.ok !== expect.ok) {
    throw new Error(`${id}: expected ok=${expect.ok}, got ${result.ok}`);
  }
  for (const name of ["url", "protocol", "requireDatagram", "constructorRequireUnreliable", "useByob", "readyMs", "closedMs", "datagramsSent"]) {
    if (hasExpectedValue(expect, name) && result[name] !== expect[name]) {
      throw new Error(`${id}: expected ${name}=${JSON.stringify(expect[name])}, got ${JSON.stringify(result[name])}`);
    }
  }
  if (hasExpectedValue(expect, "datagramReceiveMode") &&
    result.datagramReceiveMode !== expect.datagramReceiveMode) {
    throw new Error(`${id}: expected datagramReceiveMode=${JSON.stringify(expect.datagramReceiveMode)}, got ${JSON.stringify(result.datagramReceiveMode)}`);
  }
  if (hasExpectedValue(expect, "datagramSendMode") &&
    (result.datagramSendMode || "baton") !== expect.datagramSendMode) {
    throw new Error(`${id}: expected datagramSendMode=${JSON.stringify(expect.datagramSendMode)}, got ${JSON.stringify(result.datagramSendMode)}`);
  }
  if (hasExpectedValue(expect, "datagramSendSize") &&
    (result.datagramSendSize || 0) !== expect.datagramSendSize) {
    throw new Error(`${id}: expected datagramSendSize=${JSON.stringify(expect.datagramSendSize)}, got ${JSON.stringify(result.datagramSendSize || 0)}`);
  }
  if (hasExpectedValue(expect, "datagramSendCount") &&
    (result.datagramSendCount || 1) !== expect.datagramSendCount) {
    throw new Error(`${id}: expected datagramSendCount=${JSON.stringify(expect.datagramSendCount)}, got ${JSON.stringify(result.datagramSendCount || 1)}`);
  }
  if (hasExpectedValue(expect, "streamMode") &&
    (result.streamMode || "baton") !== expect.streamMode) {
    throw new Error(`${id}: expected streamMode=${JSON.stringify(expect.streamMode)}, got ${JSON.stringify(result.streamMode || "baton")}`);
  }
  for (const name of ["streamSize", "streamCount", "streamBytesSent",
    "streamBytesReceived", "streamFinSent", "streamFinReceived"]) {
    if (hasExpectedValue(expect, name) && result[name] !== expect[name]) {
      throw new Error(`${id}: expected ${name}=${JSON.stringify(expect[name])}, got ${JSON.stringify(result[name])}`);
    }
  }
  if (expect.received) {
    assertArrayEquals(id, "received", result.received, expect.received,
      sequenceOrderMatters);
  }
  if (expect.sent) {
    assertArrayEquals(id, "sent", result.sent, expect.sent,
      sequenceOrderMatters);
  }
  if (expect.sentVariants) {
    assertArrayEqualsAny(id, "sent", result.sent, expect.sentVariants,
      sequenceOrderMatters);
  }
  if (expect.datagramsReceived) {
    assertArrayEquals(id, "datagramsReceived", result.datagramsReceived, expect.datagramsReceived);
  }
  if (expect.datagramLengths) {
    assertArrayEquals(id, "datagramLengths", result.datagramLengths,
      expect.datagramLengths);
  }
  if (hasExpectedValue(expect, "datagramLengthsMin") &&
    (!Array.isArray(result.datagramLengths) ||
      result.datagramLengths.length < expect.datagramLengthsMin)) {
    throw new Error(`${id}: expected at least ${expect.datagramLengthsMin} datagram lengths, got ${JSON.stringify(result.datagramLengths)}`);
  }
  if (hasExpectedValue(expect, "datagramLength")) {
    const lengths = Array.isArray(result.datagramLengths) ?
      result.datagramLengths : [];
    if (lengths.length === 0 ||
      lengths.some((length) => length !== expect.datagramLength)) {
      throw new Error(`${id}: expected all datagram lengths to be ${expect.datagramLength}, got ${JSON.stringify(result.datagramLengths)}`);
    }
  }
  if (expect.eventsInclude) {
    const events = Array.isArray(result.events) ?
      result.events.map((entry) => entry && entry.event) : [];
    for (const event of expect.eventsInclude) {
      if (!events.includes(event)) {
        throw new Error(`${id}: expected event ${JSON.stringify(event)}, got ${JSON.stringify(events)}`);
      }
    }
  }
  if (expect.eventsExclude) {
    const events = Array.isArray(result.events) ?
      result.events.map((entry) => entry && entry.event) : [];
    for (const event of expect.eventsExclude) {
      if (events.includes(event)) {
        throw new Error(`${id}: excluded event ${JSON.stringify(event)} was present in ${JSON.stringify(events)}`);
      }
    }
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
  if (hasExpectedValue(expect, "protocolConstructorOk") &&
    (!result.protocolConstructor ||
      result.protocolConstructor.ok !== expect.protocolConstructorOk)) {
    throw new Error(`${id}: protocol constructor check failed`);
  }
  if (expect.protocolConstructorTestsInclude) {
    assertDiagnosticTestsInclude(id, "protocolConstructor", result.protocolConstructor,
      expect.protocolConstructorTestsInclude);
  }
  if (hasExpectedValue(expect, "urlConstructorOk") &&
    (!result.urlConstructor ||
      result.urlConstructor.ok !== expect.urlConstructorOk)) {
    throw new Error(`${id}: URL constructor check failed`);
  }
  if (expect.urlConstructorTestsInclude) {
    assertDiagnosticTestsInclude(id, "urlConstructor", result.urlConstructor,
      expect.urlConstructorTestsInclude);
  }
  if (hasExpectedValue(expect, "optionsConstructorOk") &&
    (!result.optionsConstructor ||
      result.optionsConstructor.ok !== expect.optionsConstructorOk)) {
    throw new Error(`${id}: options constructor check failed`);
  }
  if (expect.optionsConstructorTestsInclude) {
    assertDiagnosticTestsInclude(id, "optionsConstructor", result.optionsConstructor,
      expect.optionsConstructorTestsInclude);
  }
  if (hasExpectedValue(expect, "datagramWritableOk") &&
    (!result.datagramWritable ||
      result.datagramWritable.ok !== expect.datagramWritableOk)) {
    throw new Error(`${id}: datagram writable check failed`);
  }
  if (expect.datagramWritableTestsInclude) {
    assertDiagnosticTestsInclude(id, "datagramWritable", result.datagramWritable,
      expect.datagramWritableTestsInclude);
  }
  if (hasExpectedValue(expect, "streamWritableOk") &&
    (!result.streamWritable ||
      result.streamWritable.ok !== expect.streamWritableOk)) {
    throw new Error(`${id}: stream writable check failed`);
  }
  if (expect.streamWritableTestsInclude) {
    assertDiagnosticTestsInclude(id, "streamWritable", result.streamWritable,
      expect.streamWritableTestsInclude);
  }
  if (hasExpectedValue(expect, "postCloseOk") &&
    (!result.postClose || result.postClose.ok !== expect.postCloseOk)) {
    throw new Error(`${id}: post-close check failed`);
  }
  if (expect.postCloseTestsInclude) {
    assertDiagnosticTestsInclude(id, "postClose", result.postClose,
      expect.postCloseTestsInclude);
  }
  if (hasExpectedValue(expect, "postCloseDatagramOk") &&
    (!result.postCloseDatagram ||
      result.postCloseDatagram.ok !== expect.postCloseDatagramOk)) {
    throw new Error(`${id}: post-close datagram check failed`);
  }
  if (expect.postCloseDatagramTestsInclude) {
    assertDiagnosticTestsInclude(id, "postCloseDatagram",
      result.postCloseDatagram, expect.postCloseDatagramTestsInclude);
  }
  if (hasExpectedValue(expect, "closeSessionOk") &&
    (!result.closeSession ||
      result.closeSession.ok !== expect.closeSessionOk)) {
    throw new Error(`${id}: close-session check failed`);
  }
  if (expect.closeSessionTestsInclude) {
    assertDiagnosticTestsInclude(id, "closeSession", result.closeSession,
      expect.closeSessionTestsInclude);
  }
  if (hasExpectedValue(expect, "sessionDiagnosticsOk") &&
    (!result.sessionDiagnostics ||
      result.sessionDiagnostics.ok !== expect.sessionDiagnosticsOk)) {
    throw new Error(`${id}: session diagnostics check failed`);
  }
  if (expect.sessionDiagnosticsTestsInclude) {
    assertDiagnosticTestsInclude(id, "sessionDiagnostics",
      result.sessionDiagnostics, expect.sessionDiagnosticsTestsInclude);
  }
  if (expect.server) {
    if (!result.server) {
      throw new Error(`${id}: missing server summary`);
    }
    for (const [name, minimum] of Object.entries(expect.server)) {
      if (minimum === null) {
        /* Expected-result manifests use null to omit one inherited
         * server-summary assertion when browser-version evidence shows the
         * field is not stable enough to claim.
         */
        continue;
      }
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

function childTimeoutMs(scenario) {
  if (Number.isFinite(CHILD_TIMEOUT_OVERRIDE_MS) &&
    CHILD_TIMEOUT_OVERRIDE_MS > 0) {
    return CHILD_TIMEOUT_OVERRIDE_MS;
  }
  const scenarioTimeoutMs = Number(scenario.timeoutMs || 30000);
  return Math.max(90000, scenarioTimeoutMs * 4 + 60000);
}

function runChild(command, args, env, timeoutMs) {
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
    let timedOut = false;
    let killTimer = null;
    const timeoutTimer = Number.isFinite(timeoutMs) && timeoutMs > 0 ?
      setTimeout(() => {
        timedOut = true;
        try {
          child.kill("SIGTERM");
        } catch (_) {}
        killTimer = setTimeout(() => {
          try {
            child.kill("SIGKILL");
          } catch (_) {}
        }, 3000);
      }, timeoutMs) : null;

    child.once("exit", (code, signal) => {
      if (timeoutTimer) {
        clearTimeout(timeoutTimer);
      }
      if (killTimer) {
        clearTimeout(killTimer);
      }
      if (code === 0 && !timedOut) {
        resolveRun({ stdout, stderr });
      } else {
        const reason = timedOut ?
          `timeout after ${timeoutMs} ms` : `code=${code} signal=${signal}`;
        rejectRun(new Error(`${args[0]} failed: ${reason}\n${stdout}\n${stderr}`));
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
      expected: expectedMetadata(expectedEntry)
    };
  }
  if (expectedEntry && expectedEntry.status !== "pass") {
    throw new Error(`${scenario.id}: unsupported expected status ${expectedEntry.status}`);
  }

  const runner = BROWSER_RUNNERS[browser];
  if (!runner) {
    throw new Error(`unsupported browser: ${browser}`);
  }
  const rendered = renderTemplate(scenario, vars);
  const expectedOverride = expectedEntry && expectedEntry.expect ?
    renderTemplate(expectedEntry.expect, vars) : {};
  const scenarioExpect = mergeExpect(rendered.expect || {}, expectedOverride);
  const env = {
    ...process.env,
    PICOQUIC_WT_URL: rendered.wtUrl,
    PICOQUIC_WT_PROTOCOL: rendered.protocol || "devious-baton-00",
    PICOQUIC_WT_REQUIRE_DATAGRAM: rendered.requireDatagram === false ? "0" : "1",
    PICOQUIC_WT_USE_BYOB: rendered.useByob === false ? "0" : "1",
    PICOQUIC_WT_DATAGRAM_RECEIVE_MODE: rendered.datagramReceiveMode || "baton",
    PICOQUIC_WT_DATAGRAM_SEND_MODE: rendered.datagramSendMode || "baton",
    PICOQUIC_WT_STREAM_MODE: rendered.streamMode || "baton",
    PICOQUIC_WT_STREAM_SIZE: String(rendered.streamSize || 0),
    PICOQUIC_WT_STREAM_COUNT: String(rendered.streamCount || 1),
    PICOQUIC_WT_EXPECT_OK: scenarioExpect.ok === false ? "0" : "1",
    PICOQUIC_WT_PROTOCOL_CONSTRUCTOR: scenarioExpect.ok === false ? "0" : "1",
    PICOQUIC_WT_INCLUDE_SERVER_SUMMARY: "1",
    PICOQUIC_WT_TIMEOUT_MS: String(rendered.timeoutMs || 30000),
    PICOQUIC_WT_PORT: String(vars.port)
  };
  if (rendered.certificateHashAlgorithm) {
    env.PICOQUIC_WT_CERT_HASH_ALG = rendered.certificateHashAlgorithm;
  }
  if (Number.isInteger(rendered.datagramSendSize)) {
    env.PICOQUIC_WT_DATAGRAM_SEND_SIZE = String(rendered.datagramSendSize);
  }
  if (Number.isInteger(rendered.datagramSendCount)) {
    env.PICOQUIC_WT_DATAGRAM_SEND_COUNT = String(rendered.datagramSendCount);
  }
  if (Number.isInteger(scenarioExpect.datagramLengthsMin)) {
    env.PICOQUIC_WT_DATAGRAM_RECEIVE_MIN =
      String(scenarioExpect.datagramLengthsMin);
  }
  if (Array.isArray(scenarioExpect.received)) {
    env.PICOQUIC_WT_EXPECT_RECEIVED = JSON.stringify(scenarioExpect.received);
  }
  if (Array.isArray(scenarioExpect.sent)) {
    env.PICOQUIC_WT_EXPECT_SENT = JSON.stringify(scenarioExpect.sent);
  }
  if (Array.isArray(scenarioExpect.sentVariants)) {
    env.PICOQUIC_WT_EXPECT_SENT_VARIANTS =
      JSON.stringify(scenarioExpect.sentVariants);
  }
  if (Array.isArray(scenarioExpect.datagramsReceived)) {
    env.PICOQUIC_WT_EXPECT_DATAGRAMS_RECEIVED =
      JSON.stringify(scenarioExpect.datagramsReceived);
  }
  if (Array.isArray(scenarioExpect.datagramLengths)) {
    env.PICOQUIC_WT_EXPECT_DATAGRAM_LENGTHS =
      JSON.stringify(scenarioExpect.datagramLengths);
  }
  if (Number.isInteger(scenarioExpect.datagramsSent)) {
    env.PICOQUIC_WT_EXPECT_DATAGRAMS_SENT = String(scenarioExpect.datagramsSent);
  }
  if (scenarioExpect.sequenceOrderMatters === false) {
    env.PICOQUIC_WT_EXPECT_ORDERED = "0";
  }
  if (rendered.certificateHashMode === "wrong") {
    env.PICOQUIC_WT_CERT_HASH = WRONG_CERT_HASH;
    env.PICOQUIC_WT_IGNORE_CERT_ERRORS = "0";
  } else if (rendered.certificateHashAlgorithm &&
    rendered.certificateHashAlgorithm !== "sha-256") {
    env.PICOQUIC_WT_IGNORE_CERT_ERRORS = "0";
  }
  const run = await runChild(process.execPath, [runner], env,
    childTimeoutMs(rendered));
  const result = parseJsonOutput(run.stdout);
  assertScenarioResult(rendered.id, result, scenarioExpect);
  const scenarioResult = {
    id: rendered.id,
    title: rendered.title || "",
    status: "pass",
    coverage: rendered.coverage || [],
    result
  };
  if (expectedEntry) {
    scenarioResult.expected = expectedMetadata(expectedEntry);
  }
  return scenarioResult;
}

function commandList(args) {
  const manifestPath = resolve(takeOption(args, "--manifest", DEFAULT_MANIFEST));
  const expectedPath = takeOption(args, "--expected", "");
  const json = hasOption(args, "--json");
  if (args.length !== 0) {
    throw new Error(`unexpected list arguments: ${args.join(" ")}`);
  }
  const manifest = loadManifest(manifestPath);
  const expected = expectedPath ?
    loadExpected(resolve(expectedPath), manifest) : { path: "", entries: new Map() };
  const result = {
    suite: manifest.suite || "",
    description: manifest.description || "",
    expected: expected.path,
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

function supportStatus(entry) {
  if (!entry) {
    return "claimed-pass";
  }
  if (entry.status === "pass") {
    return "expected-pass";
  }
  return entry.status;
}

function loadSupport(manifestPath, expectedDir) {
  const manifest = loadManifest(manifestPath);
  const browsers = Object.keys(BROWSER_RUNNERS);
  return {
    manifest,
    result: {
      suite: manifest.suite || "",
      description: manifest.description || "",
      manifest: manifestPath,
      expectedDir,
      browsers: browsers.map((browser) => {
        const expectedPath = join(expectedDir, `${browser}-stable.json`);
        const expected = loadExpected(expectedPath, manifest);
        requireExpectedBrowser(expected, browser);
        const scenarios = manifest.scenarios.map((scenario) => {
          const entry = expected.entries.get(scenario.id);
          const summary = {
            id: scenario.id,
            title: scenario.title || "",
            coverage: scenario.coverage || [],
            status: supportStatus(entry)
          };
          if (entry) {
            summary.expected = expectedMetadata(entry);
          }
          return summary;
        });
        return {
          browser,
          expected: expected.path,
          counts: scenarios.reduce((counts, scenario) => {
            counts[scenario.status] = (counts[scenario.status] || 0) + 1;
            return counts;
          }, {}),
          scenarios
        };
      })
    }
  };
}

function commandSupport(args) {
  const manifestPath = resolve(takeOption(args, "--manifest", DEFAULT_MANIFEST));
  const expectedDir = resolve(takeOption(args, "--expected-dir",
    DEFAULT_EXPECTED_DIR));
  const json = hasOption(args, "--json");
  if (args.length !== 0) {
    throw new Error(`unexpected support arguments: ${args.join(" ")}`);
  }

  const { manifest, result } = loadSupport(manifestPath, expectedDir);

  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`# suite: ${result.suite}`);
    console.log(["scenario", ...result.browsers.map((entry) => entry.browser)]
      .join("\t"));
    for (const [index, scenario] of manifest.scenarios.entries()) {
      console.log([
        scenario.id,
        ...result.browsers.map((browser) => browser.scenarios[index].status)
      ].join("\t"));
    }
  }
}

function addCoverageStatus(coverageEntry, browser, status) {
  const browserEntry = coverageEntry.browsers[browser] ||
    { counts: {} };
  browserEntry.counts[status] = (browserEntry.counts[status] || 0) + 1;
  coverageEntry.browsers[browser] = browserEntry;
}

function commandCoverage(args) {
  const manifestPath = resolve(takeOption(args, "--manifest", DEFAULT_MANIFEST));
  const expectedDir = resolve(takeOption(args, "--expected-dir",
    DEFAULT_EXPECTED_DIR));
  const json = hasOption(args, "--json");
  if (args.length !== 0) {
    throw new Error(`unexpected coverage arguments: ${args.join(" ")}`);
  }

  const { result: support } = loadSupport(manifestPath, expectedDir);
  const coverage = new Map();
  for (const browser of support.browsers) {
    for (const scenario of browser.scenarios) {
      for (const tag of scenario.coverage) {
        if (!coverage.has(tag)) {
          coverage.set(tag, { tag, scenarios: new Set(), browsers: {} });
        }
        const entry = coverage.get(tag);
        entry.scenarios.add(scenario.id);
        addCoverageStatus(entry, browser.browser, scenario.status);
      }
    }
  }
  const result = {
    suite: support.suite,
    description: support.description,
    manifest: support.manifest,
    expectedDir: support.expectedDir,
    coverage: Array.from(coverage.values())
      .sort((left, right) => left.tag.localeCompare(right.tag))
      .map((entry) => ({
        tag: entry.tag,
        scenarioCount: entry.scenarios.size,
        scenarios: Array.from(entry.scenarios).sort(),
        browsers: entry.browsers
      }))
  };

  if (json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`# suite: ${result.suite}`);
    console.log([
      "coverage",
      "scenarios",
      ...support.browsers.map((browser) => browser.browser)
    ].join("\t"));
    for (const entry of result.coverage) {
      console.log([
        entry.tag,
        String(entry.scenarioCount),
        ...support.browsers.map((browser) =>
          JSON.stringify(entry.browsers[browser.browser]?.counts || {}))
      ].join("\t"));
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
  requireExpectedBrowser(expected, browser);
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
  } else if (args[0] === "support") {
    args.shift();
    commandSupport(args);
  } else if (args[0] === "coverage") {
    args.shift();
    commandCoverage(args);
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
