# WebTransport example

A runnable, end-to-end demo of MystralNative's [WebTransport](https://developer.mozilla.org/en-US/docs/Web/API/WebTransport_API)
client (HTTP/3 over QUIC, backed by [Cloudflare quiche](https://github.com/cloudflare/quiche)).

It exercises the full client surface against a local echo server:

- connection lifecycle (`ready` / `closed`)
- unreliable **datagrams**
- reliable **bidirectional** streams
- reliable **unidirectional** streams (outgoing + the server's echo stream back)

```
webtransport/
├── webtransport-test.js   ← the example (runs under the mystral runtime)
├── server/                ← a small self-contained Rust echo server
│   ├── Cargo.toml
│   └── src/main.rs
└── README.md              ← you are here
```

## Requirements

- A `mystral` binary built **with quiche** (`MYSTRAL_HAS_QUICHE`). quiche is a
  prebuilt dependency — fetch it with `node scripts/download-deps.mjs --only quiche`
  before configuring CMake. If quiche is not compiled in, `new WebTransport(...)`
  rejects its `ready` promise instead of connecting.
- A Rust toolchain (`cargo`) to build the echo server.

## Running it

From the repo root, in two terminals:

```bash
# Terminal 1 — start the echo server (binds 127.0.0.1:4433)
cargo run --release --manifest-path examples/webtransport/server/Cargo.toml

# Terminal 2 — run the example against it
./build/mystral run examples/webtransport/webtransport-test.js --headless
```

Expected output from the example:

```
[WebTransport] connecting to https://127.0.0.1:4433/echo
[WebTransport] session ready
[WebTransport] datagram echo: ping
[WebTransport] bidi echo: hello over a bidi stream
[WebTransport] uni echo: hello over a uni stream
[WebTransport] all transports verified — closing
```

## The echo server

`server/` is an ~180-line Rust program built on the [`wtransport`](https://crates.io/crates/wtransport)
crate. It listens on `127.0.0.1:4433`, accepts a WebTransport session on any
path, and echoes back every datagram, unidirectional stream, and bidirectional
stream it receives.

It uses a **self-signed** certificate (valid for `localhost` / `127.0.0.1`) and
prints the certificate's SHA-256 hash on startup. For local/dev use the client
currently connects without verifying the peer certificate; pinning a self-signed
cert from JS via `serverCertificateHashes` is planned (see the
[WebTransport guide](../../docs/docs/guides/webtransport.mdx)).

## Automated test

The same server backs the e2e suite, which builds it, starts it, and asserts the
round-trips:

```bash
bun test tests/webtransport
```
