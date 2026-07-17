/**
 * CLI Basic Tests
 *
 * Tests CLI argument parsing, help, version - no GPU required.
 * These tests run in CI.
 */

import { describe, it, expect, beforeAll } from "bun:test";
import { spawn } from "bun";
import { existsSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");

describe("CLI Basic", () => {
  beforeAll(() => {
    // Check if mystral binary exists
    if (!existsSync(MYSTRAL_BIN)) {
      console.warn(`Warning: mystral binary not found at ${MYSTRAL_BIN}`);
      console.warn("Run 'bun run build' first to build the CLI");
    }
  });

  it("should show help with --help", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const proc = spawn({
      cmd: [MYSTRAL_BIN, "--help"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    const exitCode = await proc.exited;

    expect(exitCode).toBe(0);
    expect(stdout).toContain("Mystral CLI");
    expect(stdout).toContain("USAGE:");
    expect(stdout).toContain("mystral run");
  });

  it("should show version with --version", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const proc = spawn({
      cmd: [MYSTRAL_BIN, "--version"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    const exitCode = await proc.exited;

    expect(exitCode).toBe(0);
    expect(stdout).toContain("Mystral Native Runtime");
  });

  it("should fail with missing script file", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const proc = spawn({
      cmd: [MYSTRAL_BIN, "run"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stderr = await new Response(proc.stderr).text();
    const exitCode = await proc.exited;

    expect(exitCode).toBe(1);
    expect(stderr).toContain("No script file specified");
  });

  it("should fail with nonexistent script", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const proc = spawn({
      cmd: [MYSTRAL_BIN, "run", "nonexistent-file.js"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stderr = await new Response(proc.stderr).text();
    const exitCode = await proc.exited;

    expect(exitCode).toBe(1);
    expect(stderr).toContain("Cannot open file");
  });

  it("should accept valid CLI options", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    // Just test that help mentions all the options we support
    const proc = spawn({
      cmd: [MYSTRAL_BIN, "--help"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("--width");
    expect(stdout).toContain("--height");
    expect(stdout).toContain("--title");
    expect(stdout).toContain("--headless");
    expect(stdout).toContain("--screenshot");
    expect(stdout).toContain("--frames");
    expect(stdout).toContain("--quiet");
    expect(stdout).toContain("--benchmark");
    expect(stdout).toContain("--warmup");
    expect(stdout).toContain("--benchmark-output");
  });
});
