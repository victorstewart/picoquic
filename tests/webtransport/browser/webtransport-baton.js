(function () {
  "use strict";

  var DEFAULT_BATON_PATH = "/baton?version=0&baton=251&count=1";
  var DEFAULT_PROTOCOL = "devious-baton-00";
  var DEFAULT_TIMEOUT_MS = 15000;
  var READ_BUFFER_SIZE = 16384;
  var MAX_PACKET_BYTES = 262144;

  function hasAutoRun(searchParams) {
    var value = searchParams.get("autorun");
    return value === "1" || value === "true" || value === "yes";
  }

  function defaultTarget() {
    var url = new URL(DEFAULT_BATON_PATH, window.location.href);
    url.hash = "";
    return url.href;
  }

  function nowMs() {
    return Math.round(performance.now());
  }

  function errorText(error) {
    if (error && typeof error === "object") {
      return (error.name ? error.name + ": " : "") + (error.message || String(error));
    }
    return String(error);
  }

  function decodeBase64Url(text) {
    var normalized = text.replace(/-/g, "+").replace(/_/g, "/");
    while ((normalized.length & 3) !== 0) {
      normalized += "=";
    }

    var binary = atob(normalized);
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  function encodeVarint(value) {
    if (value < 0 || value > 0x3fffffff) {
      throw new RangeError("unsupported varint value " + value);
    }
    if (value < 0x40) {
      return new Uint8Array([value]);
    }
    if (value < 0x4000) {
      return new Uint8Array([0x40 | (value >> 8), value & 0xff]);
    }
    return new Uint8Array([
      0x80 | ((value >> 24) & 0x3f),
      (value >> 16) & 0xff,
      (value >> 8) & 0xff,
      value & 0xff
    ]);
  }

  function makeBatonPacket(baton, paddingLength) {
    var padding = paddingLength || 0;
    var prefix = encodeVarint(padding);
    var packet = new Uint8Array(prefix.length + padding + 1);
    packet.set(prefix, 0);
    packet[prefix.length + padding] = baton & 0xff;
    return packet;
  }

  function varintLength(firstByte) {
    return 1 << (firstByte >> 6);
  }

  function decodeVarint(bytes, length) {
    if (length === 8) {
      var big = BigInt(bytes[0] & 0x3f);
      for (var i = 1; i < 8; i++) {
        big = (big << 8n) | BigInt(bytes[i]);
      }
      if (big > BigInt(Number.MAX_SAFE_INTEGER)) {
        throw new RangeError("padding length exceeds Number.MAX_SAFE_INTEGER");
      }
      return Number(big);
    }

    var value = bytes[0] & 0x3f;
    for (var j = 1; j < length; j++) {
      value = (value * 256) + bytes[j];
    }
    return value;
  }

  function BatonDecoder(maxPacketBytes) {
    this.varintBytes = new Uint8Array(8);
    this.varintCount = 0;
    this.varintNeed = 0;
    this.paddingExpected = -1;
    this.paddingRead = 0;
    this.baton = -1;
    this.bytes = 0;
    this.maxPacketBytes = maxPacketBytes;
  }

  BatonDecoder.prototype.push = function push(chunk) {
    var offset = 0;
    var length = chunk.byteLength;
    this.bytes += length;
    if (this.bytes > this.maxPacketBytes) {
      throw new RangeError("baton packet too large");
    }

    while (offset < length && this.paddingExpected < 0) {
      if (this.varintCount === 0) {
        this.varintNeed = varintLength(chunk[offset]);
      }
      this.varintBytes[this.varintCount++] = chunk[offset++];
      if (this.varintCount === this.varintNeed) {
        this.paddingExpected = decodeVarint(this.varintBytes, this.varintNeed);
        if (this.paddingExpected + this.varintNeed + 1 > this.maxPacketBytes) {
          throw new RangeError("baton padding too large");
        }
      }
    }

    if (offset < length && this.paddingExpected >= 0 && this.paddingRead < this.paddingExpected) {
      var padEnd = offset + Math.min(length - offset, this.paddingExpected - this.paddingRead);
      for (var i = offset; i < padEnd; i++) {
        if (chunk[i] !== 0) {
          throw new Error("non-zero baton padding byte");
        }
      }
      this.paddingRead += padEnd - offset;
      offset = padEnd;
    }

    if (offset < length && this.paddingExpected >= 0 && this.paddingRead === this.paddingExpected) {
      if (this.baton >= 0) {
        throw new Error("extra bytes after baton");
      }
      this.baton = chunk[offset++];
    }

    if (offset !== length) {
      throw new Error("extra bytes after baton");
    }
  };

  BatonDecoder.prototype.finish = function finish() {
    if (this.paddingExpected < 0) {
      throw new Error("stream ended before baton padding length");
    }
    if (this.paddingRead !== this.paddingExpected) {
      throw new Error("stream ended inside baton padding");
    }
    if (this.baton < 0) {
      throw new Error("stream ended before baton byte");
    }
    return this.baton;
  };

  async function readBaton(readable, useByob) {
    var decoder = new BatonDecoder(MAX_PACKET_BYTES);
    var byobReader = null;

    if (useByob !== false) {
      try {
        byobReader = readable.getReader({ mode: "byob" });
        var buffer = new ArrayBuffer(READ_BUFFER_SIZE);
        while (true) {
          var result = await byobReader.read(new Uint8Array(buffer));
          if (result.done) {
            byobReader.releaseLock();
            return decoder.finish();
          }
          decoder.push(result.value);
          buffer = result.value.buffer.byteLength >= READ_BUFFER_SIZE ?
            result.value.buffer : new ArrayBuffer(READ_BUFFER_SIZE);
        }
      } catch (error) {
        if (byobReader) {
          try {
            byobReader.releaseLock();
          } catch (_) {}
        }
        if (!(error instanceof TypeError) && !/byob|byte stream/i.test(errorText(error))) {
          throw error;
        }
      }
    }

    var reader = readable.getReader();
    while (true) {
      var read = await reader.read();
      if (read.done) {
        reader.releaseLock();
        return decoder.finish();
      }
      decoder.push(read.value);
    }
  }

  function getDatagramWritable(datagrams) {
    if (!datagrams) {
      return null;
    }
    if (datagrams.writable) {
      return datagrams.writable;
    }
    if (typeof datagrams.createWritable === "function") {
      return datagrams.createWritable();
    }
    return null;
  }

  function buildTransportOptions(options) {
    var requireDatagram = options.requireDatagram !== false;
    var transportOptions = {
      allowPooling: false,
      requireUnreliable: requireDatagram,
      anticipatedConcurrentIncomingBidirectionalStreams: 4,
      anticipatedConcurrentIncomingUnidirectionalStreams: 4
    };

    if (options.protocol) {
      transportOptions.protocols = [options.protocol];
    }

    if (options.certificateHash) {
      transportOptions.serverCertificateHashes = [{
        algorithm: options.certificateHashAlgorithm || "sha-256",
        value: decodeBase64Url(options.certificateHash)
      }];
    }

    return transportOptions;
  }

  function closeConstructedTransport(transport) {
    try {
      transport.ready.catch(function () {});
    } catch (_) {}
    try {
      transport.closed.catch(function () {});
    } catch (_) {}
    try {
      transport.close({ closeCode: 0, reason: "constructor-test" });
    } catch (_) {}
  }

  function buildProtocolConstructorOptions(options, protocols) {
    var transportOptions = buildTransportOptions({
      certificateHash: options.certificateHash,
      certificateHashAlgorithm: options.certificateHashAlgorithm,
      requireDatagram: false
    });
    transportOptions.requireUnreliable = false;
    transportOptions.protocols = protocols;
    return transportOptions;
  }

  function runProtocolConstructorTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    function expectThrows(name, protocols, expectedName) {
      try {
        var transport = new WebTransport(url,
          buildProtocolConstructorOptions(options, protocols));
        closeConstructedTransport(transport);
        record(name, false, "constructor did not throw");
      } catch (error) {
        record(name, error && error.name === expectedName, errorText(error));
      }
    }

    function expectConstructs(name, protocols) {
      try {
        var transport = new WebTransport(url,
          buildProtocolConstructorOptions(options, protocols));
        closeConstructedTransport(transport);
        record(name, true, "");
      } catch (error) {
        record(name, false, errorText(error));
      }
    }

    expectConstructs("valid-protocol", [DEFAULT_PROTOCOL]);
    expectThrows("duplicate-protocol", [DEFAULT_PROTOCOL, DEFAULT_PROTOCOL], "SyntaxError");
    expectThrows("empty-protocol", [""], "SyntaxError");
    expectThrows("long-protocol", [new Array(514).join("a")], "SyntaxError");
    expectThrows("non-isomorphic-protocol", ["\u0100"], "SyntaxError");

    result.ok = result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  function buildUrlConstructorOptions(options) {
    var transportOptions = buildTransportOptions({
      certificateHash: options.certificateHash,
      certificateHashAlgorithm: options.certificateHashAlgorithm,
      requireDatagram: false
    });
    transportOptions.requireUnreliable = false;
    transportOptions.protocols = [options.protocol || DEFAULT_PROTOCOL];
    return transportOptions;
  }

  function runUrlConstructorTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    function expectThrows(name, targetUrl, expectedName) {
      try {
        var transport = new WebTransport(targetUrl,
          buildUrlConstructorOptions(options));
        closeConstructedTransport(transport);
        record(name, false, "constructor did not throw");
      } catch (error) {
        record(name, error && error.name === expectedName, errorText(error));
      }
    }

    expectThrows("http-url", url.replace(/^https:/, "http:"), "SyntaxError");
    expectThrows("url-fragment", url + "#fragment", "SyntaxError");
    expectThrows("malformed-url", "https://[", "SyntaxError");

    result.ok = result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  function runOptionsConstructorTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    function expectThrows(name, configure, expectedName) {
      try {
        var transportOptions = buildUrlConstructorOptions(options);
        configure(transportOptions);
        var transport = new WebTransport(url, transportOptions);
        closeConstructedTransport(transport);
        record(name, false, "constructor did not throw");
      } catch (error) {
        record(name, error && error.name === expectedName, errorText(error));
      }
    }

    expectThrows("allow-pooling-with-certificate-hash", function (transportOptions) {
      transportOptions.allowPooling = true;
    }, "NotSupportedError");

    result.ok = result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  async function runDatagramWritableTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();
    var transport = null;
    var writer = null;

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    try {
      transport = new WebTransport(url, buildTransportOptions(options));
      try {
        transport.closed.catch(function () {});
      } catch (_) {}
      await transport.ready;
      var datagramWritable = getDatagramWritable(transport.datagrams);
      if (!datagramWritable) {
        record("writable-available", false, "WebTransport datagrams are unavailable");
      } else {
        writer = datagramWritable.getWriter();
        await writer.ready;
        try {
          await writer.write("not a BufferSource");
          record("string-chunk", false, "write resolved");
        } catch (error) {
          record("string-chunk", error && error.name === "TypeError",
            errorText(error));
        }
      }
    } catch (error) {
      record("setup", false, errorText(error));
    } finally {
      if (writer) {
        try {
          writer.releaseLock();
        } catch (_) {}
      }
      if (transport) {
        try {
          transport.close({ closeCode: 0, reason: "datagram-writable-test" });
        } catch (_) {}
      }
    }

    result.ok = result.tests.length > 0 && result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  async function runStreamWritableTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();
    var transport = null;
    var writer = null;

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    try {
      transport = new WebTransport(url, buildTransportOptions(options));
      try {
        transport.closed.catch(function () {});
      } catch (_) {}
      await transport.ready;
      var writable = await transport.createUnidirectionalStream();
      writer = writable.getWriter();
      await writer.ready;
      try {
        await writer.write("not a BufferSource");
        record("string-chunk", false, "write resolved");
      } catch (error) {
        record("string-chunk", error && error.name === "TypeError",
          errorText(error));
      }
    } catch (error) {
      record("setup", false, errorText(error));
    } finally {
      if (writer) {
        try {
          writer.releaseLock();
        } catch (_) {}
      }
      if (transport) {
        try {
          transport.close({ closeCode: 0, reason: "stream-writable-test" });
        } catch (_) {}
      }
    }

    result.ok = result.tests.length > 0 && result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  function withTimeout(promise, timeoutMs, onTimeout) {
    var timer = 0;
    var timeout = new Promise(function (_, reject) {
      timer = setTimeout(function () {
        if (onTimeout) {
          onTimeout();
        }
        reject(new Error("timeout after " + timeoutMs + " ms"));
      }, timeoutMs);
    });
    return Promise.race([promise, timeout]).finally(function () {
      clearTimeout(timer);
    });
  }

  async function runBatonTest(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var requireDatagram = options.requireDatagram !== false;
    var result = {
      ok: false,
      url: options.url,
      requireDatagram: requireDatagram,
      constructorRequireUnreliable: requireDatagram,
      useByob: options.useByob !== false,
      startedMs: nowMs(),
      readyMs: 0,
      closedMs: 0,
      received: [],
      sent: [],
      datagramsReceived: [],
      datagramsSent: 0,
      protocol: "",
      events: []
    };
    window.__picoquicWebTransportProgress = result;
    var timeoutMs = options.timeoutMs || DEFAULT_TIMEOUT_MS;
    var transport = new WebTransport(options.url, buildTransportOptions(options));
    var finished = false;
    var sentZero = false;
    var transportClosed = false;
    var resolveDone;
    var rejectDone;
    var done = new Promise(function (resolve, reject) {
      resolveDone = resolve;
      rejectDone = reject;
    });

    function note(event) {
      result.events.push({ t: nowMs() - result.startedMs, event: event });
      window.__picoquicWebTransportProgress = result;
      if (options.onProgress) {
        options.onProgress(result);
      }
    }

    function isNetworkError(error) {
      return (error && error.name === "NetworkError") ||
        errorText(error).toLowerCase().indexOf("network error") >= 0;
    }

    function fail(error, mayBePostCloseReadError) {
      if (finished) {
        return;
      }
      if (sentZero && isSessionClosedError(error) && datagramRequirementMet()) {
        markClosed();
        return;
      }
      if (mayBePostCloseReadError && sentZero && datagramRequirementMet() &&
        isNetworkError(error)) {
        /* Safari 26.5 can reject pending WebTransport readers with NetworkError
         * during a clean peer-initiated close. Only treat that as benign after
         * the expected exchange is complete, and only if transport.closed also
         * fulfills.
         */
        transport.closed.then(markClosed, function (closeError) {
          fail(closeError, false);
        });
        return;
      }
      finished = true;
      result.error = errorText(error);
      note("error " + result.error);
      rejectDone(error);
    }

    function isSessionClosedError(error) {
      var text = errorText(error).toLowerCase();
      return text.indexOf("session is closed") >= 0 ||
        text.indexOf("transport is closed") >= 0;
    }

    function markClosed() {
      if (!transportClosed) {
        transportClosed = true;
        note("closed");
      }
      maybeDone();
    }

    function datagramRequirementMet() {
      return !requireDatagram || result.datagramsReceived.length > 0;
    }

    function maybeDone() {
      if (!finished && sentZero && transportClosed && datagramRequirementMet()) {
        finished = true;
        result.ok = true;
        result.closedMs = nowMs() - result.startedMs;
        resolveDone(result);
      }
    }

    function track(promise, mayBePostCloseReadError) {
      promise.catch(function (error) {
        fail(error, mayBePostCloseReadError === true);
      });
    }

    async function writePacket(writable, baton) {
      var writer = writable.getWriter();
      try {
        await writer.ready;
        await writer.write(makeBatonPacket(baton, 0));
        result.sent.push(baton);
        note("sent " + baton);
        if (baton === 0) {
          sentZero = true;
          maybeDone();
        }
        await writer.close();
      } finally {
        writer.releaseLock();
      }
    }

    async function openBidiAndReply(baton) {
      var next = (baton + 1) & 0xff;
      var stream = await transport.createBidirectionalStream();
      await writePacket(stream.writable, next);
      if (next !== 0) {
        track(handleBatonStream(stream.readable, "local-bidi"), true);
      }
    }

    async function openUniAndReply(baton) {
      var next = (baton + 1) & 0xff;
      var writable = await transport.createUnidirectionalStream();
      await writePacket(writable, next);
    }

    async function writeSameBidiAndReply(stream, baton) {
      var next = (baton + 1) & 0xff;
      await writePacket(stream.writable, next);
    }

    async function handleBatonStream(readable, mode, stream) {
      note("reading " + mode);
      var baton = await readBaton(readable, options.useByob !== false);
      result.received.push(baton);
      note("received " + baton + " on " + mode);
      if (baton === 0) {
        throw new Error("unexpected received zero baton");
      }
      if (mode === "remote-uni") {
        await openBidiAndReply(baton);
      } else if (mode === "remote-bidi") {
        await writeSameBidiAndReply(stream, baton);
      } else {
        await openUniAndReply(baton);
      }
    }

    async function acceptUnidirectional() {
      var reader = transport.incomingUnidirectionalStreams.getReader();
      while (true) {
        var incoming = await reader.read();
        if (incoming.done) {
          reader.releaseLock();
          return;
        }
        track(handleBatonStream(incoming.value, "remote-uni"), true);
      }
    }

    async function acceptBidirectional() {
      var reader = transport.incomingBidirectionalStreams.getReader();
      while (true) {
        var incoming = await reader.read();
        if (incoming.done) {
          reader.releaseLock();
          return;
        }
        track(handleBatonStream(incoming.value.readable, "remote-bidi", incoming.value), true);
      }
    }

    async function readDatagrams() {
      if (!transport.datagrams || !transport.datagrams.readable) {
        throw new Error("WebTransport datagrams are unavailable");
      }

      var reader = transport.datagrams.readable.getReader();
      while (true) {
        var read = await reader.read();
        if (read.done) {
          reader.releaseLock();
          return;
        }
        var decoder = new BatonDecoder(MAX_PACKET_BYTES);
        decoder.push(read.value);
        var baton = decoder.finish();
        result.datagramsReceived.push(baton);
        note("datagram " + baton);
        maybeDone();
      }
    }

    try {
      note("connecting");
      await transport.ready;
      result.readyMs = nowMs() - result.startedMs;
      result.protocol = transport.protocol || "";
      if (options.requireProtocol !== false &&
        options.protocol && result.protocol !== options.protocol) {
        throw new Error("unexpected WebTransport protocol '" + result.protocol + "'");
      }
      note("ready");

      transport.closed.then(markClosed, function (error) {
        fail(error, false);
      });

      track(acceptUnidirectional(), true);
      track(acceptBidirectional(), true);
      track(readDatagrams(), true);

      var datagramWritable = getDatagramWritable(transport.datagrams);
      if (datagramWritable) {
        var datagramWriter = datagramWritable.getWriter();
        try {
          await datagramWriter.ready;
          await datagramWriter.write(makeBatonPacket(42, 0));
        } finally {
          datagramWriter.releaseLock();
        }
        result.datagramsSent = 1;
        note("datagram sent");
      }

      return await withTimeout(done, timeoutMs, function () {
        try {
          transport.close({ closeCode: 1, reason: "timeout" });
        } catch (_) {}
      });
    } catch (error) {
      if (!result.error) {
        result.error = errorText(error);
        note("error " + result.error);
      }
      throw error;
    }
  }

  function setText(id, text) {
    var element = document.getElementById(id);
    if (element) {
      element.textContent = text;
    }
  }

  function render(result) {
    setText("received", String((result.received || []).length));
    setText("sent", String((result.sent || []).length));
    setText("datagrams", String((result.datagramsReceived || []).length));
    document.getElementById("log").textContent = JSON.stringify(result, null, 2);
  }

  function readOptionsFromPage() {
    var search = new URLSearchParams(window.location.search);
    return {
      url: document.getElementById("target").value,
      timeoutMs: Number(search.get("timeoutMs")) || DEFAULT_TIMEOUT_MS,
      certificateHash: search.get("certHash") || "",
      certificateHashAlgorithm: search.get("certHashAlg") || "sha-256",
      protocol: search.get("protocol") || DEFAULT_PROTOCOL,
      requireProtocol: search.get("requireProtocol") !== "0",
      requireDatagram: search.get("requireDatagram") !== "0",
      useByob: search.get("useByob") !== "0",
      onProgress: render
    };
  }

  async function runFromPage() {
    var button = document.getElementById("run");
    var root = document.documentElement;
    var options = readOptionsFromPage();
    button.disabled = true;
    root.dataset.result = "";
    setText("state", "running");

    try {
      var result = await runBatonTest(options);
      render(result);
      root.dataset.result = "pass";
      setText("state", "pass");
      return result;
    } catch (error) {
      var progress = window.__picoquicWebTransportProgress;
      var failed = {
        ok: false,
        url: options.url,
        requireDatagram: options.requireDatagram !== false,
        constructorRequireUnreliable: options.requireDatagram !== false,
        useByob: options.useByob !== false,
        received: [],
        sent: [],
        datagramsReceived: [],
        protocol: "",
        error: errorText(error)
      };
      if (progress && progress.url === options.url) {
        failed = progress;
        failed.ok = false;
        failed.error = failed.error || errorText(error);
      }
      render(failed);
      root.dataset.result = "fail";
      setText("state", "fail");
      return failed;
    } finally {
      button.disabled = false;
    }
  }

  function init() {
    var target = document.getElementById("target");
    var search = new URLSearchParams(window.location.search);
    target.value = search.get("url") || defaultTarget();

    document.getElementById("controls").addEventListener("submit", function (event) {
      event.preventDefault();
      window.__picoquicWebTransportResult = runFromPage();
    });

    if (hasAutoRun(search)) {
      window.__picoquicWebTransportResult = runFromPage();
    }
  }

  window.picoquicWebTransportBaton = {
    defaultTarget: defaultTarget,
    makeBatonPacket: makeBatonPacket,
    runProtocolConstructorTests: runProtocolConstructorTests,
    runUrlConstructorTests: runUrlConstructorTests,
    runOptionsConstructorTests: runOptionsConstructorTests,
    runDatagramWritableTests: runDatagramWritableTests,
    runStreamWritableTests: runStreamWritableTests,
    runBatonTest: runBatonTest
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init, { once: true });
  } else {
    init();
  }
}());
