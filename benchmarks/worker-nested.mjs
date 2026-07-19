import { CharacterRows } from './worker-shared-schema.mjs';

const CAPACITY = 200_000;
const WARMUP_ROUNDS = 10;
const MEASURED_ROUNDS = 50;
const TOTAL_ROUNDS = WARMUP_ROUNDS + MEASURED_ROUNDS;

function initialize(table) {
    table.length = CAPACITY;
    for (let index = 0; index < CAPACITY; index++) {
        table.energy[index] = (index * 17 + 31) >>> 0;
        table.settlementId[index] = index % 4096;
        table.birthDay[index] = -((index * 13) % 20000);
    }
}

function updateRange(table) {
    for (let index = 0; index < CAPACITY; index++) {
        table.energy[index] =
            (Math.imul(table.energy[index], 1664525) + 1013904223) >>> 0;
    }
}

function checksum(table) {
    let value = 0;
    for (let index = 0; index < CAPACITY; index++) {
        value = (value + table.energy[index]) >>> 0;
    }
    return value;
}

function summarize(samples) {
    const sorted = samples.slice().sort((left, right) => left - right);
    const percentile = fraction => sorted[Math.min(
        sorted.length - 1,
        Math.floor((sorted.length - 1) * fraction),
    )];
    return {
        medianMs: percentile(0.5),
        p95Ms: percentile(0.95),
        p99Ms: percentile(0.99),
    };
}

const baseline = CharacterRows.create({ capacity: CAPACITY });
const nested = CharacterRows.create({ capacity: CAPACITY });
initialize(baseline);
initialize(nested);

const baselineSamples = [];
for (let round = 0; round < TOTAL_ROUNDS; round++) {
    const startedAt = performance.now();
    updateRange(baseline);
    const elapsed = performance.now() - startedAt;
    if (round >= WARMUP_ROUNDS) baselineSamples.push(elapsed);
}

const simulation = new Worker('./benchmarks/worker-nested-owner.mjs', {
    type: 'module',
    name: 'nested-benchmark-owner',
});
const pending = new Map();
simulation.onmessage = ({ data }) => {
    const entry = pending.get(data.round);
    if (!entry) return;
    pending.delete(data.round);
    if (data.ok) entry.resolve();
    else entry.reject(new Error(data.error || 'Nested Worker benchmark round failed'));
};
simulation.onerror = event => {
    const error = event.error || new Error(event.message || 'Nested Worker benchmark failed');
    for (const entry of pending.values()) entry.reject(error);
    pending.clear();
};

function runNestedRound(round) {
    return new Promise((resolve, reject) => {
        pending.set(round, { resolve, reject });
        simulation.postMessage({
            round,
            characters: nested.handle(),
            length: CAPACITY,
        });
    });
}

async function main() {
    const nestedSamples = [];
    for (let round = 0; round < TOTAL_ROUNDS; round++) {
        const startedAt = performance.now();
        await runNestedRound(round);
        const elapsed = performance.now() - startedAt;
        if (round >= WARMUP_ROUNDS) nestedSamples.push(elapsed);
    }

    const baselineChecksum = checksum(baseline);
    const nestedChecksum = checksum(nested);
    const result = {
        workload: {
            capacity: CAPACITY,
            workers: 4,
            warmupRounds: WARMUP_ROUNDS,
            measuredRounds: MEASURED_ROUNDS,
        },
        baseline: summarize(baselineSamples),
        nested: summarize(nestedSamples),
        checksum: {
            baseline: baselineChecksum,
            nested: nestedChecksum,
            match: baselineChecksum === nestedChecksum,
        },
    };
    console.log('[NestedWorkerBenchmark] ' + JSON.stringify(result));
    simulation.terminate();
    process.exit(result.checksum.match ? 0 : 1);
}

main().catch(error => {
    console.error('[NestedWorkerBenchmark] failed: ' + error.message);
    simulation.terminate();
    process.exit(1);
});
