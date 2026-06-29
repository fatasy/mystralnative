/**
 * WebTransport JavaScript polyfill.
 *
 * Defines the W3C WebTransport API surface (WebTransport, WebTransportError,
 * WebTransportDatagramDuplexStream-shaped object, send/receive streams) on top
 * of the low-level `__wt*` native bridge functions registered by
 * webtransport.cpp. It also installs `globalThis.__wtDispatch`, the single entry
 * point the native layer calls (on the main thread) to deliver events.
 *
 * Streams are backed by the runtime's real WHATWG ReadableStream/WritableStream
 * (installed by the streams polyfill in runtime.cpp), so they support the full
 * surface real WebTransport code expects: getReader().read() / getWriter()
 * .write()/close()/abort(), plus pipeTo(), pipeThrough(), tee() and async
 * iteration (for await...of). Datagrams and incoming streams are fed by stashing
 * each stream's controller and pushing native events into it.
 */

namespace mystral {
namespace webtransport {

const char* kWebTransportPolyfill = R"JS(
(function () {
  if (typeof globalThis.WebTransport !== 'undefined' && globalThis.__wtSessions) {
    return; // already installed
  }

  if (typeof globalThis.ReadableStream === 'undefined' ||
      typeof globalThis.WritableStream === 'undefined') {
    // The streams polyfill (runtime.cpp) must run first.
    console.error('[WebTransport] Web Streams not available; WebTransport disabled.');
    return;
  }

  const sessions = new Map();
  globalThis.__wtSessions = sessions;

  // --- WebTransportError ---------------------------------------------------
  class WebTransportError extends Error {
    constructor(message, options) {
      super(message || 'WebTransport error');
      this.name = 'WebTransportError';
      this.source = (options && options.source) || 'session';
      this.streamErrorCode = (options && options.streamErrorCode) ?? null;
    }
  }
  globalThis.WebTransportError = WebTransportError;

  function toBytes(chunk) {
    if (chunk instanceof Uint8Array) return chunk;
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) {
      return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    }
    return new Uint8Array(chunk);
  }

  // A readable backed by a controller we stash so native events can push into
  // it. Returns { stream, controller }.
  function makeReadable() {
    const box = {};
    box.stream = new ReadableStream({ start(c) { box.controller = c; } });
    return box;
  }

  // A WebTransportSendStream: a WritableStream that writes to a QUIC stream.
  function makeSendStream(sessionId, streamId) {
    return new WritableStream({
      write(chunk) {
        const bytes = toBytes(chunk);
        const n = __wtStreamWrite(sessionId, streamId, bytes, false);
        if (n < 0) throw new WebTransportError('Failed to write to stream', { source: 'stream' });
      },
      close() {
        // Send an empty frame with FIN to half-close.
        __wtStreamWrite(sessionId, streamId, new Uint8Array(0), true);
      },
      abort() {
        __wtStreamShutdown(sessionId, streamId, 0);
      },
    });
  }

  // A datagram WritableStream (writes unreliable datagrams).
  function makeDatagramWritable(sessionId) {
    return new WritableStream({
      write(chunk) {
        const bytes = toBytes(chunk);
        const r = __wtSendDatagram(sessionId, bytes);
        if (r < 0) {
          // Datagrams are unreliable; a full queue is not a fatal error.
        }
      },
    });
  }

  // --- WebTransport --------------------------------------------------------
  class WebTransport {
    constructor(url, options) {
      this._url = url;

      let readyResolve, readyReject, closedResolve, closedReject;
      this.ready = new Promise((res, rej) => { readyResolve = res; readyReject = rej; });
      this.closed = new Promise((res, rej) => { closedResolve = res; closedReject = rej; });
      // Avoid unhandled-rejection noise if the caller doesn't await closed.
      this.closed.catch(() => {});

      const id = __wtConnect(String(url));
      if (!id || id <= 0) {
        const err = new WebTransportError('Failed to initiate WebTransport connection to ' + url);
        readyReject(err);
        closedReject(err);
        this._state = null;
        return;
      }

      const dgramReadable = makeReadable();
      this.datagrams = {
        readable: dgramReadable.stream,
        writable: makeDatagramWritable(id),
        createWritable: () => makeDatagramWritable(id),
        maxDatagramSize: 1200,
        incomingMaxAge: null,
        outgoingMaxAge: null,
        incomingHighWaterMark: 1,
        outgoingHighWaterMark: 1,
      };

      const incomingUni = makeReadable();
      const incomingBidi = makeReadable();
      this.incomingUnidirectionalStreams = incomingUni.stream;
      this.incomingBidirectionalStreams = incomingBidi.stream;

      const state = {
        id,
        readyResolve, readyReject, closedResolve, closedReject,
        dgramReadable, incomingUni, incomingBidi,
        streams: new Map(),  // streamId -> { readable: box }
        ready: false,
        closedFlag: false,
        lastError: null,
      };
      this._state = state;
      sessions.set(id, state);
    }

    async createUnidirectionalStream() {
      const st = this._state;
      if (!st) throw new WebTransportError('Session is not connected');
      const sid = __wtCreateStream(st.id, false);
      if (sid < 0) throw new WebTransportError('Unable to create unidirectional stream', { source: 'stream' });
      return makeSendStream(st.id, sid);
    }

    async createBidirectionalStream() {
      const st = this._state;
      if (!st) throw new WebTransportError('Session is not connected');
      const sid = __wtCreateStream(st.id, true);
      if (sid < 0) throw new WebTransportError('Unable to create bidirectional stream', { source: 'stream' });
      const readable = makeReadable();
      const writable = makeSendStream(st.id, sid);
      st.streams.set(sid, { readable });
      return { readable: readable.stream, writable };
    }

    close(closeInfo) {
      const st = this._state;
      if (!st) return;
      const code = (closeInfo && closeInfo.closeCode) || 0;
      const reason = (closeInfo && closeInfo.reason) || '';
      __wtClose(st.id, code, reason);
    }
  }
  globalThis.WebTransport = WebTransport;

  // --- Native -> JS dispatch ----------------------------------------------
  globalThis.__wtDispatch = function (sessionId, type, a, b, c) {
    const st = sessions.get(sessionId);
    if (!st) return;

    switch (type) {
      case 'ready':
        st.ready = true;
        st.readyResolve();
        break;

      case 'error': {
        const err = new WebTransportError(a || 'WebTransport error');
        st.lastError = err;
        if (!st.ready) st.readyReject(err);
        break;
      }

      case 'closed': {
        if (st.closedFlag) break;
        st.closedFlag = true;
        const info = { closeCode: 0, reason: a || '' };
        if (st.lastError && !st.ready) {
          st.closedReject(st.lastError);
        } else {
          st.closedResolve(info);
        }
        try { st.dgramReadable.controller.close(); } catch (e) {}
        try { st.incomingUni.controller.close(); } catch (e) {}
        try { st.incomingBidi.controller.close(); } catch (e) {}
        for (const s of st.streams.values()) {
          try { if (s.readable) s.readable.controller.close(); } catch (e) {}
        }
        sessions.delete(sessionId);
        break;
      }

      case 'datagram':
        try { st.dgramReadable.controller.enqueue(a); } catch (e) {}  // a: Uint8Array
        break;

      case 'incomingUni': {
        const readable = makeReadable();
        st.streams.set(a, { readable });   // a: streamId
        try { st.incomingUni.controller.enqueue(readable.stream); } catch (e) {}
        break;
      }

      case 'incomingBidi': {
        const readable = makeReadable();
        const writable = makeSendStream(st.id, a);  // a: streamId
        st.streams.set(a, { readable });
        try { st.incomingBidi.controller.enqueue({ readable: readable.stream, writable }); } catch (e) {}
        break;
      }

      case 'streamData': {
        const s = st.streams.get(a);  // a: streamId, b: Uint8Array, c: fin
        if (s && s.readable) {
          if (b && b.length) { try { s.readable.controller.enqueue(b); } catch (e) {} }
          if (c) { try { s.readable.controller.close(); } catch (e) {} }
        }
        break;
      }

      case 'streamReset': {
        const s = st.streams.get(a);  // a: streamId, b: error code
        if (s && s.readable) {
          try {
            s.readable.controller.error(new WebTransportError('Stream reset (code ' + b + ')', { source: 'stream', streamErrorCode: b }));
          } catch (e) {}
        }
        st.streams.delete(a);
        break;
      }
    }
  };
})();
)JS";

}  // namespace webtransport
}  // namespace mystral
