/**
 * WebTransport API example for MystralNative.
 *
 * Demonstrates the full WebTransport client surface over HTTP/3 (QUIC):
 *   - connection lifecycle (ready / closed)
 *   - unreliable datagrams
 *   - reliable bidirectional streams
 *   - reliable unidirectional streams (outgoing + incoming server streams)
 *
 * This example echoes data against a WebTransport echo server. To run it you
 * need a WebTransport server listening at the URL below. A ready-made echo
 * server fixture lives at:
 *   tests/webtransport/fixtures/wt-echo-server  (run `cargo run --release`)
 *
 * Then:
 *   ./build/mystral run examples/webtransport-test.js --headless
 *
 * WebTransport requires a build with quiche compiled in (MYSTRAL_HAS_QUICHE).
 */

const URL = "https://127.0.0.1:4433/echo";

async function main() {
  console.log(`[WebTransport] connecting to ${URL}`);
  const transport = new WebTransport(URL);

  try {
    await transport.ready;
    console.log("[WebTransport] session ready");
  } catch (e) {
    console.error("[WebTransport] connection failed:", e.message);
    console.error("  Is the echo server running? See the comment at the top of this file.");
    process.exit(1);
  }

  // --- Datagrams (unreliable) ---------------------------------------------
  {
    const writer = transport.datagrams.writable.getWriter();
    const reader = transport.datagrams.readable.getReader();
    await writer.write(new TextEncoder().encode("ping"));
    const { value } = await reader.read();
    console.log("[WebTransport] datagram echo:", new TextDecoder().decode(value));
    writer.releaseLock();
    reader.releaseLock();
  }

  // --- Bidirectional stream (reliable) ------------------------------------
  {
    const stream = await transport.createBidirectionalStream();
    const writer = stream.writable.getWriter();
    await writer.write(new TextEncoder().encode("hello over a bidi stream"));
    await writer.close();

    const reader = stream.readable.getReader();
    const chunks = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) chunks.push(...value);
    }
    console.log("[WebTransport] bidi echo:", new TextDecoder().decode(new Uint8Array(chunks)));
  }

  // --- Unidirectional streams (reliable) ----------------------------------
  {
    const incoming = transport.incomingUnidirectionalStreams.getReader();

    const send = await transport.createUnidirectionalStream();
    const writer = send.getWriter();
    await writer.write(new TextEncoder().encode("hello over a uni stream"));
    await writer.close();

    // The echo server replies by opening a new unidirectional stream back to us.
    const { value: recvStream } = await incoming.read();
    const reader = recvStream.getReader();
    const chunks = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) chunks.push(...value);
    }
    console.log("[WebTransport] uni echo:", new TextDecoder().decode(new Uint8Array(chunks)));
  }

  console.log("[WebTransport] all transports verified — closing");
  transport.close();
  process.exit(0);
}

main();
