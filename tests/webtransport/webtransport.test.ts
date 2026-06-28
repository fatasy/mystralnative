/**
 * WebTransport API end-to-end tests.
 *
 * These exercise the native WebTransport implementation (QUIC + HTTP/3 via
 * quiche) against a real WebTransport echo server (the small Rust `wtransport`
 * server shipped at `examples/webtransport/server`). They validate the full
 * client surface:
 *   - connection lifecycle (ready)
 *   - datagrams (send + receive echo)
 *   - bidirectional streams (send + receive echo)
 *   - unidirectional streams (send + receive a server-initiated echo stream)
 *
 * Requirements (the suite skips cleanly if any are missing):
 *   - The `mystral` binary built WITH quiche (MYSTRAL_HAS_QUICHE). WebTransport
 *     is feature-detected at runtime by attempting a connection.
 *   - A Rust toolchain (`cargo`) to build the echo server.
 *
 * Because they need a Rust toolchain and a live UDP server, these are NOT part
 * of the default CI `bun test tests/ci` run; run them explicitly with
 * `bun test tests/webtransport`.
 */

import { describe, it, expect, beforeAll, afterAll } from "bun:test";
import { spawn, spawnSync } from "bun";
import { existsSync, mkdirSync, writeFileSync, rmSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");
// The echo server lives with the runnable example so users can verify
// WebTransport themselves (see examples/webtransport/README.md).
const SERVER_DIR = join(import.meta.dir, "../../examples/webtransport/server");
const SERVER_BIN = join(SERVER_DIR, "target/release/wt-echo-server");
const TEST_DIR = join(import.meta.dir, "../../.test-tmp/webtransport");
const SERVER_URL = "https://127.0.0.1:4433/echo";

const hasMystral = existsSync(MYSTRAL_BIN);
const hasCargo = spawnSync(["cargo", "--version"]).exitCode === 0;

// Whether the binary was built with WebTransport support (quiche compiled in).
let webTransportSupported = false;

let serverProc: ReturnType<typeof spawn> | null = null;

async function startServer(): Promise<boolean> {
  if (!existsSync(SERVER_BIN)) {
    console.log("Building WebTransport echo server (cargo)...");
    const build = spawnSync(["cargo", "build", "--release"], { cwd: SERVER_DIR });
    if (build.exitCode !== 0) {
      console.log("Echo server build failed:", build.stderr?.toString());
      return false;
    }
  }
  serverProc = spawn({ cmd: [SERVER_BIN], stdout: "pipe", stderr: "pipe" });

  // Wait for the "LISTENING" line so we know the UDP socket is bound.
  const reader = serverProc.stdout.getReader();
  const decoder = new TextDecoder();
  const deadline = Date.now() + 15000;
  let buffer = "";
  while (Date.now() < deadline) {
    const { value, done } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value);
    if (buffer.includes("LISTENING")) {
      reader.releaseLock();
      return true;
    }
  }
  reader.releaseLock();
  return false;
}

// Runs a JS script under the mystral runtime (headless) and returns stdout.
async function runScript(name: string, source: string): Promise<string> {
  if (!existsSync(TEST_DIR)) mkdirSync(TEST_DIR, { recursive: true });
  const path = join(TEST_DIR, name);
  writeFileSync(path, source);
  const proc = spawn({
    cmd: [MYSTRAL_BIN, "run", path, "--headless"],
    stdout: "pipe",
    stderr: "pipe",
  });
  const timer = setTimeout(() => proc.kill(), 30000);
  const stdout = await new Response(proc.stdout).text();
  await proc.exited;
  clearTimeout(timer);
  return stdout;
}

describe("WebTransport API", () => {
  beforeAll(async () => {
    if (!hasMystral) {
      console.log("Skipping: mystral binary not found (run 'bun run build').");
      return;
    }

    // Feature-detect WebTransport support: the global exists in all builds, but a
    // connection only initiates when quiche is compiled in.
    const probe = await runScript(
      "wt-probe.js",
      `console.log('WT_GLOBAL:' + (typeof WebTransport));
       const wt = new WebTransport('https://127.0.0.1:4433/probe');
       wt.ready.then(() => {}).catch(() => {});
       console.log('WT_CONSTRUCT_OK');
       process.exit(0);`,
    );
    webTransportSupported = probe.includes("WT_CONSTRUCT_OK");

    if (!webTransportSupported) {
      console.log("Skipping: build has no WebTransport (quiche) support.");
      return;
    }
    if (!hasCargo) {
      console.log("Skipping: cargo not available to build the echo server.");
      return;
    }
    const started = await startServer();
    if (!started) {
      console.log("Skipping: could not start the WebTransport echo server.");
      webTransportSupported = false;
    }
  });

  afterAll(() => {
    serverProc?.kill();
    try {
      rmSync(TEST_DIR, { recursive: true, force: true });
    } catch {}
  });

  const guard = () => hasMystral && hasCargo && webTransportSupported;

  it("connects and the ready promise fulfills", async () => {
    if (!guard()) return;
    const out = await runScript(
      "wt-ready.js",
      `async function main() {
        const wt = new WebTransport('${SERVER_URL}');
        try { await wt.ready; console.log('PASS: ready'); }
        catch (e) { console.log('FAIL: ' + e.message); }
        process.exit(0);
      }
      main();`,
    );
    expect(out).toContain("PASS: ready");
  });

  it("echoes a datagram", async () => {
    if (!guard()) return;
    const out = await runScript(
      "wt-datagram.js",
      `async function main() {
        const wt = new WebTransport('${SERVER_URL}');
        await wt.ready;
        const writer = wt.datagrams.writable.getWriter();
        const reader = wt.datagrams.readable.getReader();
        await writer.write(new Uint8Array([1, 2, 3, 4, 5]));
        const { value } = await reader.read();
        if (value && value.length === 5 && value[0] === 1 && value[4] === 5) {
          console.log('PASS: datagram ' + Array.from(value).join(','));
        } else {
          console.log('FAIL: datagram ' + (value ? Array.from(value).join(',') : 'none'));
        }
        process.exit(0);
      }
      main();`,
    );
    expect(out).toContain("PASS: datagram 1,2,3,4,5");
  });

  it("echoes a bidirectional stream", async () => {
    if (!guard()) return;
    const out = await runScript(
      "wt-bidi.js",
      `async function main() {
        const wt = new WebTransport('${SERVER_URL}');
        await wt.ready;
        const stream = await wt.createBidirectionalStream();
        const writer = stream.writable.getWriter();
        const reader = stream.readable.getReader();
        await writer.write(new TextEncoder().encode('hello bidi'));
        await writer.close();
        let bytes = [];
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) bytes.push(...value);
        }
        const text = new TextDecoder().decode(new Uint8Array(bytes));
        console.log(text === 'hello bidi' ? 'PASS: bidi ' + text : 'FAIL: bidi ' + JSON.stringify(text));
        process.exit(0);
      }
      main();`,
    );
    expect(out).toContain("PASS: bidi hello bidi");
  });

  it("echoes a unidirectional stream", async () => {
    if (!guard()) return;
    const out = await runScript(
      "wt-uni.js",
      `async function main() {
        const wt = new WebTransport('${SERVER_URL}');
        await wt.ready;
        const incoming = wt.incomingUnidirectionalStreams.getReader();
        const send = await wt.createUnidirectionalStream();
        const writer = send.getWriter();
        await writer.write(new TextEncoder().encode('hello uni'));
        await writer.close();
        const { value: recvStream } = await incoming.read();
        if (!recvStream) { console.log('FAIL: no incoming uni'); process.exit(0); }
        const reader = recvStream.getReader();
        let bytes = [];
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) bytes.push(...value);
        }
        const text = new TextDecoder().decode(new Uint8Array(bytes));
        console.log(text === 'hello uni' ? 'PASS: uni ' + text : 'FAIL: uni ' + JSON.stringify(text));
        process.exit(0);
      }
      main();`,
    );
    expect(out).toContain("PASS: uni hello uni");
  });
});
