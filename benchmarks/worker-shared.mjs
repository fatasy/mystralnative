import { CharacterRows } from './worker-shared-schema.mjs';
import { WorkerPool } from 'mystral/worker-pool';

const CAPACITY = 200_000;
const WORKERS = 4;
const WARMUP_ROUNDS = 10;
const MEASURED_ROUNDS = 50;
const TOTAL_ROUNDS = WARMUP_ROUNDS + MEASURED_ROUNDS;

function initialize(table) {
    table.length = CAPACITY;
    for (let index = 0; index < CAPACITY; index++) {
        table.energy[index] = (index * 2654435761) >>> 0;
        table.settlementId[index] = index % 4096;
        table.birthDay[index] = -((index * 17) % 30_000);
    }
}

function updateRange(table, start, end) {
    const energy = table.energy;
    for (let index = start; index < end; index++) {
        energy[index] = (Math.imul(energy[index], 1664525) + 1013904223) >>> 0;
    }
}

function percentile(samples, ratio) {
    const sorted = samples.slice().sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * ratio))];
}

function summarize(samples) {
    return {
        medianMs: percentile(samples, 0.5),
        p95Ms: percentile(samples, 0.95),
        p99Ms: percentile(samples, 0.99),
    };
}

function checksum(table) {
    let hash = 0x811c9dc5;
    for (let index = 0; index < CAPACITY; index++) {
        hash ^= table.energy[index];
        hash = Math.imul(hash, 0x01000193);
    }
    return hash >>> 0;
}

const baseline = CharacterRows.create({ capacity: CAPACITY });
const parallel = CharacterRows.create({ capacity: CAPACITY });
initialize(baseline);
initialize(parallel);

const baselineSamples = [];
for (let round = 0; round < TOTAL_ROUNDS; round++) {
    const startedAt = performance.now();
    updateRange(baseline, 0, CAPACITY);
    const elapsed = performance.now() - startedAt;
    if (round >= WARMUP_ROUNDS) baselineSamples.push(elapsed);
}

const parallelSamples = [];

const pool = new WorkerPool('./benchmarks/worker-shared-worker.mjs', {
    size: WORKERS,
    name: 'benchmark',
});

async function runParallelBenchmark() {
    for (let round = 0; round < TOTAL_ROUNDS; round++) {
        const startedAt = performance.now();
        await pool.run(
            { characters: parallel.handle() },
            { length: CAPACITY },
        );
        const elapsed = performance.now() - startedAt;
        if (round >= WARMUP_ROUNDS) parallelSamples.push(elapsed);
    }

    const baselineChecksum = checksum(baseline);
    const parallelChecksum = checksum(parallel);
    const result = {
        workload: {
            capacity: CAPACITY,
            workers: WORKERS,
            warmupRounds: WARMUP_ROUNDS,
            measuredRounds: MEASURED_ROUNDS,
        },
        baseline: summarize(baselineSamples),
        parallel: summarize(parallelSamples),
        checksum: {
            baseline: baselineChecksum,
            parallel: parallelChecksum,
            match: baselineChecksum === parallelChecksum,
        },
    };
    console.log('[WorkerBenchmark] ' + JSON.stringify(result));
    pool.terminate();
    process.exit(result.checksum.match ? 0 : 1);
}

runParallelBenchmark().catch(error => {
    console.error('[WorkerBenchmark] WorkerPool failed: ' + error.message);
    pool.terminate();
    process.exit(1);
});
