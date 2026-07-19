#include "js/runtime_sources.h"

namespace mystral::js::runtime_sources {

const char* agentBridge() {
    return R"JS(
(() => {
    const inspectors = new Map();
    const actions = new Map();

    function requireId(id, type) {
        if (typeof id !== 'string' || id.length === 0) {
            throw new TypeError(type + ' id must be a non-empty string');
        }
        return id;
    }

    function optionsOrEmpty(options) {
        return options && typeof options === 'object' ? options : {};
    }

    function compareById(a, b) {
        return a.id < b.id ? -1 : a.id > b.id ? 1 : 0;
    }

    const api = {
        version: 1,

        exposeInspector(id, inspect, options = {}) {
            id = requireId(id, 'Inspector');
            if (typeof inspect !== 'function') {
                throw new TypeError('Inspector ' + id + ' must be a function');
            }
            options = optionsOrEmpty(options);
            const record = {
                id,
                inspect,
                label: options.label === undefined ? id : String(options.label),
                kind: options.kind === undefined ? 'entity' : String(options.kind),
                description: options.description === undefined ? '' : String(options.description)
            };
            inspectors.set(id, record);
            return () => inspectors.get(id) === record && inspectors.delete(id);
        },

        exposeAction(id, run, options = {}) {
            id = requireId(id, 'Action');
            if (typeof run !== 'function') {
                throw new TypeError('Action ' + id + ' must be a function');
            }
            options = optionsOrEmpty(options);
            const record = {
                id,
                run,
                description: options.description === undefined ? '' : String(options.description),
                entityId: options.entityId === undefined ? null : String(options.entityId),
                inputSchema: options.inputSchema === undefined ? null : options.inputSchema
            };
            actions.set(id, record);
            return () => actions.get(id) === record && actions.delete(id);
        },

        removeInspector(id) {
            return inspectors.delete(String(id));
        },

        removeAction(id) {
            return actions.delete(String(id));
        },

        clear() {
            inspectors.clear();
            actions.clear();
        }
    };

    function dispatch(method, params) {
        params = optionsOrEmpty(params);

        if (method === 'list') {
            return {
                entities: Array.from(inspectors.values(), (record) => ({
                    id: record.id,
                    label: record.label,
                    kind: record.kind,
                    description: record.description
                })).sort(compareById),
                actions: Array.from(actions.values(), (record) => ({
                    id: record.id,
                    description: record.description,
                    entityId: record.entityId,
                    inputSchema: record.inputSchema
                })).sort(compareById)
            };
        }

        if (method === 'inspect') {
            const id = requireId(params.id, 'Inspector');
            const record = inspectors.get(id);
            if (!record) throw new Error('Unknown inspector: ' + id);
            const snapshot = record.inspect(params.input);
            if (snapshot && typeof snapshot.then === 'function') {
                throw new Error('Inspector ' + id + ' returned a Promise; inspectors must be synchronous');
            }
            return { id, snapshot: snapshot === undefined ? null : snapshot };
        }

        if (method === 'act') {
            const id = requireId(params.id, 'Action');
            const record = actions.get(id);
            if (!record) throw new Error('Unknown action: ' + id);
            const result = record.run(params.input);
            if (result && typeof result.then === 'function') {
                throw new Error('Action ' + id + ' returned a Promise; agent actions must be synchronous');
            }
            return { id, result: result === undefined ? null : result };
        }

        throw new Error('Unknown agent method: ' + method);
    }

    Object.defineProperty(api, '__dispatch', { value: dispatch });
    Object.freeze(api);
    Object.defineProperty(globalThis, 'mystralAgent', {
        value: api,
        writable: false,
        configurable: false,
        enumerable: false
    });
})();
)JS";
}

const char* storagePolyfill() {
    return R"JS(
// localStorage - backed by native C++ file storage
(function() {
    function createStorage(nativeBacked) {
        // In-memory store for sessionStorage (or fallback)
        var memStore = {};
        var memKeys = [];

        var storage = {
            getItem: function(key) {
                key = String(key);
                if (nativeBacked) {
                    return __storageGetItem(key);
                }
                return memStore.hasOwnProperty(key) ? memStore[key] : null;
            },
            setItem: function(key, value) {
                key = String(key);
                value = String(value);
                if (nativeBacked) {
                    __storageSetItem(key, value);
                } else {
                    if (!memStore.hasOwnProperty(key)) {
                        memKeys.push(key);
                    }
                    memStore[key] = value;
                }
            },
            removeItem: function(key) {
                key = String(key);
                if (nativeBacked) {
                    __storageRemoveItem(key);
                } else {
                    if (memStore.hasOwnProperty(key)) {
                        delete memStore[key];
                        var idx = memKeys.indexOf(key);
                        if (idx !== -1) memKeys.splice(idx, 1);
                    }
                }
            },
            clear: function() {
                if (nativeBacked) {
                    __storageClear();
                } else {
                    memStore = {};
                    memKeys = [];
                }
            },
            key: function(index) {
                if (nativeBacked) {
                    return __storageKey(index);
                }
                return index >= 0 && index < memKeys.length ? memKeys[index] : null;
            },
            get length() {
                if (nativeBacked) {
                    return __storageLength();
                }
                return memKeys.length;
            }
        };

        // Wrap with Proxy for bracket access (localStorage['key'] and localStorage.key)
        if (typeof Proxy !== 'undefined') {
            return new Proxy(storage, {
                get: function(target, prop) {
                    // Return own methods/properties first
                    if (prop in target) return target[prop];
                    if (typeof prop === 'symbol') return undefined;
                    // Treat as getItem
                    return target.getItem(prop);
                },
                set: function(target, prop, value) {
                    // Don't intercept known method names
                    if (prop === 'getItem' || prop === 'setItem' || prop === 'removeItem' ||
                        prop === 'clear' || prop === 'key' || prop === 'length') {
                        return false;
                    }
                    if (typeof prop === 'symbol') return false;
                    target.setItem(prop, value);
                    return true;
                },
                deleteProperty: function(target, prop) {
                    target.removeItem(prop);
                    return true;
                }
            });
        }

        return storage;
    }

    // localStorage: backed by native C++ file storage (persistent)
    globalThis.localStorage = createStorage(true);

    // sessionStorage: in-memory only (cleared when app closes)
    globalThis.sessionStorage = createStorage(false);
})();
)JS";
}

const char* fetchPolyfill() {
    return R"(
// TextDecoder polyfill (if not available)
if (typeof TextDecoder === 'undefined') {
    class TextDecoder {
        constructor(encoding = 'utf-8') {
            this.encoding = encoding;
        }
        decode(input) {
            if (!input) return '';
            const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
            let result = '';
            for (let i = 0; i < bytes.length; i++) {
                result += String.fromCharCode(bytes[i]);
            }
            // Handle UTF-8 decoding properly
            try {
                return decodeURIComponent(escape(result));
            } catch (e) {
                return result;
            }
        }
    }
    globalThis.TextDecoder = TextDecoder;
}

// TextEncoder polyfill (if not available)
if (typeof TextEncoder === 'undefined') {
    class TextEncoder {
        constructor() {
            this.encoding = 'utf-8';
        }
        encode(str) {
            const utf8 = unescape(encodeURIComponent(str));
            const result = new Uint8Array(utf8.length);
            for (let i = 0; i < utf8.length; i++) {
                result[i] = utf8.charCodeAt(i);
            }
            return result;
        }
    }
    globalThis.TextEncoder = TextEncoder;
}

// AbortController / AbortSignal polyfill (Web API standard)
// Three.js' FileLoader/GLTFLoader (r168+) construct an AbortController to
// manage fetch cancellation, so these globals must exist or loading throws
// "ReferenceError: AbortController is not defined". MystralNative's native
// fetch cannot cancel an in-flight request, but it honors an aborted signal
// by rejecting the fetch promise (see fetch() below).
if (typeof AbortSignal === 'undefined') {
    const makeAbortError = function (name, message) {
        const error = new Error(message);
        error.name = name;
        return error;
    };
    class AbortSignal {
        constructor() {
            this.aborted = false;
            this.reason = undefined;
            this.onabort = null;
            this._listeners = [];
        }
        addEventListener(type, listener) {
            if (type === 'abort' && typeof listener === 'function') {
                this._listeners.push(listener);
            }
        }
        removeEventListener(type, listener) {
            if (type === 'abort') {
                this._listeners = this._listeners.filter(function (l) { return l !== listener; });
            }
        }
        dispatchEvent(event) {
            if (event && event.type === 'abort') {
                if (typeof this.onabort === 'function') this.onabort(event);
                const listeners = this._listeners.slice();
                for (let i = 0; i < listeners.length; i++) listeners[i](event);
            }
            return true;
        }
        throwIfAborted() {
            if (this.aborted) throw (this.reason !== undefined
                ? this.reason
                : makeAbortError('AbortError', 'The operation was aborted'));
        }
        _fireAbort(reason) {
            if (this.aborted) return;
            this.aborted = true;
            this.reason = (reason !== undefined
                ? reason
                : makeAbortError('AbortError', 'The operation was aborted'));
            this.dispatchEvent({ type: 'abort', target: this });
        }
        static abort(reason) {
            const signal = new AbortSignal();
            signal._fireAbort(reason);
            return signal;
        }
        static timeout(ms) {
            const signal = new AbortSignal();
            setTimeout(function () {
                signal._fireAbort(makeAbortError('TimeoutError', 'The operation timed out'));
            }, ms);
            return signal;
        }
        static any(signals) {
            const result = new AbortSignal();
            const list = Array.from(signals || []);
            for (let i = 0; i < list.length; i++) {
                const s = list[i];
                if (!s) continue;
                if (s.aborted) { result._fireAbort(s.reason); return result; }
                s.addEventListener('abort', function () { result._fireAbort(s.reason); });
            }
            return result;
        }
    }
    globalThis.AbortSignal = AbortSignal;
}

if (typeof AbortController === 'undefined') {
    class AbortController {
        constructor() {
            this.signal = new AbortSignal();
        }
        abort(reason) {
            this.signal._fireAbort(reason);
        }
    }
    globalThis.AbortController = AbortController;
}

// Blob class (Web API standard)
if (typeof Blob === 'undefined') {
    class Blob {
        constructor(blobParts = [], options = {}) {
            this.type = options.type || '';

            // Concatenate all parts into a single ArrayBuffer
            let totalSize = 0;
            const parts = [];

            for (const part of blobParts) {
                if (part instanceof ArrayBuffer) {
                    parts.push(new Uint8Array(part));
                    totalSize += part.byteLength;
                } else if (part instanceof Uint8Array) {
                    parts.push(part);
                    totalSize += part.byteLength;
                } else if (part instanceof Blob) {
                    // Need to get the Blob's internal data
                    parts.push(new Uint8Array(part._data));
                    totalSize += part._data.byteLength;
                } else if (typeof part === 'string') {
                    const encoder = new TextEncoder();
                    const encoded = encoder.encode(part);
                    parts.push(encoded);
                    totalSize += encoded.byteLength;
                }
            }

            // Create final buffer
            const buffer = new ArrayBuffer(totalSize);
            const view = new Uint8Array(buffer);
            let offset = 0;
            for (const part of parts) {
                view.set(part, offset);
                offset += part.byteLength;
            }

            this._data = buffer;
            this.size = totalSize;
        }

        async arrayBuffer() {
            return this._data;
        }

        async text() {
            const decoder = new TextDecoder();
            return decoder.decode(new Uint8Array(this._data));
        }

        slice(start = 0, end = this.size, type = '') {
            const data = new Uint8Array(this._data, start, end - start);
            return new Blob([data], { type });
        }

        async stream() {
            // ReadableStream not implemented yet
            throw new Error('Blob.stream() not implemented');
        }
    }
    globalThis.Blob = Blob;
}

// Headers class - mimics Web Headers API
class Headers {
    constructor(init = {}) {
        this._headers = new Map();
        if (init) {
            if (init instanceof Headers) {
                init.forEach((value, key) => this._headers.set(key.toLowerCase(), value));
            } else if (Array.isArray(init)) {
                init.forEach(([key, value]) => this._headers.set(key.toLowerCase(), value));
            } else if (typeof init === 'object') {
                Object.entries(init).forEach(([key, value]) => this._headers.set(key.toLowerCase(), value));
            }
        }
    }

    get(name) {
        return this._headers.get(name.toLowerCase()) || null;
    }

    set(name, value) {
        this._headers.set(name.toLowerCase(), value);
    }

    has(name) {
        return this._headers.has(name.toLowerCase());
    }

    delete(name) {
        this._headers.delete(name.toLowerCase());
    }

    entries() {
        return this._headers.entries();
    }

    keys() {
        return this._headers.keys();
    }

    values() {
        return this._headers.values();
    }

    forEach(callback) {
        this._headers.forEach((value, key) => callback(value, key, this));
    }

    [Symbol.iterator]() {
        return this._headers.entries();
    }
}
globalThis.Headers = Headers;

// Response class
class Response {
    constructor(data, options = {}) {
        this._data = data;
        this.ok = options.ok !== undefined ? options.ok : true;
        this.status = options.status || 200;
        this.statusText = options.statusText || 'OK';
        this.url = options.url || '';
        this.headers = new Headers(options.headers || {});
    }

    async arrayBuffer() {
        return this._data;
    }

    async text() {
        const decoder = new TextDecoder();
        return decoder.decode(new Uint8Array(this._data));
    }

    async json() {
        const text = await this.text();
        return JSON.parse(text);
    }

    async blob() {
        return new Blob([this._data]);
    }
}

// Request class (Web API standard) - Three.js' FileLoader (r168+) wraps the
// URL in a Request (with headers/credentials/signal) before calling fetch().
class Request {
    constructor(input, init = {}) {
        if (input && typeof input === 'object' && typeof input.url === 'string') {
            this.url = input.url;
            this.method = init.method || input.method || 'GET';
            this.headers = new Headers(init.headers || input.headers || {});
            this.credentials = init.credentials || input.credentials || 'same-origin';
            this.signal = init.signal || input.signal || null;
            this.body = init.body !== undefined ? init.body : (input.body != null ? input.body : null);
        } else {
            this.url = String(input);
            this.method = (init.method || 'GET');
            this.headers = new Headers(init.headers || {});
            this.credentials = init.credentials || 'same-origin';
            this.signal = init.signal || null;
            this.body = init.body !== undefined ? init.body : null;
        }
        this.mode = init.mode || 'cors';
    }
}
globalThis.Request = Request;

// Fetch function - supports file://, http://, and https://
// HTTP requests are now async via libuv (non-blocking)
// Accepts either a URL string or a Request object (Three.js passes a Request).
async function fetch(input, options = {}) {
    let url;
    if (input && typeof input === 'object' && typeof input.url === 'string') {
        // Unwrap a Request object: pull url + per-request fields unless overridden.
        url = input.url;
        if (options.signal === undefined && input.signal) options.signal = input.signal;
        if (options.method === undefined && input.method) options.method = input.method;
        if (options.headers === undefined && input.headers) options.headers = input.headers;
        if (options.body === undefined && input.body != null) options.body = input.body;
    } else {
        url = String(input);
    }

    // AbortController support: reject up-front if the signal is already aborted.
    // The native request itself cannot be cancelled mid-flight, but a late abort
    // rejects the promise (the in-flight native op simply completes and is ignored).
    const signal = options && options.signal;
    const abortError = () => (signal && signal.reason !== undefined ? signal.reason : new Error('AbortError'));
    if (signal && signal.aborted) {
        return Promise.reject(abortError());
    }

    // blob: URLs - created via URL.createObjectURL(). Three.js GLTFLoader uses
    // these for embedded (GLB) and external textures, fetched by ImageBitmapLoader.
    if (url.startsWith('blob:')) {
        const blob = (typeof URL !== 'undefined' && URL._getBlobData) ? URL._getBlobData(url) : null;
        if (!blob) {
            return new Response(new ArrayBuffer(0), { ok: false, status: 404, statusText: 'Not Found', url });
        }
        let data = blob._data;
        if (data instanceof Uint8Array) data = data.buffer;
        if (!(data instanceof ArrayBuffer)) data = new ArrayBuffer(0);
        return new Response(data, {
            ok: true, status: 200, statusText: 'OK', url,
            headers: { 'content-type': blob.type || '' }
        });
    }

    // Check URL type
    if (url.startsWith('http://') || url.startsWith('https://')) {
        // HTTP/HTTPS request via async libcurl + libuv (non-blocking)
        return new Promise((resolve, reject) => {
            if (signal) signal.addEventListener('abort', () => reject(abortError()));
            __httpRequestAsync(url, options, (result) => {
                if (result.error) {
                    reject(new Error('Fetch error: ' + result.error));
                } else {
                    resolve(new Response(result.data || new ArrayBuffer(0), {
                        ok: result.ok,
                        status: result.status,
                        statusText: result.ok ? 'OK' : 'Error',
                        url: result.url || url
                    }));
                }
            });
        });
    }

    // File URL or relative path - use async file reading for non-blocking I/O
    let path = url;
    if (url.startsWith('file://')) {
        path = url;
    } else if (!url.includes('://')) {
        // Relative path - treat as file
        path = url;
    } else {
        throw new Error('Unsupported URL scheme: ' + url.split('://')[0]);
    }

    // Use async file reading to avoid blocking the render loop
    return new Promise((resolve, reject) => {
        if (signal) signal.addEventListener('abort', () => reject(abortError()));
        __readFileAsync(path, (data, error) => {
            if (error) {
                reject(new Error('File read error: ' + error));
            } else if (data === null) {
                resolve(new Response(new ArrayBuffer(0), {
                    ok: false,
                    status: 404,
                    statusText: 'Not Found',
                    url: url
                }));
            } else {
                resolve(new Response(data, {
                    ok: true,
                    status: 200,
                    statusText: 'OK',
                    url: url
                }));
            }
        });
    });
}

// Also expose globally
globalThis.fetch = fetch;
globalThis.Response = Response;
)";
}

const char* streamsPolyfill() {
    return R"STREAMS(
(function () {
  const AS_ITER = Symbol.asyncIterator;

  if (typeof globalThis.ReadableStream === 'undefined') {
    class ReadableStreamDefaultController {
      constructor(stream) { this._stream = stream; }
      enqueue(chunk) { this._stream._enqueue(chunk); }
      close() { this._stream._close(); }
      error(e) { this._stream._error(e); }
      get desiredSize() {
        const s = this._stream;
        if (s._state === 'errored') return null;
        if (s._state === 'closed') return 0;
        return 1;
      }
    }

    class ReadableStreamDefaultReader {
      constructor(stream) {
        this._stream = stream;
        let res, rej;
        this._closedPromise = new Promise((a, b) => { res = a; rej = b; });
        this._closedResolve = res; this._closedReject = rej;
        this._closedPromise.catch(() => {});
        if (stream._state === 'closed') res();
        else if (stream._state === 'errored') rej(stream._storedError);
      }
      read() {
        const s = this._stream;
        if (!s) return Promise.reject(new TypeError('Reader has been released'));
        if (s._queue.length) return Promise.resolve({ value: s._queue.shift(), done: false });
        if (s._state === 'errored') return Promise.reject(s._storedError);
        if (s._state === 'closed') return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve, reject) => s._readRequests.push({ resolve, reject }));
      }
      cancel(reason) { return this._stream ? this._stream.cancel(reason) : Promise.resolve(); }
      releaseLock() {
        if (!this._stream) return;
        this._stream._reader = null;
        this._stream = null;
      }
      get closed() { return this._closedPromise; }
    }

    class ReadableStream {
      constructor(underlyingSource = {}, strategy = {}) {
        this._queue = [];
        this._readRequests = [];
        this._state = 'readable';
        this._storedError = undefined;
        this._reader = null;
        this._source = underlyingSource || {};
        this._controller = new ReadableStreamDefaultController(this);
        if (typeof this._source.start === 'function') {
          try { Promise.resolve(this._source.start(this._controller)).catch((e) => this._error(e)); }
          catch (e) { this._error(e); }
        }
      }
      _enqueue(chunk) {
        if (this._state !== 'readable') return;
        if (this._readRequests.length) this._readRequests.shift().resolve({ value: chunk, done: false });
        else this._queue.push(chunk);
      }
      _close() {
        if (this._state !== 'readable') return;
        this._state = 'closed';
        while (this._readRequests.length) this._readRequests.shift().resolve({ value: undefined, done: true });
        if (this._reader && this._reader._closedResolve) this._reader._closedResolve();
      }
      _error(e) {
        if (this._state !== 'readable') return;
        this._state = 'errored';
        this._storedError = e;
        while (this._readRequests.length) this._readRequests.shift().reject(e);
        if (this._reader && this._reader._closedReject) this._reader._closedReject(e);
      }
      get locked() { return this._reader !== null; }
      getReader(opts) {
        if (opts && opts.mode === 'byob') throw new TypeError('BYOB readers are not supported');
        if (this._reader) throw new TypeError('ReadableStream is locked to a reader');
        this._reader = new ReadableStreamDefaultReader(this);
        return this._reader;
      }
      cancel(reason) {
        if (this._state === 'readable') {
          this._queue = [];
          try { if (typeof this._source.cancel === 'function') this._source.cancel(reason); } catch (e) {}
          this._close();
        }
        return Promise.resolve();
      }
      async pipeTo(dest, options = {}) {
        options = options || {};
        const reader = this.getReader();
        const writer = dest.getWriter();
        try {
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            if (writer.ready) { try { await writer.ready; } catch (e) {} }
            await writer.write(value);
          }
          if (!options.preventClose) await writer.close();
        } catch (e) {
          if (!options.preventAbort) { try { await writer.abort(e); } catch (_) {} }
          reader.releaseLock(); writer.releaseLock();
          throw e;
        }
        reader.releaseLock();
        writer.releaseLock();
      }
      pipeThrough(transform, options) {
        if (!transform || !transform.writable || !transform.readable)
          throw new TypeError('pipeThrough requires an object with { writable, readable }');
        this.pipeTo(transform.writable, options).catch(() => {});
        return transform.readable;
      }
      tee() {
        const reader = this.getReader();
        const boxA = {}; boxA.stream = new globalThis.ReadableStream({ start(c) { boxA.c = c; } });
        const boxB = {}; boxB.stream = new globalThis.ReadableStream({ start(c) { boxB.c = c; } });
        (async () => {
          try {
            while (true) {
              const { value, done } = await reader.read();
              if (done) { boxA.c.close(); boxB.c.close(); break; }
              boxA.c.enqueue(value); boxB.c.enqueue(value);
            }
          } catch (e) { boxA.c.error(e); boxB.c.error(e); }
        })();
        return [boxA.stream, boxB.stream];
      }
      [AS_ITER]() {
        const reader = this.getReader();
        return {
          next() { return reader.read(); },
          return(v) { reader.releaseLock(); return Promise.resolve({ value: v, done: true }); },
          [AS_ITER]() { return this; },
        };
      }
    }
    globalThis.ReadableStream = ReadableStream;
  }

  if (typeof globalThis.WritableStream === 'undefined') {
    class WritableStreamDefaultWriter {
      constructor(stream) {
        this._stream = stream;
        this.ready = Promise.resolve();
        let r; this.closed = new Promise((res) => { r = res; }); this._closedResolve = r;
        this.closed.catch(() => {});
      }
      write(chunk) {
        const s = this._stream;
        if (!s) return Promise.reject(new TypeError('Writer has been released'));
        if (s._state === 'errored') return Promise.reject(s._storedError);
        try { return Promise.resolve(s._sink.write ? s._sink.write(chunk, s._controller) : undefined); }
        catch (e) { s._error(e); return Promise.reject(e); }
      }
      close() {
        const s = this._stream;
        if (!s) return Promise.resolve();
        if (s._state === 'writable') s._state = 'closed';
        if (this._closedResolve) this._closedResolve();
        try { return Promise.resolve(s._sink.close ? s._sink.close() : undefined); }
        catch (e) { return Promise.reject(e); }
      }
      abort(reason) {
        const s = this._stream;
        if (!s) return Promise.resolve();
        s._state = 'errored'; s._storedError = reason;
        try { return Promise.resolve(s._sink.abort ? s._sink.abort(reason) : undefined); }
        catch (e) { return Promise.reject(e); }
      }
      get desiredSize() { return 1; }
      releaseLock() { if (this._stream) { this._stream._writer = null; this._stream = null; } }
    }

    class WritableStreamDefaultController {
      constructor(stream) { this._stream = stream; }
      error(e) { this._stream._error(e); }
    }

    class WritableStream {
      constructor(underlyingSink = {}, strategy = {}) {
        this._sink = underlyingSink || {};
        this._state = 'writable';
        this._storedError = undefined;
        this._writer = null;
        this._controller = new WritableStreamDefaultController(this);
        if (typeof this._sink.start === 'function') {
          try { this._sink.start(this._controller); } catch (e) { this._error(e); }
        }
      }
      _error(e) { if (this._state === 'writable') { this._state = 'errored'; this._storedError = e; } }
      get locked() { return this._writer !== null; }
      getWriter() {
        if (this._writer) throw new TypeError('WritableStream is locked to a writer');
        this._writer = new WritableStreamDefaultWriter(this);
        return this._writer;
      }
      abort(reason) {
        if (this._state === 'writable') {
          this._state = 'errored'; this._storedError = reason;
          try { if (this._sink.abort) this._sink.abort(reason); } catch (e) {}
        }
        return Promise.resolve();
      }
      close() {
        if (this._state === 'writable') {
          this._state = 'closed';
          try { if (this._sink.close) return Promise.resolve(this._sink.close()); } catch (e) { return Promise.reject(e); }
        }
        return Promise.resolve();
      }
    }
    globalThis.WritableStream = WritableStream;
  }

  if (typeof globalThis.TransformStream === 'undefined') {
    class TransformStream {
      constructor(transformer = {}, writableStrategy = {}, readableStrategy = {}) {
        transformer = transformer || {};
        const box = {};
        this.readable = new globalThis.ReadableStream({ start(c) { box.c = c; } });
        const transform = typeof transformer.transform === 'function'
          ? transformer.transform
          : (chunk, controller) => controller.enqueue(chunk);
        const tc = {
          enqueue: (chunk) => box.c.enqueue(chunk),
          terminate: () => box.c.close(),
          error: (e) => box.c.error(e),
        };
        this.writable = new globalThis.WritableStream({
          start() { if (typeof transformer.start === 'function') return transformer.start(tc); },
          write(chunk) { return transform(chunk, tc); },
          close() {
            const done = typeof transformer.flush === 'function' ? transformer.flush(tc) : undefined;
            return Promise.resolve(done).then(() => box.c.close());
          },
          abort(reason) { box.c.error(reason); },
        });
      }
    }
    globalThis.TransformStream = TransformStream;
  }

  if (typeof globalThis.TextEncoderStream === 'undefined') {
    class TextEncoderStream {
      constructor() {
        this.encoding = 'utf-8';
        const encoder = new TextEncoder();
        const ts = new globalThis.TransformStream({
          transform(chunk, c) { c.enqueue(encoder.encode(chunk == null ? '' : String(chunk))); },
        });
        this.readable = ts.readable;
        this.writable = ts.writable;
      }
    }
    globalThis.TextEncoderStream = TextEncoderStream;
  }

  if (typeof globalThis.TextDecoderStream === 'undefined') {
    class TextDecoderStream {
      constructor(label = 'utf-8', options = {}) {
        this.encoding = label || 'utf-8';
        const decoder = new TextDecoder(this.encoding);
        const ts = new globalThis.TransformStream({
          transform(chunk, c) {
            let bytes;
            if (chunk instanceof Uint8Array) bytes = chunk;
            else if (chunk instanceof ArrayBuffer) bytes = new Uint8Array(chunk);
            else if (ArrayBuffer.isView(chunk)) bytes = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
            else bytes = chunk;
            const text = decoder.decode(bytes);
            if (text) c.enqueue(text);
          },
        });
        this.readable = ts.readable;
        this.writable = ts.writable;
      }
    }
    globalThis.TextDecoderStream = TextDecoderStream;
  }
})();
)STREAMS";
}

const char* urlPolyfill() {
    return R"JS(
// URLSearchParams polyfill
if (typeof URLSearchParams === 'undefined') {
    class URLSearchParams {
        constructor(init) {
            this._params = [];
            if (typeof init === 'string') {
                const str = init.startsWith('?') ? init.slice(1) : init;
                if (str) {
                    str.split('&').forEach(pair => {
                        const eq = pair.indexOf('=');
                        if (eq >= 0) {
                            this._params.push([decodeURIComponent(pair.slice(0, eq)), decodeURIComponent(pair.slice(eq + 1))]);
                        } else {
                            this._params.push([decodeURIComponent(pair), '']);
                        }
                    });
                }
            } else if (init && typeof init === 'object') {
                if (Array.isArray(init)) {
                    init.forEach(([k, v]) => this._params.push([String(k), String(v)]));
                } else {
                    Object.entries(init).forEach(([k, v]) => this._params.push([String(k), String(v)]));
                }
            }
        }
        get(name) {
            const entry = this._params.find(([k]) => k === name);
            return entry ? entry[1] : null;
        }
        has(name) { return this._params.some(([k]) => k === name); }
        set(name, value) {
            const idx = this._params.findIndex(([k]) => k === name);
            if (idx >= 0) this._params[idx] = [name, String(value)];
            else this._params.push([name, String(value)]);
        }
        append(name, value) { this._params.push([String(name), String(value)]); }
        delete(name) { this._params = this._params.filter(([k]) => k !== name); }
        toString() {
            return this._params.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
        }
        forEach(cb) { this._params.forEach(([k, v]) => cb(v, k, this)); }
        entries() { return this._params[Symbol.iterator](); }
        keys() { return this._params.map(([k]) => k)[Symbol.iterator](); }
        values() { return this._params.map(([, v]) => v)[Symbol.iterator](); }
        [Symbol.iterator]() { return this.entries(); }
    }
    globalThis.URLSearchParams = URLSearchParams;
}

// URL polyfill
if (typeof URL === 'undefined') {
    const _blobStore = new Map();
    let _blobCounter = 0;

    class URL {
        constructor(url, base) {
            if (typeof url !== 'string') url = String(url);
            let fullUrl = url;

            // Resolve relative URLs against base
            if (base !== undefined) {
                const b = typeof base === 'string' ? base : String(base);
                if (/^[a-z][a-z0-9+.-]*:/i.test(url)) {
                    // url is already absolute
                    fullUrl = url;
                } else if (url.startsWith('//')) {
                    const proto = b.match(/^([a-z][a-z0-9+.-]*:)/i);
                    fullUrl = (proto ? proto[1] : 'https:') + url;
                } else if (url.startsWith('/')) {
                    const origin = b.match(/^([a-z][a-z0-9+.-]*:\/\/[^/?#]*)/i);
                    fullUrl = (origin ? origin[1] : '') + url;
                } else {
                    const baseNoQuery = b.split('?')[0].split('#')[0];
                    const lastSlash = baseNoQuery.lastIndexOf('/');
                    fullUrl = baseNoQuery.slice(0, lastSlash + 1) + url;
                }
            }

            // Parse components
            const match = fullUrl.match(/^([a-z][a-z0-9+.-]*:)?(\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/i);
            if (!match) throw new TypeError('Invalid URL: ' + url);

            this.protocol = match[1] || '';
            const authority = match[3] || '';
            this.pathname = match[4] || '/';
            this.search = match[5] || '';
            this.hash = match[6] || '';

            // Parse authority (userinfo@host:port)
            const atIdx = authority.lastIndexOf('@');
            const hostPart = atIdx >= 0 ? authority.slice(atIdx + 1) : authority;
            const portMatch = hostPart.match(/:(\d+)$/);
            this.port = portMatch ? portMatch[1] : '';
            this.hostname = portMatch ? hostPart.slice(0, -portMatch[0].length) : hostPart;
            this.host = this.port ? this.hostname + ':' + this.port : this.hostname;
            this.origin = this.protocol ? this.protocol + '//' + this.host : '';
            this.href = fullUrl;
            this.username = '';
            this.password = '';
            if (atIdx >= 0) {
                const userInfo = authority.slice(0, atIdx);
                const colonIdx = userInfo.indexOf(':');
                this.username = colonIdx >= 0 ? userInfo.slice(0, colonIdx) : userInfo;
                this.password = colonIdx >= 0 ? userInfo.slice(colonIdx + 1) : '';
            }
            this.searchParams = new URLSearchParams(this.search);
        }

        toString() { return this.href; }
        toJSON() { return this.href; }

        static createObjectURL(blob) {
            const id = 'blob:mystral-native/' + (_blobCounter++);
            _blobStore.set(id, blob);
            return id;
        }

        static revokeObjectURL(url) {
            _blobStore.delete(url);
        }

        // Internal: retrieve blob data for Blob-backed Workers.
        static _getBlobData(url) {
            return _blobStore.get(url);
        }
    }

    globalThis.URL = URL;
}

// Native Worker facade. Each instance owns an OS thread and a JS engine.
if (typeof Worker === 'undefined') {
    const nativeWorkers = new Map();

    globalThis.__mystralDispatchWorkerMessage = function(id, type, payload, transfers) {
        const worker = nativeWorkers.get(id);
        if (!worker || worker._terminated) return;
        if (type === 'ready') {
            worker._ready = true;
            return;
        }
        if (type === 'exit') {
            worker._terminated = true;
            nativeWorkers.delete(id);
            return;
        }
        if (type === 'error') {
            const event = { type: 'error', message: payload, error: new Error(payload), target: worker };
            if (worker.onerror) worker.onerror.call(worker, event);
            for (const listener of worker._errorListeners.slice()) listener.call(worker, event);
            return;
        }
        const event = { type: 'message', data: __mystralParseMessage(payload, transfers), target: worker };
        if (worker.onmessage) worker.onmessage.call(worker, event);
        for (const listener of worker._messageListeners.slice()) listener.call(worker, event);
    };

    class NativeWorker {
        constructor(url, options = {}) {
            this.onmessage = null;
            this.onerror = null;
            this._terminated = false;
            this._ready = false;
            this._messageListeners = [];
            this._errorListeners = [];

            let kind = 1;
            let code = '';
            if (url && url._data) {
                code = new TextDecoder().decode(new Uint8Array(url._data));
                kind = 0;
            } else if (typeof url === 'string' && url.startsWith('blob:')) {
                const blob = URL._getBlobData(url);
                if (blob && blob._data) {
                    code = new TextDecoder().decode(new Uint8Array(blob._data));
                    kind = 0;
                }
            } else {
                code = String(url);
            }

            if (!code) throw new TypeError('Worker requires a script URL or Blob');
            const limit = name => {
                const value = options && options[name];
                if (value === undefined) return undefined;
                if (!Number.isSafeInteger(value) || value <= 0) {
                    throw new RangeError('Worker ' + name + ' must be a positive safe integer');
                }
                return value;
            };
            const maxMessages = limit('maxMessages');
            const maxMessageBytes = limit('maxMessageBytes');
            const maxQueuedBytes = limit('maxQueuedBytes');
            if (maxMessageBytes !== undefined && maxQueuedBytes !== undefined &&
                maxMessageBytes > maxQueuedBytes) {
                throw new RangeError('Worker maxMessageBytes cannot exceed maxQueuedBytes');
            }
            this._id = __mystralWorkerCreate(
                kind,
                code,
                options && options.name ? String(options.name) : '',
                maxMessages,
                maxMessageBytes,
                maxQueuedBytes,
            );
            if (this._id < 0) throw new Error('Failed to create Worker');
            nativeWorkers.set(this._id, this);
        }

        postMessage(data, transferList = []) {
            if (this._terminated) return;
            const prepared = __mystralPrepareMessage(data, transferList);
            const status = __mystralWorkerPostMessage(this._id, prepared.payload, prepared.transfers);
            if (status === 0) return;
            if (status === 2) throw new RangeError('Worker input queue is full');
            if (status === 3) throw new RangeError('Worker message exceeds the input queue byte limit');
            if (status === 5) throw new TypeError('Worker transfer list contains an invalid ArrayBuffer');
            throw new Error('Worker is no longer running');
        }

        terminate() {
            if (this._terminated) return;
            this._terminated = true;
            nativeWorkers.delete(this._id);
            __mystralWorkerTerminate(this._id);
        }

        addEventListener(type, handler) {
            if (typeof handler !== 'function') return;
            if (type === 'message') this._messageListeners.push(handler);
            else if (type === 'error') this._errorListeners.push(handler);
        }

        removeEventListener(type, handler) {
            const listeners = type === 'message' ? this._messageListeners : type === 'error' ? this._errorListeners : null;
            if (!listeners) return;
            const index = listeners.indexOf(handler);
            if (index >= 0) listeners.splice(index, 1);
        }
    }

    globalThis.Worker = NativeWorker;
}
)JS";
}

const char* domCreateElement() {
    return R"(
            function installEventTarget(target) {
                const listeners = Object.create(null);
                target.addEventListener = function(type, callback, options) {
                    if (typeof callback !== 'function' && !callback?.handleEvent) return;
                    const capture = typeof options === 'boolean' ? options : !!options?.capture;
                    (listeners[String(type)] ??= []).push({ callback, capture });
                };
                target.removeEventListener = function(type, callback, options) {
                    const capture = typeof options === 'boolean' ? options : !!options?.capture;
                    const entries = listeners[String(type)];
                    if (!entries) return;
                    const index = entries.findIndex(entry => entry.callback === callback && entry.capture === capture);
                    if (index >= 0) entries.splice(index, 1);
                };
                target.__dispatchListeners = function(event, capture) {
                    const entries = (listeners[event.type] || []).slice();
                    for (const entry of entries) {
                        if (entry.capture !== capture) continue;
                        if (typeof entry.callback === 'function') entry.callback.call(this, event);
                        else entry.callback.handleEvent.call(entry.callback, event);
                        if (event.__immediatePropagationStopped) break;
                    }
                    if (!capture && !event.__immediatePropagationStopped) {
                        const handler = this['on' + event.type];
                        if (typeof handler === 'function') handler.call(this, event);
                    }
                };
                target.dispatchEvent = function(event) {
                    if (!event || !event.type) throw new TypeError('Invalid event');
                    event.target = this;
                    event.currentTarget = this;
                    event.eventPhase = Event.AT_TARGET;
                    this.__dispatchListeners(event, true);
                    if (!event.__immediatePropagationStopped) this.__dispatchListeners(event, false);
                    event.currentTarget = null;
                    event.eventPhase = Event.NONE;
                    return !event.defaultPrevented;
                };
                return target;
            }

            function createTextControl(tagName) {
                const element = installEventTarget({
                    tagName: tagName.toUpperCase(),
                    type: tagName === 'input' ? 'text' : undefined,
                    value: '',
                    defaultValue: '',
                    disabled: false,
                    readOnly: false,
                    placeholder: '',
                    selectionStart: 0,
                    selectionEnd: 0,
                    selectionDirection: 'none',
                    style: {},
                    className: '',
                    id: ''
                });
                element.focus = function() {
                    if (this.disabled || this.readOnly) return;
                    if (document.activeElement === this) return;
                    if (document.activeElement && document.activeElement !== document.body &&
                        typeof document.activeElement.blur === 'function') {
                        document.activeElement.blur();
                    }
                    __nativeFocusTextInput.call(this);
                    this.dispatchEvent(new Event('focus'));
                };
                element.blur = function() {
                    if (document.activeElement !== this) return;
                    __nativeBlurTextInput.call(this);
                    this.dispatchEvent(new Event('blur'));
                };
                element.select = function() {
                    this.selectionStart = 0;
                    this.selectionEnd = this.value.length;
                    this.selectionDirection = 'none';
                };
                element.setSelectionRange = function(start, end, direction) {
                    const length = this.value.length;
                    this.selectionStart = Math.max(0, Math.min(length, Number(start) || 0));
                    this.selectionEnd = Math.max(this.selectionStart, Math.min(length, Number(end) || 0));
                    this.selectionDirection = direction || 'none';
                };
                element.setTextInputArea = function(x, y, width, height, cursor) {
                    return __nativeSetTextInputArea(x, y, width, height, cursor || 0);
                };
                element.__applyTextInput = function(text) {
                    if (this.disabled || this.readOnly) return;
                    text = String(text);
                    const start = this.selectionStart == null ? this.value.length : this.selectionStart;
                    const end = this.selectionEnd == null ? start : this.selectionEnd;
                    this.value = this.value.slice(0, start) + text + this.value.slice(end);
                    this.selectionStart = this.selectionEnd = start + text.length;
                    this.selectionDirection = 'none';
                };
                return element;
            }

            document.createElement = function(tagName) {
                tagName = String(tagName || '').toLowerCase();
                if (tagName === 'canvas') {
                    return {
                        tagName: 'CANVAS',
                        width: 64,
                        height: 64,
                        style: {},
                        toDataURL: function(mimeType) {
                            return __nativeCanvasToDataURL(mimeType || 'image/png');
                        },
                        getContext: function(type) { return null; }
                    };
                }
                if (tagName === 'script') {
                    return {
                        tagName: 'SCRIPT',
                        src: '',
                        type: '',
                        async: false,
                        onload: null,
                        onerror: null
                    };
                }
                if (tagName === 'style') {
                    return {
                        tagName: 'STYLE',
                        type: 'text/css',
                        textContent: ''
                    };
                }
                if (tagName === 'input' || tagName === 'textarea') {
                    return createTextControl(tagName);
                }
                if (tagName === 'div' || tagName === 'span' || tagName === 'img') {
                    return {
                        tagName: (tagName || '').toUpperCase(),
                        style: {},
                        className: '',
                        id: ''
                    };
                }
                return { tagName: (tagName || '').toUpperCase(), style: {} };
            };
            // Namespaced variant — libraries (e.g. three.js loaders) create
            // elements via createElementNS('http://www.w3.org/1999/xhtml', tag)
            document.createElementNS = function(namespaceURI, tagName) {
                return document.createElement(tagName);
            };
            // DOM event class globals — browser code type-checks events with
            // `instanceof PointerEvent` etc.; without these constructors that
            // check is a ReferenceError. Dispatched events are still plain
            // objects (instanceof yields false) — code should duck-type until
            // dispatch constructs real instances.
            if (typeof globalThis.Event === 'undefined') {
                globalThis.Event = class Event {
                    constructor(type, init) {
                        this.type = String(type || '');
                        this.bubbles = !!init?.bubbles;
                        this.cancelable = !!init?.cancelable;
                        this.defaultPrevented = false;
                        this.cancelBubble = false;
                        this.target = null;
                        this.currentTarget = null;
                        this.eventPhase = 0;
                        this.__immediatePropagationStopped = false;
                        Object.assign(this, init || {});
                    }
                    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
                    stopPropagation() { this.cancelBubble = true; }
                    stopImmediatePropagation() {
                        this.cancelBubble = true;
                        this.__immediatePropagationStopped = true;
                    }
                };
                Object.assign(globalThis.Event, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });
                Object.assign(globalThis.Event.prototype, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });
            }
            globalThis.UIEvent ??= class UIEvent extends globalThis.Event {};
            globalThis.MouseEvent ??= class MouseEvent extends globalThis.UIEvent {};
            globalThis.PointerEvent ??= class PointerEvent extends globalThis.MouseEvent {};
            globalThis.WheelEvent ??= class WheelEvent extends globalThis.MouseEvent {};
            globalThis.KeyboardEvent ??= class KeyboardEvent extends globalThis.UIEvent {};
            globalThis.InputEvent ??= class InputEvent extends globalThis.UIEvent {};
            globalThis.CompositionEvent ??= class CompositionEvent extends globalThis.UIEvent {};
            document.activeElement = document.body;
        )";
}

const char* clipboard() {
    return R"(
            navigator.clipboard ??= {
                readText() {
                    return Promise.resolve(__nativeClipboardReadText());
                },
                writeText(text) {
                    if (__nativeClipboardWriteText(String(text))) return Promise.resolve();
                    return Promise.reject(new Error('System clipboard is unavailable'));
                }
            };
        )";
}

const char* imageSupport() {
    return R"(
            // Pre-cache WebP support so @loaders.gl knows we can decode it
            // The library checks document.createElement('canvas').toDataURL('image/webp')
            (function() {
                try {
                    var canvas = document.createElement('canvas');
                    if (canvas && canvas.toDataURL) {
                        // Test WebP support - this caches the result
                        var webpResult = canvas.toDataURL('image/webp');
                        var webpSupported = webpResult.indexOf('data:image/webp') === 0;
                        console.log('[Mystral] WebP format support: ' + (webpSupported ? 'YES' : 'NO'));
                    }
                } catch (e) {
                    console.log('[Mystral] Error checking image format support: ' + e);
                }
            })();
        )";
}

}  // namespace mystral::js::runtime_sources
