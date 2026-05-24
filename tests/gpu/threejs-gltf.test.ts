/**
 * Three.js GLTF + GLB Loading Test (GPU)
 *
 * Verifies that Three.js' own GLTFLoader can load BOTH a .glb (binary glTF)
 * and a .gltf (JSON glTF + .bin + JPG textures) on MystralNative, and render
 * them with the WebGPU renderer.
 *
 * Regression coverage for: "ReferenceError: AbortController is not defined".
 * Three.js' FileLoader (r168+) constructs an AbortController and wraps the URL
 * in a Request before calling fetch(); embedded/external textures are fetched
 * via blob: URLs. All three of those (AbortController, Request, blob: fetch)
 * are polyfilled in src/runtime.cpp.
 *
 * Requires a GPU - runs locally, not in CI.
 */

import { describe, it, expect, beforeAll } from "bun:test";
import { spawn } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import { join } from "path";

const ROOT = join(import.meta.dir, "../..");
const MYSTRAL_BIN = join(ROOT, "build/mystral");
const EXAMPLE = join(ROOT, "examples/threejs-gltf.js");
const OUTPUT_DIR = join(ROOT, ".test-output");

describe("Three.js GLTF/GLB loading", () => {
  beforeAll(() => {
    if (!existsSync(OUTPUT_DIR)) mkdirSync(OUTPUT_DIR, { recursive: true });
  });

  it("loads a GLB and a GLTF via Three.js GLTFLoader and renders them", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }
    if (!existsSync(EXAMPLE)) {
      console.log("Skipping: threejs-gltf.js bundle not found");
      return;
    }

    const outputPath = join(OUTPUT_DIR, "threejs-gltf.png");
    if (existsSync(outputPath)) rmSync(outputPath);

    const proc = spawn({
      // Run from the repo root so the ./examples/assets/* paths resolve.
      cwd: ROOT,
      cmd: [
        MYSTRAL_BIN,
        "run",
        EXAMPLE,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "120",
        "--width",
        "1280",
        "--height",
        "720",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    const stderr = await new Response(proc.stderr).text();
    await proc.exited;

    const combined = stdout + stderr;
    if (combined) console.log(combined);

    // The original bug: GLTFLoader threw because AbortController was missing.
    expect(combined).not.toContain("AbortController is not defined");
    expect(combined).not.toContain("Request is not defined");
    expect(combined).not.toContain("Couldn't load texture");

    // Both formats must load through Three.js.
    expect(stdout).toContain("GLB loaded OK");
    expect(stdout).toContain("GLTF loaded OK");

    // And a frame must actually render.
    expect(combined).toContain("Screenshot saved");
    expect(existsSync(outputPath)).toBe(true);
    const stats = statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000);
  }, 60000);
});
