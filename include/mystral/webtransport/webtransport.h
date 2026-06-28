/**
 * WebTransport API (native)
 *
 * Implements the client side of the W3C WebTransport API
 * (https://developer.mozilla.org/en-US/docs/Web/API/WebTransport_API) on top of
 * QUIC + HTTP/3 via Cloudflare quiche.
 *
 * Architecture:
 *   - quiche provides the QUIC transport and the thin HTTP/3 layer used only to
 *     perform the extended CONNECT (:protocol = webtransport) handshake.
 *   - After the session is established, WebTransport datagrams are carried as
 *     HTTP/3 datagrams (RFC 9297, quarter-stream-id prefix) over QUIC datagrams,
 *     and WebTransport streams are carried as raw QUIC streams using the
 *     WEBTRANSPORT_STREAM (0x41) / WT uni stream type (0x54) signal framing.
 *   - The UDP socket and timers run on the shared libuv event loop. All JS
 *     callbacks are dispatched on the main thread from processEvents().
 *
 * When quiche/libuv are not compiled in, a stub is used so that constructing a
 * WebTransport in JS rejects cleanly instead of failing to link.
 */

#pragma once

namespace mystral {
namespace js {
class Engine;
}

namespace webtransport {

/**
 * Initialize the WebTransport subsystem. Idempotent. Safe to call before the
 * libuv event loop is running (sockets are created lazily on connect()).
 */
void init();

/**
 * Tear down all active sessions and release resources. Idempotent.
 */
void shutdown();

/**
 * Register the JS bindings: the low-level `__wt*` native bridge functions and
 * the high-level `WebTransport` / `WebTransportError` polyfill classes.
 * Returns true on success.
 */
bool initBindings(js::Engine* engine);

/**
 * Drive QUIC I/O for all sessions and dispatch any queued events to JS.
 * Must be called once per frame from the runtime poll loop, after the libuv
 * event loop has been pumped (EventLoop::runOnce()).
 */
void processEvents();

}  // namespace webtransport
}  // namespace mystral
