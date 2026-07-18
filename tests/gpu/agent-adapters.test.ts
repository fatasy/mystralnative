import { describe, expect, it } from "bun:test";
import { existsSync } from "node:fs";
import { createServer } from "node:net";
import { join, resolve } from "node:path";
import { MystralDebugClient } from "../../tools/lib/mystral-debug-client";

const ROOT = resolve(import.meta.dir, "../..");
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

describe("Semantic agent adapters", () => {
  it("exposes bounded scene, camera, and performance contracts", async () => {
    if (!existsSync(BINARY)) return;
    const port = await freePort();
    const runtime = Bun.spawn([
      BINARY, "run", FIXTURE, "--no-sdl", "--debug-port", String(port),
    ], { cwd: ROOT, stdout: "pipe", stderr: "pipe" });
    const client = MystralDebugClient.forPort(port);

    try {
      const deadline = Date.now() + 20_000;
      while (true) {
        try { await client.request("hello"); break; }
        catch {
          if (Date.now() >= deadline) throw new Error("Debug server did not start");
          await Bun.sleep(100);
        }
      }

      const list: any = await client.request("agent.list");
      expect(list.entities.map((entry: any) => entry.id)).toEqual([
        "camera.active", "performance.live", "scene.main", "scene.node",
      ]);
      expect(list.actions.map((entry: any) => entry.id)).toEqual([
        "camera.focus-entity", "camera.look-at", "camera.set-transform",
      ]);

      const firstPage: any = await client.request("agent.inspect", {
        id: "scene.main", input: { depth: 3, limit: 2 },
      });
      expect(firstPage.snapshot).toMatchObject({ total: 3, nextCursor: 2 });
      expect(firstPage.snapshot.nodes.map((node: any) => node.id)).toEqual(["world.root", "actor.player"]);

      const secondPage: any = await client.request("agent.inspect", {
        id: "scene.main", input: { depth: 3, limit: 2, cursor: 2 },
      });
      expect(secondPage.snapshot.nodes[0]).toMatchObject({
        id: "item.sword", stability: "authored", parentId: "actor.player",
      });

      const camera: any = await client.request("agent.inspect", { id: "camera.active" });
      expect(camera.snapshot.position).toEqual({ x: 0, y: 2, z: 5 });
      await client.request("agent.act", {
        id: "camera.set-transform", input: { position: { x: 5, y: 6, z: 7 } },
      });
      const moved: any = await client.request("agent.inspect", { id: "camera.active" });
      expect(moved.snapshot.position).toEqual({ x: 5, y: 6, z: 7 });

      const performance: any = await client.request("agent.inspect", { id: "performance.live" });
      expect(performance.snapshot.renderer.render).toMatchObject({ calls: 4, triangles: 12 });
      expect(performance.snapshot.runtime.heapLimitBytes).toBeGreaterThan(0);

      const started: any = await client.request("profile.start", { frames: 10 });
      // A fixed-size profile must stop collecting at its own bound even if the
      // controller sends profile.stop a couple of frames late.
      await client.request("waitForFrame", { frame: started.targetFrame + 2, timeoutMs: 10_000 });
      const profile: any = await client.request("profile.stop");
      expect(profile.sampledFrames).toBe(10);
      expect(profile.workers.activeStart).toBe(0);
      expect(profile.workers.activeEnd).toBe(0);

      const metrics: any = await client.request("metrics.snapshot");
      expect(metrics.value.runtime.workersActive).toBe(0);
      expect(metrics.value.runtime.sharedMemoryBytes).toBe(0);

      await client.request("quit");
    } finally {
      client.close();
      runtime.kill();
    }
  }, 30_000);
});
