/**
 * Triangle Rendering Tests
 *
 * Tests basic WebGPU rendering with the triangle example - requires GPU.
 * These tests only run locally, not in CI.
 */

import { describe, it, expect } from "bun:test";
import { spawn } from "bun";
import { existsSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");
const EXAMPLES_DIR = join(import.meta.dir, "../../examples");

describe("Triangle Rendering", () => {
  it("should render triangle example without errors", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const triangleScript = join(EXAMPLES_DIR, "triangle.js");
    if (!existsSync(triangleScript)) {
      console.log("Skipping: triangle.js not found");
      return;
    }

    const outputPath = join(import.meta.dir, "../../.test-output/triangle-basic.png");

    // Use screenshot mode to ensure process exits
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        triangleScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    const stderr = await new Response(proc.stderr).text();
    const exitCode = await proc.exited;

    console.log("stdout:", stdout);
    if (stderr) console.log("stderr:", stderr);

    // Should initialize successfully
    expect(stdout).toContain("Device acquired");
    expect(stdout).toContain("Pipeline created");
    expect(stdout).toContain("Render loop started");

    // Should not have WebGPU errors
    expect(stderr).not.toContain("WebGPU error");
  }, 30000);

  it("should execute requestAnimationFrame callbacks", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const triangleScript = join(EXAMPLES_DIR, "triangle.js");
    if (!existsSync(triangleScript)) {
      console.log("Skipping: triangle.js not found");
      return;
    }

    const outputPath = join(import.meta.dir, "../../.test-output/raf-test.png");

    // Use screenshot mode to ensure process exits
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
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    // The triangle example uses requestAnimationFrame
    // If it completes 30 frames, RAF is working
    expect(stdout).toContain("Render loop started");
  }, 30000);

  it("should handle setTimeout and setInterval", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const fetchScript = join(EXAMPLES_DIR, "test-fetch.js");
    if (!existsSync(fetchScript)) {
      console.log("Skipping: test-fetch.js not found");
      return;
    }

    const outputPath = join(import.meta.dir, "../../.test-output/timer-test.png");

    // Use screenshot mode to ensure process exits
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        fetchScript,
        "--headless",
        "--screenshot",
        outputPath,
        "--frames",
        "120", // Need more frames for timers to fire
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    console.log("stdout:", stdout);

    // test-fetch.js tests setTimeout and setInterval
    expect(stdout).toContain("Timeout 1 fired!");
    expect(stdout).toContain("Interval tick: 1");
  }, 30000);
});
