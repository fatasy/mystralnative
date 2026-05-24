#!/usr/bin/env bun
/**
 * Bundle all mystral-test examples
 *
 * This script bundles all TypeScript examples in the mystral-test directory
 * into JavaScript files that can be run with the mystral CLI.
 *
 * Usage:
 *   bun scripts/bundle-examples.ts
 */

import { Glob } from "bun";
import { join, basename } from "path";

// Source is in examples/internal/ (not synced to public repo)
const EXAMPLES_DIR = join(import.meta.dir, "../examples/internal/mystral-test");
// Output goes to examples/ (synced to public repo)
const OUTPUT_DIR = join(import.meta.dir, "../examples");

// Separate source directories for different example types
const PIXI_TEST_DIR = join(import.meta.dir, "../examples/internal/pixi-test");
const PIXI_ALPHA_TEST_DIR = join(import.meta.dir, "../examples/internal/pixi-alpha-test");
const THREEJS_RT_DIR = join(import.meta.dir, "../examples/internal/threejs-rt");

// List of example files to bundle (source -> output name)
const EXAMPLES: Array<{ source: string; output: string; dir?: string }> = [
  { source: "main.ts", output: "mystral-helmet.js" },
  { source: "daynight-demo.ts", output: "daynight.js" },
  { source: "sponza-native.ts", output: "sponza.js" },
  { source: "main.ts", output: "pixi-test.js", dir: PIXI_TEST_DIR },
  { source: "main.ts", output: "pixi-alpha-test.js", dir: PIXI_ALPHA_TEST_DIR },
  { source: "threejs-rt-shadows.ts", output: "threejs-rt-shadows.js", dir: THREEJS_RT_DIR },
  { source: "threejs-rt-hardware.ts", output: "threejs-rt-hardware.js", dir: THREEJS_RT_DIR },
];

async function bundleExample(example: { source: string; output: string; dir?: string }): Promise<boolean> {
  const entryPath = join(example.dir || EXAMPLES_DIR, example.source);
  const outputPath = join(OUTPUT_DIR, example.output);

  console.log(`Bundling: ${example.source} -> ${example.output}`);

  try {
    const result = await Bun.build({
      entrypoints: [entryPath],
      target: "browser",
      format: "esm",
      minify: false,
      sourcemap: "none",
      splitting: false,
      treeshaking: true,
      define: {
        "process.env.NODE_ENV": '"production"',
      },
    });

    if (!result.success) {
      console.error(`  Failed to bundle ${example.source}:`);
      for (const log of result.logs) {
        console.error(`    ${log}`);
      }
      return false;
    }

    // Get the bundled output
    let output = await result.outputs[0].text();

    // Post-process: Replace import.meta with a safe fallback
    output = output.replace(/import\.meta/g, '({ env: {}, url: "" })');

    // Write to file
    await Bun.write(outputPath, output);

    const sizeKB = (output.length / 1024).toFixed(1);
    console.log(`  Done: ${sizeKB} KB`);
    return true;
  } catch (error: any) {
    console.error(`  Error bundling ${example.source}: ${error}`);
    if (error.errors) {
      for (const e of error.errors) {
        console.error(`    - ${e.text || e.message || e}`);
      }
    }
    return false;
  }
}

async function main() {
  console.log("=== Bundling Mystral Engine Examples ===\n");

  let success = 0;
  let failed = 0;

  for (const example of EXAMPLES) {
    const result = await bundleExample(example);
    if (result) {
      success++;
    } else {
      failed++;
    }
  }

  console.log(`\n=== Complete ===`);
  console.log(`Success: ${success}/${EXAMPLES.length}`);

  if (failed > 0) {
    console.log(`Failed: ${failed}/${EXAMPLES.length}`);
    process.exit(1);
  }
}

main();
