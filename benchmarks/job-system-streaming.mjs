// Fixed local-file streaming workload for the native job system.
// Keep the target and scheduling limits stable so reports remain comparable.

const TARGET = 'benchmarks/runtime-core.js';
const REQUESTS_PER_FRAME = 2;
const MAX_IN_FLIGHT = 512;
const WARMUP_FRAMES = 120;
const SUBMIT_FRAMES = 180;
const FRAME_PACING_MS = 0.25;

let frame = 0;
let checksum = 0;

const benchmarkState = {
    name: 'job-system-streaming',
    version: 1,
    target: TARGET,
    requestsPerFrame: REQUESTS_PER_FRAME,
    maxInFlightLimit: MAX_IN_FLIGHT,
    warmupFrames: WARMUP_FRAMES,
    submitFrames: SUBMIT_FRAMES,
    framePacingMs: FRAME_PACING_MS,
    frame: 0,
    submitted: 0,
    completed: 0,
    failed: 0,
    inFlight: 0,
    maxInFlightObserved: 0,
    bytesRead: 0,
    checksum: 0,
};
globalThis.__mystralRuntimeBenchmark = benchmarkState;

async function readAsset() {
    benchmarkState.submitted++;
    benchmarkState.inFlight++;
    benchmarkState.maxInFlightObserved = Math.max(
        benchmarkState.maxInFlightObserved,
        benchmarkState.inFlight,
    );

    try {
        const response = await fetch(TARGET);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const buffer = await response.arrayBuffer();
        const bytes = new Uint8Array(buffer);
        benchmarkState.bytesRead += bytes.byteLength;
        checksum = (checksum + bytes[0] + bytes[bytes.length - 1]) >>> 0;
        benchmarkState.checksum = checksum;
        benchmarkState.completed++;
    } catch (error) {
        benchmarkState.failed++;
        console.error('[JobSystemStreaming] ' + error.message);
    } finally {
        benchmarkState.inFlight--;
    }
}

function update() {
    frame++;
    benchmarkState.frame = frame;

    if (frame > WARMUP_FRAMES && frame <= WARMUP_FRAMES + SUBMIT_FRAMES) {
        const available = MAX_IN_FLIGHT - benchmarkState.inFlight;
        const toSchedule = Math.min(REQUESTS_PER_FRAME, available);
        for (let index = 0; index < toSchedule; index++) {
            void readAsset();
        }
    }

    // no-SDL frames are uncapped. This small fixed pacing window gives native
    // workers time to run and leaves the final 60 measured frames for draining.
    const pacingDeadline = performance.now() + FRAME_PACING_MS;
    while (performance.now() < pacingDeadline) {}

    requestAnimationFrame(update);
}

requestAnimationFrame(update);
