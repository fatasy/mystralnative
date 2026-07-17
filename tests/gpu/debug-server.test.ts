import { describe, expect, it } from "bun:test";
import { existsSync } from "node:fs";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "../..");
const CLIENT = join(ROOT, "tools", "mystral-debug-client.ts");
const TRIANGLE = join(ROOT, "examples", "triangle.js");
const BINARY_NAME = process.platform === "win32" ? "mystral.exe" : "mystral";
const BINARY_CANDIDATES = [
  process.env.MYSTRAL_BIN,
  join(ROOT, "build-agent", "Release", BINARY_NAME),
  join(ROOT, "build-agent", BINARY_NAME),
  join(ROOT, "build", "Release", BINARY_NAME),
  join(ROOT, "build", BINARY_NAME),
].filter((value): value is string => Boolean(value));

function findBinary(): string | undefined {
  return BINARY_CANDIDATES.find(existsSync);
}

async function freePort(): Promise<number> {
  return await new Promise((resolvePort, reject) => {
    const server = createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      if (!address || typeof address === "string") {
        server.close();
        reject(new Error("Could not reserve a debug port"));
        return;
      }
      server.close(() => resolvePort(address.port));
    });
  });
}

async function runClient(port: number, ...args: string[]): Promise<any> {
  const child = Bun.spawn([process.execPath, CLIENT, ...args, "--port", String(port)], {
    cwd: ROOT,
    stdout: "pipe",
    stderr: "pipe",
  });
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(child.stdout).text(),
    new Response(child.stderr).text(),
    child.exited,
  ]);
  if (exitCode !== 0) {
    throw new Error(stderr || stdout || `Client exited with ${exitCode}`);
  }
  return JSON.parse(stdout);
}

async function waitUntilReady(port: number): Promise<void> {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    try {
      await runClient(port, "hello");
      return;
    } catch {
      await Bun.sleep(200);
    }
  }
  throw new Error("Debug server did not become ready");
}

describe("Debug server automation", () => {
  it("supports visual control and semantic game integration", async () => {
    const binary = findBinary();
    if (!binary) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const port = await freePort();
    const screenshot = join(tmpdir(), `mystral-debug-${process.pid}.png`);
    const runtime = Bun.spawn([
      binary,
      "run",
      TRIANGLE,
      "--headless",
      "--debug-port",
      String(port),
      "--width",
      "640",
      "--height",
      "360",
    ], {
      cwd: ROOT,
      stdout: "pipe",
      stderr: "pipe",
    });

    try {
      await waitUntilReady(port);

      const capabilities = await runClient(port, "hello");
      expect(capabilities.protocolVersion).toBe(1);
      expect(capabilities.agentBridgeVersion).toBe(1);
      expect(capabilities.methods).toContain("agent.inspect");

      const evaluation = await runClient(
        port,
        "evaluate",
        "({ answer: 6 * 7, text: 'quoted value', nested: { ok: true } })",
      );
      expect(evaluation.ok).toBe(true);
      expect(evaluation.value).toEqual({
        answer: 42,
        text: "quoted value",
        nested: { ok: true },
      });

      const registration = await runClient(
        port,
        "evaluate",
        `globalThis.__agentState = { health: 100, position: { x: 2, y: 3 } };
         mystralAgent.exposeInspector("player", () => globalThis.__agentState, {
           label: "Player", kind: "actor", description: "Live player state"
         });
         mystralAgent.exposeAction("player.damage", (input) => {
           globalThis.__agentState.health -= Number(input.amount);
           return { health: globalThis.__agentState.health };
         }, {
           entityId: "player", description: "Apply damage",
           inputSchema: { type: "object", required: ["amount"] }
         });
         true`,
      );
      expect(registration.value).toBe(true);

      const exposed = await runClient(port, "agent-list");
      expect(exposed.entities).toEqual([{
        id: "player",
        label: "Player",
        kind: "actor",
        description: "Live player state",
      }]);
      expect(exposed.actions[0]).toMatchObject({
        id: "player.damage",
        entityId: "player",
        description: "Apply damage",
      });

      const beforeAction = await runClient(port, "agent-inspect", "player");
      expect(beforeAction.snapshot.health).toBe(100);

      const action = await runClient(port, "agent-act", "player.damage", '{"amount":7}');
      expect(action.result).toEqual({ health: 93 });
      const afterAction = await runClient(port, "agent-inspect", "player");
      expect(afterAction.snapshot.health).toBe(93);
      expect(runClient(port, "agent-act", "missing.action")).rejects.toThrow("Unknown action");

      const paused = await runClient(port, "pause");
      await Bun.sleep(150);
      const stillPaused = await runClient(port, "state");
      expect(stillPaused).toEqual(paused);

      const stepped = await runClient(port, "step", "3");
      expect(stepped.frame).toBe(paused.frame + 3);

      const captured = await runClient(port, "screenshot", screenshot);
      expect(captured).toMatchObject({ width: 640, height: 360 });
      expect(existsSync(screenshot)).toBe(true);

      const logs = await runClient(port, "logs");
      expect(logs.entries.some((entry: { message: string }) =>
        entry.message.includes("Triangle example starting"))).toBe(true);

      expect(runClient(port, "raw", "unknown.method")).rejects.toThrow("Unknown method");

      expect(await runClient(port, "quit")).toEqual({ quitting: true });
      expect(await Promise.race([
        runtime.exited.then(() => true),
        Bun.sleep(5_000).then(() => false),
      ])).toBe(true);
    } finally {
      runtime.kill();
    }
  }, 45_000);
});
