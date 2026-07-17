#!/usr/bin/env bun
/**
 * Pre-bundling Script for Mystral Native Runtime
 *
 * Bundles TypeScript/JavaScript files with imports into a single JS file
 * that can be run with the mystral CLI.
 *
 * Usage:
 *   bun scripts/prebundle.ts <entry.ts> [--out <output.js>]
 *   bun scripts/prebundle.ts src/game.ts --out dist/game.bundle.js
 *
 * If --out is not specified, outputs to stdout.
 */

import { argv, write, stdout } from "bun";

interface Options {
  entry: string;
  output: string | null;
  minify: boolean;
  sourcemap: boolean;
}

function parseArgs(): Options {
  const args = argv.slice(2);
  const options: Options = {
    entry: "",
    output: null,
    minify: false,
    sourcemap: false,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--out" || arg === "-o") {
      options.output = args[++i];
    } else if (arg === "--minify") {
      options.minify = true;
    } else if (arg === "--sourcemap") {
      options.sourcemap = true;
    } else if (arg === "--help" || arg === "-h") {
      printHelp();
      process.exit(0);
    } else if (!arg.startsWith("-")) {
      options.entry = arg;
    }
  }

  return options;
}

function printHelp() {
  console.log(`
Mystral Pre-bundler - Bundle TypeScript/JavaScript for native runtime

USAGE:
    bun scripts/prebundle.ts <entry.ts> [options]

OPTIONS:
    --out, -o <file>    Output file (default: stdout)
    --minify            Minify output
    --sourcemap         Generate sourcemap
    --help, -h          Show this help

EXAMPLES:
    # Bundle to stdout
    bun scripts/prebundle.ts game.ts

    # Bundle to file
    bun scripts/prebundle.ts game.ts --out dist/game.bundle.js

    # Bundle and run
    bun scripts/prebundle.ts game.ts | mystral run -

    # Or save and run
    bun scripts/prebundle.ts game.ts --out game.js && ./build/mystral run game.js

SUPPORTED IMPORTS:
    - Relative imports: import { foo } from './utils'
    - Package imports: import * as THREE from 'three' (if installed)
    - TypeScript: Full TS support via Bun's built-in transpiler

NOTE:
    The bundled output is plain JavaScript that runs in the Mystral
    native runtime's V8 JavaScript engine.
`);
}

async function main() {
  const options = parseArgs();

  if (!options.entry) {
    console.error("Error: No entry file specified");
    console.error("Usage: bun scripts/prebundle.ts <entry.ts> [--out <output.js>]");
    process.exit(1);
  }

  // Check if entry file exists
  const entryFile = Bun.file(options.entry);
  if (!(await entryFile.exists())) {
    console.error(`Error: Entry file not found: ${options.entry}`);
    process.exit(1);
  }

  try {
    // Bundle using Bun's built-in bundler
    const result = await Bun.build({
      entrypoints: [options.entry],
      target: "browser", // Closest to the runtime's V8 environment
      format: "esm",
      minify: options.minify,
      sourcemap: options.sourcemap ? "inline" : "none",
      // Don't split - we want a single file
      splitting: false,
      // Define globals that exist in our runtime
      define: {
        "process.env.NODE_ENV": '"production"',
      },
    });

    if (!result.success) {
      console.error("Build failed:");
      for (const log of result.logs) {
        console.error(log);
      }
      process.exit(1);
    }

    // Get the bundled output
    let output = await result.outputs[0].text();

    // Post-process: Replace import.meta with a safe fallback for non-module contexts
    // This handles code like: typeof import.meta !== "undefined" && import.meta.env?.VITE_HIDE_DEBUG_OVERLAY
    output = output.replace(/import\.meta/g, '({ env: {}, url: "" })');

    // Write to file or stdout
    if (options.output) {
      await write(options.output, output);
      console.error(`Bundled: ${options.entry} -> ${options.output}`);
      console.error(`Size: ${output.length} bytes`);
    } else {
      // Write to stdout
      await write(stdout, output);
    }
  } catch (error) {
    console.error(`Error bundling: ${error}`);
    process.exit(1);
  }
}

main();
