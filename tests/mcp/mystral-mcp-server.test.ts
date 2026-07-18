import { describe, expect, it } from "bun:test";
import { existsSync } from "node:fs";
import { createServer } from "node:net";
import { join, resolve } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { MystralDebugClient } from "../../tools/lib/mystral-debug-client";

const ROOT = resolve(import.meta.dir, "../..");
const MCP_SERVER = join(ROOT, "tools", "mystral-mcp-server.ts");
const FIXTURE = join(ROOT, "tests", "gpu", "fixtures", "agent-adapter.mjs");
const BINARY_NAME = process.platform === "win32" ? "mystral.exe" : "mystral";
const BINARY = process.env.MYSTRAL_BIN || join(ROOT, "build-agent", "Release", BINARY_NAME);

async function freePort(): Promise<number> {
  return await new Promise((resolvePort, reject) => {
    const server = createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      if (!address || typeof address === "string") return reject(new Error("No test port"));
      server.close(() => resolvePort(address.port));
    });
  });
}

describe("Mystral MCP server", () => {
  it("maps MCP tools to the live native semantic protocol", async () => {
    if (!existsSync(BINARY)) return;
    const port = await freePort();
    const runtime = Bun.spawn([
      BINARY, "run", FIXTURE, "--no-sdl", "--debug-port", String(port),
    ], { cwd: ROOT, stdout: "pipe", stderr: "pipe" });
    const debug = MystralDebugClient.forPort(port);
    let mcp: Client | undefined;

    try {
      const deadline = Date.now() + 20_000;
      while (true) {
        try { await debug.request("hello"); break; }
        catch {
          if (Date.now() >= deadline) throw new Error("Debug server did not start");
          await Bun.sleep(100);
        }
      }

      mcp = new Client({ name: "mystral-mcp-test", version: "1.0.0" });
      const transport = new StdioClientTransport({
        command: process.execPath,
        args: [MCP_SERVER, "--port", String(port)],
        cwd: ROOT,
        stderr: "pipe",
      });
      await mcp.connect(transport);

      const tools = await mcp.listTools();
      const names = tools.tools.map((tool) => tool.name);
      expect(names).toContain("mystral_scene_snapshot");
      expect(names).toContain("mystral_performance_sample");
      expect(names).not.toContain("mystral_evaluate");
      expect(names).not.toContain("mystral_runtime_control");

      const scene: any = await mcp.callTool({
        name: "mystral_scene_snapshot",
        arguments: { depth: 3, limit: 10 },
      });
      expect(scene.structuredContent.snapshot.total).toBe(3);

      const profile: any = await mcp.callTool({
        name: "mystral_performance_sample",
        arguments: { frames: 5, timeoutMs: 10_000 },
      });
      expect(profile.structuredContent.profile.sampledFrames).toBe(5);
      expect(profile.structuredContent.metricsDelta.runtime.frame).toBeGreaterThanOrEqual(5);

      await debug.request("quit");
    } finally {
      await mcp?.close();
      debug.close();
      runtime.kill();
    }
  }, 40_000);
});
