#!/usr/bin/env bun

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import * as z from "zod/v4";
import { MystralDebugClient } from "./lib/mystral-debug-client";

type JsonObject = Record<string, unknown>;

const args = Bun.argv.slice(2);
const portIndex = args.indexOf("--port");
const port = portIndex >= 0 ? Number(args[portIndex + 1]) : Number(process.env.MYSTRAL_DEBUG_PORT || 9222);
const allowControl = args.includes("--allow-control");
const unsafeEvaluate = args.includes("--unsafe-evaluate");

if (!Number.isFinite(port) || port < 1 || port > 65535) {
  console.error("Invalid Mystral debug port");
  process.exit(1);
}

const client = MystralDebugClient.forPort(port);
const server = new McpServer(
  { name: "mystral-native", version: "0.1.5" },
  {
    instructions:
      "Use scene and camera inspection before taking game actions. " +
      "Scene mutation, raw input, quit, and JavaScript evaluation may be unavailable by design.",
  },
);

function objectResult(value: unknown) {
  const structuredContent: JsonObject = value && typeof value === "object" && !Array.isArray(value)
    ? { ...(value as JsonObject) }
    : { value };
  return {
    content: [{ type: "text" as const, text: JSON.stringify(value, null, 2) }],
    structuredContent,
  };
}

function unwrapEvaluation(value: any): any {
  if (value?.ok === false) throw new Error(value.error?.message || "Runtime evaluation failed");
  return value?.ok === true && "value" in value ? value.value : value;
}

function numericDelta(before: any, after: any): any {
  if (typeof before === "number" && typeof after === "number") return after - before;
  if (!before || !after || typeof before !== "object" || typeof after !== "object") return null;
  const result: JsonObject = {};
  for (const key of Object.keys(after)) {
    const delta = numericDelta(before[key], after[key]);
    if (delta !== null) result[key] = delta;
  }
  return result;
}

async function optionalInspector(id: string): Promise<any> {
  try {
    return await client.request("agent.inspect", { id });
  } catch {
    return null;
  }
}

server.registerTool("mystral_status", {
  description: "Get runtime capabilities, current frame, and pause state.",
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async () => objectResult({
  capabilities: await client.request("hello"),
  runtime: await client.request("getFrameCount"),
}));

server.registerTool("mystral_semantic_list", {
  description: "List semantic entities and explicitly exposed game actions.",
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async () => objectResult(await client.request("agent.list")));

server.registerTool("mystral_scene_snapshot", {
  description: "Read a bounded, paginated semantic scene snapshot.",
  inputSchema: {
    rootId: z.string().optional(),
    depth: z.number().int().min(0).max(8).default(2),
    limit: z.number().int().min(1).max(500).default(200),
    cursor: z.number().int().min(0).default(0),
    includeHidden: z.boolean().default(false),
  },
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async (input) => objectResult(await client.request("agent.inspect", {
  id: "scene.main",
  input,
})));

server.registerTool("mystral_scene_node", {
  description: "Inspect one semantic scene node by stable ID.",
  inputSchema: { id: z.string().min(1) },
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async ({ id }) => objectResult(await client.request("agent.inspect", {
  id: "scene.node",
  input: { id },
})));

server.registerTool("mystral_camera_get", {
  description: "Read the active camera transform and projection settings.",
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async () => objectResult(await client.request("agent.inspect", { id: "camera.active" })));

server.registerTool("mystral_camera_control", {
  description: "Run an explicitly enabled semantic camera action.",
  inputSchema: {
    action: z.enum(["set-transform", "look-at", "focus-entity"]),
    position: z.object({ x: z.number(), y: z.number(), z: z.number() }).optional(),
    quaternion: z.object({ x: z.number(), y: z.number(), z: z.number(), w: z.number() }).optional(),
    target: z.object({ x: z.number(), y: z.number(), z: z.number() }).optional(),
    id: z.string().optional(),
  },
  annotations: { readOnlyHint: false, idempotentHint: false },
}, async ({ action, ...input }) => objectResult(await client.request("agent.act", {
  id: `camera.${action}`,
  input,
})));

server.registerTool("mystral_game_action", {
  description: "Run one action explicitly exposed by the game through mystralAgent.",
  inputSchema: { id: z.string().min(1), input: z.unknown().optional() },
  annotations: { readOnlyHint: false, idempotentHint: false },
}, async ({ id, input }) => objectResult(await client.request("agent.act", { id, input })));

server.registerTool("mystral_performance_sample", {
  description: "Profile an exact number of live game frames and return runtime and WebGPU deltas.",
  inputSchema: {
    frames: z.number().int().min(1).max(3600).default(120),
    timeoutMs: z.number().int().min(100).max(120000).default(30000),
  },
  annotations: { readOnlyHint: false, idempotentHint: false },
}, async ({ frames, timeoutMs }) => {
  const metricsBefore = unwrapEvaluation(await client.request("metrics.snapshot"));
  const semanticBefore = await optionalInspector("performance.live");
  const started: any = await client.request("profile.start", { frames });
  let profile: any;
  try {
    await client.request("waitForFrame", { frame: started.targetFrame, timeoutMs });
    profile = await client.request("profile.stop");
  } catch (error) {
    try { await client.request("profile.stop"); } catch {}
    throw error;
  }
  const metricsAfter = unwrapEvaluation(await client.request("metrics.snapshot"));
  const semanticAfter = await optionalInspector("performance.live");
  return objectResult({
    profile,
    metricsBefore,
    metricsAfter,
    metricsDelta: numericDelta(metricsBefore, metricsAfter),
    semanticBefore,
    semanticAfter,
  });
});

server.registerTool("mystral_screenshot", {
  description: "Capture the current native WebGPU frame as a PNG image.",
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async () => {
  const screenshot: any = await client.request("screenshot");
  return {
    content: [
      { type: "image" as const, data: screenshot.data, mimeType: "image/png" },
      { type: "text" as const, text: `${screenshot.width}x${screenshot.height}` },
    ],
    structuredContent: { width: screenshot.width, height: screenshot.height },
  };
});

server.registerTool("mystral_logs", {
  description: "Read buffered runtime console entries after an optional sequence number.",
  inputSchema: { since: z.number().int().min(0).default(0) },
  annotations: { readOnlyHint: true, idempotentHint: true },
}, async ({ since }) => objectResult(await client.request("getLogs", { since })));

if (allowControl) {
  server.registerTool("mystral_runtime_control", {
    description: "Pause, resume, step, inject a key press, or stop the controlled runtime.",
    inputSchema: {
      action: z.enum(["pause", "resume", "step", "press", "quit"]),
      count: z.number().int().min(1).max(1000).optional(),
      key: z.string().optional(),
    },
    annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: false },
  }, async ({ action, count, key }) => {
    if (action === "step") return objectResult(await client.request("stepFrames", { count: count || 1 }));
    if (action === "press") return objectResult(await client.request("keyboard.press", { key }));
    return objectResult(await client.request(action));
  });
}

if (unsafeEvaluate) {
  server.registerTool("mystral_evaluate", {
    description: "UNSAFE: evaluate arbitrary JavaScript inside the running game.",
    inputSchema: { expression: z.string().min(1) },
    annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: false },
  }, async ({ expression }) => objectResult(await client.request("evaluate", { expression })));
}

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error(`[Mystral MCP] stdio server ready for ws://127.0.0.1:${port}`);
}

process.on("SIGINT", () => { client.close(); process.exit(0); });
process.on("SIGTERM", () => { client.close(); process.exit(0); });

main().catch((error) => {
  console.error("[Mystral MCP]", error);
  process.exit(1);
});
