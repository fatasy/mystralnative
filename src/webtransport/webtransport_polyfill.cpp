/**
 * WebTransport JavaScript polyfill.
 *
 * Defines the W3C WebTransport API surface (WebTransport, WebTransportError,
 * WebTransportDatagramDuplexStream-shaped object, send/receive streams) on top
 * of the low-level `__wt*` native bridge functions registered by
 * webtransport.cpp. It also installs `globalThis.__wtDispatch`, the single entry
 * point the native layer calls (on the main thread) to deliver events.
 *
 * Minimal, spec-shaped ReadableStream/WritableStream implementations are used so
 * the API works without depending on a full WHATWG Streams implementation. They
 * support the methods real WebTransport code uses: getReader().read() returning
 * { value, done }, and getWriter().write()/close()/abort().
 */

namespace mystral {
namespace webtransport {

const char* kWebTransportPolyfill = R"JS(
(function () {
  if (typeof globalThis.WebTransport !== 'undefined' && globalThis.__wtSessions) {
    return; // already installed
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

  // --- Minimal ReadableStream (queue-backed) -------------------------------
  class WTReadableStream {
    constructor() {
      this._chunks = [];
      this._readers = [];
      this._closed = false;
      this._error = null;
      this._locked = false;
    }
    _push(chunk) {
      if (this._readers.length) {
        this._readers.shift().resolve({ value: chunk, done: false });
      } else {
        this._chunks.push(chunk);
      }
    }
    _close() {
      this._closed = true;
      while (this._readers.length) {
        this._readers.shift().resolve({ value: undefined, done: true });
      }
    }
    _fail(err) {
      this._error = err;
      while (this._readers.length) {
        this._readers.shift().reject(err);
      }
    }
    get locked() { return this._locked; }
    getReader() {
      this._locked = true;
      const self = this;
      return {
        read() {
          if (self._chunks.length) {
            return Promise.resolve({ value: self._chunks.shift(), done: false });
          }
          if (self._error) return Promise.reject(self._error);
          if (self._closed) return Promise.resolve({ value: undefined, done: true });
          return new Promise((resolve, reject) => self._readers.push({ resolve, reject }));
        },
        cancel() { self._closed = true; return Promise.resolve(); },
        releaseLock() { self._locked = false; },
        get closed() { return Promise.resolve(); },
      };
    }
  }

  // --- Minimal WritableStream (sink-backed) --------------------------------
  class WTWritableStream {
    constructor(sink) {
      this._sink = sink;
      this._locked = false;
    }
    get locked() { return this._locked; }
    getWriter() {
      this._locked = true;
      const self = this;
      return {
        write(chunk) {
          try { return Promise.resolve(self._sink.write(chunk)); }
          catch (e) { return Promise.reject(e); }
        },
        close() {
          try { return Promise.resolve(self._sink.close ? self._sink.close() : undefined); }
          catch (e) { return Promise.reject(e); }
        },
        abort(reason) {
          try { return Promise.resolve(self._sink.abort ? self._sink.abort(reason) : undefined); }
          catch (e) { return Promise.reject(e); }
        },
        get ready() { return Promise.resolve(); },
        get closed() { return Promise.resolve(); },
        releaseLock() { self._locked = false; },
      };
    }
  }

  function toBytes(chunk) {
    if (chunk instanceof Uint8Array) return chunk;
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) {
      return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    }
    return new Uint8Array(chunk);
  }

  // A WebTransportSendStream: a WritableStream that writes to a QUIC stream.
  function makeSendStream(sessionId, streamId) {
    return new WTWritableStream({
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

      const dgramReadable = new WTReadableStream();
      const dgramWritable = new WTWritableStream({
        write: (chunk) => {
          const bytes = toBytes(chunk);
          const r = __wtSendDatagram(id, bytes);
          if (r < 0) {
            // Datagrams are unreliable; a full queue is not a fatal error.
          }
        },
      });

      this.datagrams = {
        readable: dgramReadable,
        writable: dgramWritable,
        maxDatagramSize: 1200,
        incomingMaxAge: null,
        outgoingMaxAge: null,
        incomingHighWaterMark: 1,
        outgoingHighWaterMark: 1,
      };

      const incomingUni = new WTReadableStream();
      const incomingBidi = new WTReadableStream();
      this.incomingUnidirectionalStreams = incomingUni;
      this.incomingBidirectionalStreams = incomingBidi;

      const state = {
        id,
        readyResolve, readyReject, closedResolve, closedReject,
        dgramReadable, incomingUni, incomingBidi,
        streams: new Map(),  // streamId -> { readable }
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
      const readable = new WTReadableStream();
      const writable = makeSendStream(st.id, sid);
      st.streams.set(sid, { readable });
      return { readable, writable };
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
        st.dgramReadable._close();
        st.incomingUni._close();
        st.incomingBidi._close();
        for (const s of st.streams.values()) {
          if (s.readable) s.readable._close();
        }
        sessions.delete(sessionId);
        break;
      }

      case 'datagram':
        st.dgramReadable._push(a);  // a: Uint8Array
        break;

      case 'incomingUni': {
        const readable = new WTReadableStream();
        st.streams.set(a, { readable });   // a: streamId
        st.incomingUni._push(readable);
        break;
      }

      case 'incomingBidi': {
        const readable = new WTReadableStream();
        const writable = makeSendStream(st.id, a);  // a: streamId
        st.streams.set(a, { readable });
        st.incomingBidi._push({ readable, writable });
        break;
      }

      case 'streamData': {
        const s = st.streams.get(a);  // a: streamId, b: Uint8Array, c: fin
        if (s && s.readable) {
          if (b && b.length) s.readable._push(b);
          if (c) s.readable._close();
        }
        break;
      }

      case 'streamReset': {
        const s = st.streams.get(a);  // a: streamId, b: error code
        if (s && s.readable) s.readable._fail(new WebTransportError('Stream reset (code ' + b + ')', { source: 'stream', streamErrorCode: b }));
        st.streams.delete(a);
        break;
      }
    }
  };
})();
)JS";

}  // namespace webtransport
}  // namespace mystral
