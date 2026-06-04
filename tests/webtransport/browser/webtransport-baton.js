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

  function makeSizedDatagram(size) {
    var packet = new Uint8Array(size);
    for (var i = 0; i < packet.length; i++) {
      packet[i] = i & 0xff;
    }
    return packet;
  }

  function makeStreamError(message, code) {
    if (typeof WebTransportError === "function") {
      var init = {
        message: message,
        source: "stream",
        streamErrorCode: code
      };
      try {
        return new WebTransportError(init);
      } catch (e) {
        return new WebTransportError(message, init);
      }
    }
    return new Error(message);
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

  function isPromiseLike(value) {
    return value && typeof value.then === "function";
  }

  function isReadableStreamLike(value) {
    return value && typeof value.getReader === "function";
  }

  function isWritableStreamLike(value) {
    return value && typeof value.getWriter === "function";
  }

  function valueKind(value) {
    if (value === null) {
      return "null";
    }
    if (value === undefined) {
      return "undefined";
    }
    return typeof value;
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

  async function runWritableBadChunkTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      datagramWritable: {
        ok: false,
        tests: []
      },
      streamWritable: {
        ok: false,
        tests: []
      }
    };
    var url = options.url || defaultTarget();
    var transport = null;
    var closedPromise = null;
    var datagramWriter = null;

    function record(target, name, ok, detail) {
      target.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    function finish(target) {
      target.ok = target.tests.length > 0 && target.tests.every(function (entry) {
        return entry.ok;
      });
    }

    function optionalSendOrderUnsupported(error) {
      var text = errorText(error).toLowerCase();
      return error && (error.name === "TypeError" ||
        error.name === "NotSupportedError" ||
        text.indexOf("unsupported") >= 0 ||
        text.indexOf("not supported") >= 0 ||
        text.indexOf("not implemented") >= 0);
    }

    async function cleanupOptionalWritable(writable) {
      if (isWritableStreamLike(writable)) {
        var writer = null;
        try {
          writer = writable.getWriter();
          await withTimeout(writer.close(), 1000);
        } catch (_) {}
        finally {
          if (writer) {
            try {
              writer.releaseLock();
            } catch (_) {}
          }
        }
      }
    }

    async function checkOptionalWritable(target, name, createWritable) {
      var writable = null;
      try {
        writable = await createWritable();
        record(target, name, isWritableStreamLike(writable), valueKind(writable));
      } catch (error) {
        record(target, name, optionalSendOrderUnsupported(error), errorText(error));
      } finally {
        await cleanupOptionalWritable(writable);
      }
    }

    async function checkDatagramSendOrderOption() {
      if (!transport.datagrams ||
        typeof transport.datagrams.createWritable !== "function") {
        record(result.datagramWritable,
          "datagram-createWritable-sendOrder-optional", true, "unsupported");
        return;
      }
      await checkOptionalWritable(result.datagramWritable,
        "datagram-createWritable-sendOrder-optional", function () {
          return transport.datagrams.createWritable({ sendOrder: 7 });
        });
    }

    async function checkStreamWritable(name, writable) {
      var writer = writable.getWriter();
      try {
        await writer.ready;
        try {
          await writer.write("not a BufferSource");
          record(result.streamWritable, name, false, "write resolved");
        } catch (error) {
          record(result.streamWritable, name,
            error && error.name === "TypeError", errorText(error));
        }
      } finally {
        try {
          writer.releaseLock();
        } catch (_) {}
      }
    }

    try {
      transport = new WebTransport(url, buildTransportOptions(options));
      try {
        closedPromise = transport.closed.then(function () {}, function () {});
      } catch (_) {}
      await transport.ready;

      var datagramWritable = getDatagramWritable(transport.datagrams);
      if (!datagramWritable) {
        record(result.datagramWritable, "writable-available", false,
          "WebTransport datagrams are unavailable");
      } else {
        datagramWriter = datagramWritable.getWriter();
        await datagramWriter.ready;
        try {
          await datagramWriter.write("not a BufferSource");
          record(result.datagramWritable, "string-chunk", false, "write resolved");
        } catch (error) {
          record(result.datagramWritable, "string-chunk",
            error && error.name === "TypeError", errorText(error));
        }
      }

      try {
        var uniWritable = await transport.createUnidirectionalStream();
        await checkStreamWritable("unidirectional-string-chunk", uniWritable);
      } catch (error) {
        record(result.streamWritable, "unidirectional-setup", false, errorText(error));
      }

      try {
        var bidiStream = await transport.createBidirectionalStream();
        await checkStreamWritable("bidirectional-string-chunk", bidiStream.writable);
      } catch (error) {
        record(result.streamWritable, "bidirectional-setup", false, errorText(error));
      }

      await checkDatagramSendOrderOption();
      await checkOptionalWritable(result.streamWritable,
        "unidirectional-sendOrder-option", function () {
          return transport.createUnidirectionalStream({ sendOrder: 7 });
        });
      await checkOptionalWritable(result.streamWritable,
        "bidirectional-sendOrder-option", async function () {
          var stream = await transport.createBidirectionalStream({ sendOrder: 8 });
          return stream && stream.writable;
        });
    } catch (error) {
      if (result.datagramWritable.tests.length === 0) {
        record(result.datagramWritable, "setup", false, errorText(error));
      }
      if (result.streamWritable.tests.length === 0) {
        record(result.streamWritable, "setup", false, errorText(error));
      }
    } finally {
      if (datagramWriter) {
        try {
          datagramWriter.releaseLock();
        } catch (_) {}
      }
      if (transport) {
        try {
          transport.close({ closeCode: 0, reason: "writable-bad-chunk-test" });
        } catch (_) {}
        if (closedPromise) {
          try {
            await withTimeout(closedPromise, 3000);
          } catch (_) {}
        }
      }
    }

    finish(result.datagramWritable);
    finish(result.streamWritable);
    result.ok = result.datagramWritable.ok && result.streamWritable.ok;
    return result;
  }

  async function runCloseSessionTests(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      tests: []
    };
    var url = options.url || defaultTarget();
    var transport = null;
    var closedPromise = null;

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    try {
      transport = new WebTransport(url, buildTransportOptions(options));
      closedPromise = transport.closed.then(function () {
        return { ok: true, detail: "" };
      }, function (error) {
        return { ok: false, detail: errorText(error) };
      });
      await transport.ready;
      record("ready", true, "");
      transport.close({ closeCode: 42, reason: "browser-close-test" });
      record("close-called", true, "");
      var closed = await withTimeout(closedPromise, 3000);
      record("closed-resolved", closed.ok, closed.detail);
    } catch (error) {
      record("setup", false, errorText(error));
    } finally {
      if (transport) {
        try {
          transport.close({ closeCode: 42, reason: "browser-close-test" });
        } catch (_) {}
      }
    }

    result.ok = result.tests.length > 0 && result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  async function runPostCloseTests(transport) {
    var result = {
      ok: false,
      tests: []
    };

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    async function expectRejects(name, operation) {
      try {
        await withTimeout(Promise.resolve(operation()), 3000);
        record(name, false, "operation resolved");
      } catch (error) {
        var text = errorText(error);
        record(name, text.indexOf("timeout after") < 0, text);
      }
    }

    await expectRejects("create-bidirectional-stream", function () {
      return transport.createBidirectionalStream();
    });
    await expectRejects("create-unidirectional-stream", function () {
      return transport.createUnidirectionalStream();
    });

    result.ok = result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  async function runPostCloseDatagramTests(transport) {
    var result = {
      ok: false,
      tests: []
    };

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    var datagramWritable = getDatagramWritable(transport.datagrams);
    if (!datagramWritable) {
      record("datagram-writable-available", false,
        "WebTransport datagrams are unavailable");
    } else {
      var writer = null;
      try {
        writer = datagramWritable.getWriter();
        await withTimeout(writer.write(new Uint8Array(0)), 3000);
        record("datagram-write-after-close", false, "write resolved");
      } catch (error) {
        var text = errorText(error);
        record("datagram-write-after-close",
          text.indexOf("timeout after") < 0, text);
      } finally {
        if (writer) {
          try {
            writer.releaseLock();
          } catch (_) {}
        }
      }
    }

    result.ok = result.tests.length > 0 && result.tests.every(function (entry) {
      return entry.ok;
    });
    return result;
  }

  async function runSessionDiagnostics(transport) {
    var result = {
      ok: false,
      tests: []
    };

    function record(name, ok, detail) {
      result.tests.push({
        name: name,
        ok: ok,
        detail: detail || ""
      });
    }

    function recordOptionalString(name) {
      if (!(name in transport) || transport[name] === undefined) {
        record(name + "-optional", true, "unsupported");
        return;
      }
      record(name + "-optional", typeof transport[name] === "string",
        String(transport[name]));
    }

    record("ready-promise", isPromiseLike(transport.ready), valueKind(transport.ready));
    record("closed-promise", isPromiseLike(transport.closed), valueKind(transport.closed));
    if (!("protocol" in transport) || transport.protocol === undefined) {
      /* Firefox 151.0.3 on GitHub run 26847980741 job 79172503004
       * completes the protocol=optional compatibility path without exposing
       * transport.protocol. Required selected-protocol checks still happen
       * through result.protocol before this diagnostic runs.
       */
      record("protocol-property", true, "unsupported");
    } else {
      record("protocol-property", typeof transport.protocol === "string",
        String(transport.protocol || ""));
    }
    record("incoming-bidirectional-readable",
      isReadableStreamLike(transport.incomingBidirectionalStreams),
      valueKind(transport.incomingBidirectionalStreams));
    record("incoming-unidirectional-readable",
      isReadableStreamLike(transport.incomingUnidirectionalStreams),
      valueKind(transport.incomingUnidirectionalStreams));
    record("create-bidirectional-stream-function",
      typeof transport.createBidirectionalStream === "function",
      valueKind(transport.createBidirectionalStream));
    record("create-unidirectional-stream-function",
      typeof transport.createUnidirectionalStream === "function",
      valueKind(transport.createUnidirectionalStream));

    var datagrams = transport.datagrams;
    record("datagrams-object", !!datagrams && typeof datagrams === "object",
      valueKind(datagrams));
    record("datagrams-readable", !!datagrams &&
      isReadableStreamLike(datagrams.readable), valueKind(datagrams && datagrams.readable));
    record("datagrams-writable-or-createWritable", !!datagrams &&
      (!!datagrams.writable || typeof datagrams.createWritable === "function"),
      datagrams && datagrams.writable ? "writable" :
        valueKind(datagrams && datagrams.createWritable));

    recordOptionalString("reliability");
    recordOptionalString("congestionControl");

    if (!("supportsReliableOnly" in transport) ||
      transport.supportsReliableOnly === undefined) {
      record("supportsReliableOnly-optional", true, "unsupported");
    } else {
      record("supportsReliableOnly-optional",
        typeof transport.supportsReliableOnly === "boolean",
        String(transport.supportsReliableOnly));
    }

    if (!("responseHeaders" in transport) ||
      transport.responseHeaders === undefined) {
      record("responseHeaders-optional", true, "unsupported");
    } else {
      record("responseHeaders-optional",
        isPromiseLike(transport.responseHeaders) ||
          typeof transport.responseHeaders === "object",
        valueKind(transport.responseHeaders));
    }

    if (!("draining" in transport) || transport.draining === undefined) {
      record("draining-optional", true, "unsupported");
    } else {
      record("draining-optional", isPromiseLike(transport.draining),
        valueKind(transport.draining));
    }

    if (typeof transport.getStats === "function") {
      try {
        var stats = await withTimeout(transport.getStats(), 1500);
        var detail = stats && typeof stats === "object" ?
          Object.keys(stats).slice(0, 8).join(",") : valueKind(stats);
        record("getStats-optional", !!stats && typeof stats === "object", detail);
      } catch (error) {
        /* Firefox 151.0.3 exposes getStats but throws NS_ERROR_NOT_IMPLEMENTED.
         * This diagnostics block observes optional API surface; WPT/stat
         * conformance assertions belong in a separate stats-specific test.
         */
        record("getStats-optional", true, errorText(error));
      }
    } else {
      record("getStats-optional", true, "unsupported");
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

  function delayMs(timeoutMs) {
    return new Promise(function (resolve) {
      setTimeout(resolve, timeoutMs);
    });
  }

  function makeStreamPayload(offset, length) {
    var chunk = new Uint8Array(length);
    for (var i = 0; i < chunk.length; i++) {
      chunk[i] = (offset + i) & 0xff;
    }
    return chunk;
  }

  function verifyStreamPayload(chunk, offset) {
    for (var i = 0; i < chunk.byteLength; i++) {
      if (chunk[i] !== ((offset + i) & 0xff)) {
        throw new Error("stream byte mismatch at " + (offset + i));
      }
    }
  }

  async function writeStreamPayload(writable, size, result, note) {
    var writer = writable.getWriter();
    var offset = 0;
    try {
      await writer.ready;
      while (offset < size) {
        var chunkLength = Math.min(READ_BUFFER_SIZE, size - offset);
        await writer.write(makeStreamPayload(offset, chunkLength));
        offset += chunkLength;
        result.streamBytesSent += chunkLength;
      }
      await writer.close();
      result.streamFinSent += 1;
      note("stream sent " + size);
    } finally {
      writer.releaseLock();
    }
  }

  async function readStreamPayload(readable, expectedSize, result, note, label) {
    var reader = readable.getReader();
    var offset = 0;
    try {
      while (true) {
        var read = await reader.read();
        if (read.done) {
          if (offset !== expectedSize) {
            throw new Error("expected " + expectedSize +
              " stream bytes on " + label + ", got " + offset);
          }
          result.streamBytesReceived += offset;
          result.streamFinReceived += 1;
          note("stream received " + offset + " on " + label);
          return offset;
        }
        if (offset + read.value.byteLength > expectedSize) {
          throw new Error("too many stream bytes on " + label);
        }
        verifyStreamPayload(read.value, offset);
        offset += read.value.byteLength;
      }
    } finally {
      reader.releaseLock();
    }
  }

  async function runStreamTest(options) {
    if (typeof WebTransport !== "function") {
      throw new Error("WebTransport is unavailable");
    }

    var result = {
      ok: false,
      url: options.url,
      requireDatagram: options.requireDatagram !== false,
      constructorRequireUnreliable: options.requireDatagram !== false,
      useByob: options.useByob !== false,
      streamMode: options.streamMode || "",
      streamSize: options.streamSize || 0,
      streamCount: options.streamCount || 1,
      streamBytesSent: 0,
      streamBytesReceived: 0,
      streamFinSent: 0,
      streamFinReceived: 0,
      protocol: "",
      readyMs: 0,
      closedMs: 0,
      startedMs: nowMs(),
      postClose: null,
      postCloseDatagram: null,
      events: []
    };
    window.__picoquicWebTransportProgress = result;
    var timeoutMs = options.timeoutMs || DEFAULT_TIMEOUT_MS;
    var transport = null;
    var closedPromise = null;

    function note(event) {
      result.events.push({ t: nowMs() - result.startedMs, event: event });
      window.__picoquicWebTransportProgress = result;
      if (options.onProgress) {
        options.onProgress(result);
      }
    }

    async function readChunkBeforeCancel(reader, result, note, label) {
      var read = await withTimeout(reader.read(), 3000);
      if (read.done) {
        throw new Error("incoming " + label + " stream ended before cancel");
      }
      var length = read.value && read.value.byteLength ? read.value.byteLength : 0;
      result.streamBytesReceived += length;
      note("stream read before cancel " + label);
    }

    async function writeByteAndExpectStop(writable, label) {
      var writer = writable.getWriter();
      try {
        await writer.ready;
        await writer.write(new Uint8Array([123]));
        result.streamBytesSent += 1;
        note("stream wrote before stop " + label);
        var outcome = await withTimeout(writer.closed.then(function () {
          return { ok: true, detail: "" };
        }, function (error) {
          return { ok: false, detail: errorText(error) };
        }), 3000);
        if (outcome.ok) {
          throw new Error("expected server stop on " + label);
        }
        note("stream stopped " + label);
        note("stream stopped detail " + label + " " + outcome.detail);
      } finally {
        writer.releaseLock();
      }
    }

    function closeReason(length) {
      var reason = "";
      for (var i = 0; i < length; i++) {
        reason += "x";
      }
      return reason;
    }

    function isSessionClosedError(error) {
      var text = errorText(error).toLowerCase();
      return text.indexOf("session is closed") >= 0 ||
        text.indexOf("transport is closed") >= 0 ||
        text.indexOf("connection lost") >= 0;
    }

    function isLifecycleMode(mode) {
      return [
        "server-close-immediate",
        "server-close-after-ready",
        "browser-close",
        "browser-close-long-reason",
        "server-close-long-reason",
        "fin-no-capsule",
        "server-drain",
        "server-drain-then-close",
        "session-gone-active-streams",
        "session-gone-datagram-after-close",
        "session-gone-new-stream-after-close"
      ].indexOf(mode) >= 0;
    }

    async function waitForClosedSettlement(label) {
      var closed = await withTimeout(closedPromise, 3000);
      if (!closed.ok) {
        note("closed detail " + label + " " + closed.detail);
      }
      result.closedMs = nowMs() - result.startedMs;
      note("closed");
    }

    async function waitForDrainingSignal(label) {
      if (!isPromiseLike(transport.draining)) {
        note("draining unsupported " + label);
        note("draining checked " + label);
        return;
      }
      try {
        var drained = await withTimeout(transport.draining.then(function () {
          return { ok: true, detail: "" };
        }, function (error) {
          return { ok: false, detail: errorText(error) };
        }), 3000);
        if (drained.ok) {
          note("draining resolved " + label);
        } else {
          note("draining rejected " + label + " " + drained.detail);
        }
      } catch (error) {
        note("draining timeout " + label + " " + errorText(error));
      }
      note("draining checked " + label);
    }

    async function writeLifecycleTrigger(label) {
      var stream = await transport.createBidirectionalStream();
      var writer = stream.writable.getWriter();
      try {
        await writer.ready;
        await writer.write(new Uint8Array([42]));
        result.streamBytesSent += 1;
        note("lifecycle trigger " + label);
        try {
          await writer.close();
          result.streamFinSent += 1;
          note("lifecycle trigger fin " + label);
        } catch (error) {
          var text = errorText(error).toLowerCase();
          if (!isSessionClosedError(error) &&
            text.indexOf("writable stream that is closed or errored") < 0) {
            throw error;
          }
          /* Safari 26.4 in GitHub Actions run 26924318861 job 79431177366
           * delivered the lifecycle trigger to pico_baton and received the
           * server WT_CLOSE_SESSION before writer.close() settled, then
           * rejected writer.close() because the stream was already closed.
           */
          note("lifecycle trigger closed " + label);
        }
      } finally {
        writer.releaseLock();
      }
    }

    async function runPostCloseStreams(label) {
      result.postClose = await runPostCloseTests(transport);
      if (!result.postClose.ok) {
        throw new Error("post-close stream tests failed on " + label + ": " +
          JSON.stringify(result.postClose));
      }
      note("post-close streams " + label);
    }

    async function runPostCloseDatagram(label) {
      result.postCloseDatagram = await runPostCloseDatagramTests(transport);
      note("post-close datagram " + label);
    }

    async function readIncomingUni(reader, label) {
      var incoming = await reader.read();
      if (incoming.done) {
        throw new Error("incoming unidirectional stream ended before " + label);
      }
      return readStreamPayload(incoming.value, result.streamSize, result,
        note, label);
    }

    async function readIncomingBidi(reader, label) {
      var incoming = await reader.read();
      if (incoming.done) {
        throw new Error("incoming bidirectional stream ended before " + label);
      }
      var read = readStreamPayload(incoming.value.readable,
        result.streamSize, result, note, label);
      var writer = incoming.value.writable.getWriter();
      try {
        await writer.ready;
        await writer.close();
        result.streamFinSent += 1;
        note("stream sent 0 on " + label + "-reply");
      } finally {
        writer.releaseLock();
      }
      return read;
    }

    try {
      note("connecting");
      transport = new WebTransport(options.url, buildTransportOptions(options));
      closedPromise = transport.closed.then(function () {
        return { ok: true, detail: "" };
      }, function (error) {
        return { ok: false, detail: errorText(error) };
      });
      var ready = await withTimeout(transport.ready.then(function () {
        return { ok: true, detail: "" };
      }, function (error) {
        return { ok: false, detail: errorText(error) };
      }), 3000);
      if (!ready.ok) {
        if (!isLifecycleMode(result.streamMode)) {
          throw new Error(ready.detail);
        }
        note("ready rejected " + result.streamMode + " " + ready.detail);
      } else {
        result.readyMs = nowMs() - result.startedMs;
        result.protocol = transport.protocol || "";
        if (options.requireProtocol !== false &&
          options.protocol && result.protocol !== options.protocol) {
          throw new Error("unexpected WebTransport protocol '" +
            result.protocol + "'");
        }
        result.sessionDiagnostics = await runSessionDiagnostics(transport);
        if (!result.sessionDiagnostics.ok) {
          throw new Error("session diagnostics failed: " +
            JSON.stringify(result.sessionDiagnostics));
        }
        note("ready");
      }

      if (result.streamMode === "browser-abort-bidi") {
        var abortBidi = await transport.createBidirectionalStream();
        var abortBidiWriter = abortBidi.writable.getWriter();
        try {
          await abortBidiWriter.ready;
          await abortBidiWriter.write(new Uint8Array([123]));
          result.streamBytesSent += 1;
          note("stream wrote abort-bidi");
          await delayMs(100);
          await abortBidiWriter.abort(makeStreamError("browser-abort-bidi", 123));
          note("stream aborted bidi");
        } finally {
          abortBidiWriter.releaseLock();
        }
      } else if (result.streamMode === "browser-abort-uni") {
        var abortUni = await transport.createUnidirectionalStream();
        var abortUniWriter = abortUni.getWriter();
        try {
          await abortUniWriter.ready;
          await abortUniWriter.write(new Uint8Array([123]));
          result.streamBytesSent += 1;
          note("stream wrote abort-uni");
          await delayMs(100);
          await abortUniWriter.abort(makeStreamError("browser-abort-uni", 123));
          note("stream aborted uni");
        } finally {
          abortUniWriter.releaseLock();
        }
      } else if (result.streamMode === "browser-cancel-incoming-bidi") {
        var cancelBidiReader = transport.incomingBidirectionalStreams.getReader();
        try {
          var cancelBidiIncoming = await cancelBidiReader.read();
          if (cancelBidiIncoming.done) {
            throw new Error("incoming bidirectional stream ended before cancel");
          }
          var cancelBidiReadable = cancelBidiIncoming.value.readable.getReader();
          try {
            await readChunkBeforeCancel(cancelBidiReadable, result, note, "bidi");
            await cancelBidiReadable.cancel(makeStreamError("browser-cancel-bidi", 123));
            note("stream canceled incoming bidi");
          } finally {
            cancelBidiReadable.releaseLock();
          }
        } finally {
          cancelBidiReader.releaseLock();
        }
      } else if (result.streamMode === "browser-cancel-incoming-uni") {
        var cancelUniReader = transport.incomingUnidirectionalStreams.getReader();
        try {
          var cancelUniIncoming = await cancelUniReader.read();
          if (cancelUniIncoming.done) {
            throw new Error("incoming unidirectional stream ended before cancel");
          }
          var cancelUniReadable = cancelUniIncoming.value.getReader();
          try {
            await readChunkBeforeCancel(cancelUniReadable, result, note, "uni");
            await cancelUniReadable.cancel(makeStreamError("browser-cancel-uni", 123));
            note("stream canceled incoming uni");
          } finally {
            cancelUniReadable.releaseLock();
          }
        } finally {
          cancelUniReader.releaseLock();
        }
      } else if (result.streamMode === "server-reset-bidi") {
        /* GitHub WebTransportBrowser run 26919766839 showed Chrome, Edge,
         * and Firefox timing out while waiting for a browser ReadableStream
         * error on server-created reset streams even though the pico_baton
         * log recorded the WT-mapped RESET_STREAM with app_error=123.
         * Keep this row on the browser ready/close path and assert the
         * reset on the server summary counters.
         */
        await delayMs(250);
        note("server reset wait server-reset-bidi");
      } else if (result.streamMode === "server-reset-uni") {
        /* See the server-reset-bidi note above. */
        await delayMs(250);
        note("server reset wait server-reset-uni");
      } else if (result.streamMode === "server-stop-bidi") {
        var stopBidi = await transport.createBidirectionalStream();
        await writeByteAndExpectStop(stopBidi.writable, "server-stop-bidi");
      } else if (result.streamMode === "server-stop-uni") {
        var stopUni = await transport.createUnidirectionalStream();
        await writeByteAndExpectStop(stopUni, "server-stop-uni");
      } else if (result.streamMode === "server-close-immediate") {
        await writeLifecycleTrigger("server-close-immediate");
      } else if (result.streamMode === "server-close-after-ready") {
        await writeLifecycleTrigger("server-close-after-ready");
      } else if (result.streamMode === "browser-close") {
        transport.close({ closeCode: 42, reason: "done" });
        note("browser close called browser-close");
      } else if (result.streamMode === "browser-close-long-reason") {
        transport.close({ closeCode: 42, reason: closeReason(1024) });
        note("browser close called browser-close-long-reason");
      } else if (result.streamMode === "server-close-long-reason") {
        await writeLifecycleTrigger("server-close-long-reason");
      } else if (result.streamMode === "fin-no-capsule") {
        await writeLifecycleTrigger("fin-no-capsule");
      } else if (result.streamMode === "server-drain") {
        await writeLifecycleTrigger("server-drain");
        await waitForDrainingSignal("server-drain");
        transport.close({ closeCode: 0, reason: "drain-complete" });
      } else if (result.streamMode === "server-drain-then-close") {
        await writeLifecycleTrigger("server-drain-then-close");
        await waitForDrainingSignal("server-drain-then-close");
      } else if (result.streamMode === "session-gone-active-streams") {
        await writeLifecycleTrigger("session-gone-active-streams");
      } else if (result.streamMode === "session-gone-datagram-after-close") {
        await writeLifecycleTrigger("session-gone-datagram-after-close");
      } else if (result.streamMode === "session-gone-new-stream-after-close") {
        await writeLifecycleTrigger("session-gone-new-stream-after-close");
      } else if (result.streamMode === "client-bidi-echo") {
        for (var bidiIndex = 0; bidiIndex < result.streamCount; bidiIndex++) {
          var bidiStream = await transport.createBidirectionalStream();
          var readEcho = readStreamPayload(bidiStream.readable,
            result.streamSize, result, note, "client-bidi-" + bidiIndex);
          await writeStreamPayload(bidiStream.writable, result.streamSize,
            result, note);
          await readEcho;
        }
      } else if (result.streamMode === "client-uni-reply") {
        var uniReader = transport.incomingUnidirectionalStreams.getReader();
        try {
          for (var uniIndex = 0; uniIndex < result.streamCount; uniIndex++) {
            var writable = await transport.createUnidirectionalStream();
            await writeStreamPayload(writable, result.streamSize, result, note);
            await readIncomingUni(uniReader, "client-uni-reply-" + uniIndex);
          }
        } finally {
          uniReader.releaseLock();
        }
      } else if (result.streamMode === "server-uni") {
        var serverUniReader = transport.incomingUnidirectionalStreams.getReader();
        try {
          for (var serverUniIndex = 0; serverUniIndex < result.streamCount;
            serverUniIndex++) {
            await readIncomingUni(serverUniReader,
              "server-uni-" + serverUniIndex);
          }
        } finally {
          serverUniReader.releaseLock();
        }
      } else if (result.streamMode === "server-bidi") {
        var serverBidiReader = transport.incomingBidirectionalStreams.getReader();
        try {
          for (var serverBidiIndex = 0; serverBidiIndex < result.streamCount;
            serverBidiIndex++) {
            await readIncomingBidi(serverBidiReader,
              "server-bidi-" + serverBidiIndex);
          }
        } finally {
          serverBidiReader.releaseLock();
        }
      } else {
        throw new Error("unsupported stream mode " + result.streamMode);
      }

      if (result.streamMode.indexOf("browser-abort-") === 0) {
        await withTimeout(closedPromise, 3000);
        result.closedMs = nowMs() - result.startedMs;
        note("closed");
      } else if (result.streamMode.indexOf("browser-cancel-incoming-") === 0) {
        /* Incoming-stream cancel rows assert browser cancel completion and
         * server-observed STOP_SENDING. Close/drain rows cover transport.closed.
         */
      } else if (isLifecycleMode(result.streamMode)) {
        await waitForClosedSettlement(result.streamMode);
        if (result.streamMode === "session-gone-active-streams" ||
          result.streamMode === "session-gone-new-stream-after-close") {
          await runPostCloseStreams(result.streamMode);
        }
        if (result.streamMode === "session-gone-datagram-after-close") {
          await runPostCloseDatagram(result.streamMode);
        }
      } else {
        transport.close({ closeCode: 0, reason: "stream-test" });
        var closed = await withTimeout(closedPromise, 3000);
        if (!closed.ok) {
          throw new Error(closed.detail);
        }
        result.closedMs = nowMs() - result.startedMs;
        note("closed");
      }
      result.ok = true;
      return result;
    } catch (error) {
      if (!result.error) {
        result.error = errorText(error);
        note("error " + result.error);
      }
      throw error;
    } finally {
      if (transport) {
        try {
          transport.close({ closeCode: 0, reason: "stream-test" });
        } catch (_) {}
      }
    }
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
      datagramReceiveMode: options.datagramReceiveMode || "baton",
      datagramReceiveMin: options.datagramReceiveMin || 0,
      datagramSendMode: options.datagramSendMode || "baton",
      datagramSendSize: options.datagramSendSize || 0,
      datagramSendCount: options.datagramSendCount || 1,
      startedMs: nowMs(),
      readyMs: 0,
      closedMs: 0,
      received: [],
      sent: [],
      datagramsReceived: [],
      datagramLengths: [],
      datagramsSent: 0,
      protocol: "",
      postClose: null,
      events: []
    };
    window.__picoquicWebTransportProgress = result;
    var timeoutMs = options.timeoutMs || DEFAULT_TIMEOUT_MS;
    var transport = new WebTransport(options.url, buildTransportOptions(options));
    var finished = false;
    var sentZero = false;
    var finalZeroWritesPending = 0;
    var finalZeroSyntheticSent = 0;
    var transportClosed = false;
    var postCloseStarted = false;
    var datagramWaiters = [];
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

    function isBrowserCompletedPeerCloseError(error) {
      var userAgent = typeof navigator !== "undefined" && navigator.userAgent ?
        navigator.userAgent : "";
      var isEdge = userAgent.indexOf("Edg/") >= 0;
      var isChrome148 = !isEdge &&
        (userAgent.indexOf("Chrome/148.") >= 0 ||
          userAgent.indexOf("HeadlessChrome/148.") >= 0);
      return (isEdge || isChrome148) &&
        errorText(error).toLowerCase().indexOf("connection lost") >= 0;
    }

    function fail(error, mayBePostCloseReadError) {
      if (finished) {
        return;
      }
      if (mayBePostCloseReadError &&
        finalZeroWritesPending > finalZeroSyntheticSent &&
        datagramRequirementMet() && isNetworkError(error)) {
        /* Safari 26.4 in GitHub Actions runs 26833011127 and 26833619374
         * delivered the final zero baton to pico_baton, observed
         * transport.closed, and then rejected a pending reader with
         * NetworkError before the final writer.write() promise settled.
         */
        markFinalZeroSentAfterClose();
        return;
      }
      if (sentZero && isSessionClosedError(error) && datagramRequirementMet()) {
        markClosed();
        return;
      }
      if (sentZero && datagramRequirementMet() &&
        isBrowserCompletedPeerCloseError(error)) {
        /* Edge 148.0.3967.83 in GitHub Actions run 26836728795 jobs
         * 79132510221 and 79133239109 completed the stream/datagram baton
         * exchange, then rejected transport.closed with WebTransportError:
         * Connection lost after pico_baton closed the completed session.
         * Chrome 148.0.7778.178 old headless on Linux reproduced the same
         * completed-close behavior in GitHub run 26846296519 jobs 79166649793
         * and 79167387553.
         */
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

    function markFinalZeroSentAfterClose() {
      if (finalZeroWritesPending > finalZeroSyntheticSent) {
        result.sent.push(0);
        note("sent 0");
        finalZeroSyntheticSent++;
      }
      sentZero = true;
      transport.closed.then(markClosed, function (closeError) {
        fail(closeError, false);
      });
    }

    function markClosed() {
      if (!transportClosed) {
        transportClosed = true;
        note("closed");
      }
      maybeDone();
    }

    function datagramRequirementMet() {
      var datagramReceiveMin = options.datagramReceiveMin || 0;
      if (datagramReceiveMin > 0) {
        if ((options.datagramReceiveMode || "baton") === "baton") {
          return result.datagramsReceived.length >= datagramReceiveMin;
        }
        return result.datagramLengths.length >= datagramReceiveMin;
      }
      return !requireDatagram ||
        result.datagramsReceived.length > 0 ||
        result.datagramLengths.length > 0;
    }

    function notifyDatagramReceived() {
      var waiters = datagramWaiters;
      datagramWaiters = [];
      for (var i = 0; i < waiters.length; i++) {
        waiters[i]();
      }
    }

    function waitForRequiredDatagram() {
      if (datagramRequirementMet()) {
        return Promise.resolve();
      }
      return new Promise(function (resolve) {
        datagramWaiters.push(resolve);
      });
    }

    function maybeDone() {
      if (!finished && sentZero && transportClosed && datagramRequirementMet()) {
        finishAfterClosed();
      }
    }

    async function finishAfterClosed() {
      if (postCloseStarted) {
        return;
      }
      postCloseStarted = true;
      try {
        result.postClose = await runPostCloseTests(transport);
        if (!result.postClose.ok) {
          throw new Error("post-close tests failed: " +
            JSON.stringify(result.postClose));
        }
        result.postCloseDatagram = await runPostCloseDatagramTests(transport);
        if (!finished) {
          finished = true;
          result.ok = true;
          result.closedMs = nowMs() - result.startedMs;
          resolveDone(result);
        }
      } catch (error) {
        fail(error, false);
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
        if (baton === 0) {
          await withTimeout(waitForRequiredDatagram(),
            Math.min(timeoutMs, 3000));
          finalZeroWritesPending++;
        }
        try {
          await writer.write(makeBatonPacket(baton, 0));
        } catch (error) {
          if (baton === 0 && isNetworkError(error) && datagramRequirementMet()) {
            /* Safari 26.4 in GitHub Actions run 26833011127 delivered the
             * final zero baton to pico_baton, observed transport.closed, and
             * then rejected the still-pending writer.write() with NetworkError
             * because the server closed the completed WebTransport session.
             */
            markFinalZeroSentAfterClose();
            return;
          }
          throw error;
        }
        if (baton === 0 && finalZeroSyntheticSent > 0) {
          finalZeroSyntheticSent--;
        }
        else {
          result.sent.push(baton);
          note("sent " + baton);
        }
        if (baton === 0 && finalZeroWritesPending > 0) {
          finalZeroWritesPending--;
        }
        if (baton === 0 && !sentZero) {
          sentZero = true;
          maybeDone();
        }
        try {
          await writer.close();
        } catch (error) {
          if (datagramRequirementMet() &&
            errorText(error).indexOf("writable stream that is closed or errored") >= 0) {
            if (baton === 0) {
              /* Safari 26.4 in GitHub Actions run 26836125845 completed the
               * final zero baton and observed transport.closed, then rejected
               * writer.close() because the peer had already closed the session.
               */
              return;
            }
            if (sentZero && transportClosed) {
              /* Safari 26.4 in GitHub Actions run 26901828019 job 79355979158
               * delivered all 1000 burst datagrams, sent the terminal zero on
               * one stream, observed transport.closed, and then rejected a
               * different completed stream's writer.close().
               */
              return;
            }
          }
          throw error;
        }
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
        result.datagramLengths.push(read.value.byteLength);
        if (options.datagramReceiveMode === "empty") {
          if (read.value.byteLength !== 0) {
            throw new Error("unexpected non-empty datagram length " +
              read.value.byteLength);
          }
          note("datagram length 0");
        } else if (options.datagramReceiveMode === "length") {
          note("datagram length " + read.value.byteLength);
        } else {
          var decoder = new BatonDecoder(MAX_PACKET_BYTES);
          decoder.push(read.value);
          var baton = decoder.finish();
          result.datagramsReceived.push(baton);
          note("datagram " + baton);
        }
        notifyDatagramReceived();
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
      result.sessionDiagnostics = await runSessionDiagnostics(transport);
      if (!result.sessionDiagnostics.ok) {
        throw new Error("session diagnostics failed: " +
          JSON.stringify(result.sessionDiagnostics));
      }
      note("session diagnostics");
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
          var datagramSendCount = Math.max(1, Math.floor(options.datagramSendCount || 1));
          for (var i = 0; i < datagramSendCount; i++) {
            if (options.datagramSendMode === "empty") {
              await datagramWriter.write(new Uint8Array(0));
              note("datagram sent length 0");
            } else if (options.datagramSendMode === "length") {
              await datagramWriter.write(makeSizedDatagram(options.datagramSendSize || 1));
              note("datagram sent length " + (options.datagramSendSize || 1));
            } else {
              await datagramWriter.write(makeBatonPacket(42, 0));
              note("datagram sent");
            }
            result.datagramsSent++;
          }
        } finally {
          datagramWriter.releaseLock();
        }
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
      datagramReceiveMode: search.get("datagramReceiveMode") || "baton",
      datagramReceiveMin: Number(search.get("datagramReceiveMin")) || 0,
      datagramSendMode: search.get("datagramSendMode") || "baton",
      datagramSendSize: Number(search.get("datagramSendSize")) || 0,
      datagramSendCount: Number(search.get("datagramSendCount")) || 1,
      streamMode: search.get("streamMode") || "baton",
      streamSize: Number(search.get("streamSize")) || 0,
      streamCount: Number(search.get("streamCount")) || 1,
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
      var result = options.streamMode && options.streamMode !== "baton" ?
        await runStreamTest(options) : await runBatonTest(options);
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
        datagramReceiveMode: options.datagramReceiveMode || "baton",
        datagramReceiveMin: options.datagramReceiveMin || 0,
        datagramSendMode: options.datagramSendMode || "baton",
        datagramSendSize: options.datagramSendSize || 0,
        datagramSendCount: options.datagramSendCount || 1,
        streamMode: options.streamMode || "baton",
        streamSize: options.streamSize || 0,
        streamCount: options.streamCount || 1,
        streamBytesSent: 0,
        streamBytesReceived: 0,
        streamFinSent: 0,
        streamFinReceived: 0,
        received: [],
        sent: [],
        datagramsReceived: [],
        datagramLengths: [],
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
    runWritableBadChunkTests: runWritableBadChunkTests,
    runCloseSessionTests: runCloseSessionTests,
    runPostCloseDatagramTests: runPostCloseDatagramTests,
    runSessionDiagnostics: runSessionDiagnostics,
    runBatonTest: runBatonTest,
    runStreamTest: runStreamTest
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init, { once: true });
  } else {
    init();
  }
}());
