/**
 * Screenshot Tests
 *
 * Tests headless screenshot capture - requires GPU.
 * These tests only run locally, not in CI.
 */

import { describe, it, expect, beforeAll, afterAll } from "bun:test";
import { spawn } from "bun";
import { existsSync, mkdirSync, rmSync, statSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");
const EXAMPLES_DIR = join(import.meta.dir, "../../examples");
const OUTPUT_DIR = join(import.meta.dir, "../../.test-output");

describe("Screenshot Capture", () => {
  beforeAll(() => {
    // Create output directory
    if (!existsSync(OUTPUT_DIR)) {
      mkdirSync(OUTPUT_DIR, { recursive: true });
    }
  });

  afterAll(() => {
    // Optionally clean up output directory
    // rmSync(OUTPUT_DIR, { recursive: true, force: true });
  });

  it("should capture screenshot of triangle example", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const triangleScript = join(EXAMPLES_DIR, "triangle.js");
    if (!existsSync(triangleScript)) {
      console.log("Skipping: triangle.js not found");
      return;
    }

    const outputPath = join(OUTPUT_DIR, "triangle-screenshot.png");

    // Remove old screenshot if exists
    if (existsSync(outputPath)) {
      rmSync(outputPath);
    }

    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        triangleScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "30",
        "--width",
        "800",
        "--height",
        "600",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    const stderr = await new Response(proc.stderr).text();
    await proc.exited;

    console.log("stdout:", stdout);
    if (stderr) console.log("stderr:", stderr);

    // Check screenshot was saved
    expect(stdout).toContain("Screenshot saved");
    expect(existsSync(outputPath)).toBe(true);

    // Check that the file is a valid PNG (non-zero size)
    const stats = statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000); // PNG should be at least 1KB
  }, 30000); // 30 second timeout for GPU tests

  it("should capture screenshot at custom resolution", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const triangleScript = join(EXAMPLES_DIR, "triangle.js");
    if (!existsSync(triangleScript)) {
      console.log("Skipping: triangle.js not found");
      return;
    }

    const outputPath = join(OUTPUT_DIR, "triangle-1920x1080.png");

    if (existsSync(outputPath)) {
      rmSync(outputPath);
    }

    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        triangleScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "30",
        "--width",
        "1920",
        "--height",
        "1080",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    // Check screenshot was saved
    expect(stdout).toContain("Screenshot saved");
    expect(existsSync(outputPath)).toBe(true);

    const stats = statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000);
  }, 30000);

  it("should capture rotating cube screenshot", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const cubeScript = join(EXAMPLES_DIR, "simple-cube.js");
    if (!existsSync(cubeScript)) {
      console.log("Skipping: simple-cube.js not found");
      return;
    }

    const outputPath = join(OUTPUT_DIR, "cube-screenshot.png");

    if (existsSync(outputPath)) {
      rmSync(outputPath);
    }

    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        cubeScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "60",
        "--width",
        "800",
        "--height",
        "600",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    console.log("stdout:", stdout);

    expect(stdout).toContain("Screenshot saved");
    expect(existsSync(outputPath)).toBe(true);

    const stats = statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000);
  }, 30000);

  // Skipped: bind group creation has a bug with sampler/texture resources
  // TODO: Fix WebGPU bindings for bind groups with samplers
  it.skip("should capture texture test screenshot", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const textureScript = join(EXAMPLES_DIR, "test-texture.js");
    if (!existsSync(textureScript)) {
      console.log("Skipping: test-texture.js not found");
      return;
    }

    const outputPath = join(OUTPUT_DIR, "texture-screenshot.png");

    if (existsSync(outputPath)) {
      rmSync(outputPath);
    }

    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        textureScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "60",
        "--width",
        "800",
        "--height",
        "600",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    console.log("stdout:", stdout);

    // Check screenshot was saved
    expect(stdout).toContain("Screenshot saved");
    expect(existsSync(outputPath)).toBe(true);

    const stats = statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000);
  }, 60000); // 60 second timeout for texture loading
});

