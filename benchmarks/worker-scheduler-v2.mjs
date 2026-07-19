import { WorkerPool } from 'mystral/worker-pool';
import { CharacterRows } from './worker-shared-schema.mjs';

const CAPACITY = 100_000;
const HEAVY_START = Math.floor(CAPACITY * 0.75);
const WARMUP_ROUNDS = 5;
const MEASURED_ROUNDS = 20;
const TOTAL_ROUNDS = WARMUP_ROUNDS + MEASURED_ROUNDS;

function initialize(table) {
    table.length = CAPACITY;
    for (let index = 0; index < CAPACITY; index++) {
        table.energy[index] = (index * 17 + 31) >>> 0;
        table.settlementId[index] = index % 4096;
        table.birthDay[index] = -((index * 13) % 20000);
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

async function measure(runRound) {
    const samples = [];
    for (let round = 0; round < TOTAL_ROUNDS; round++) {
        const startedAt = performance.now();
        await runRound();
        const elapsed = performance.now() - startedAt;
        if (round >= WARMUP_ROUNDS) samples.push(elapsed);
    }
    return summarize(samples);
}

const staticRows = CharacterRows.create({ capacity: CAPACITY });
const dynamicRows = CharacterRows.create({ capacity: CAPACITY });
initialize(staticRows);
initialize(dynamicRows);

async function main() {
    const pool = await WorkerPool.create('./benchmarks/worker-scheduler-v2-worker.mjs', {
        size: 4,
        name: 'scheduler-v2-benchmark',
    });
    const staticTiming = await measure(() => pool.parallelFor('skewed', {
        characters: staticRows.handle(),
        heavyStart: HEAVY_START,
    }, { length: CAPACITY, schedule: 'static' }));

    const dynamicTiming = await measure(() => pool.forEach('skewed', {
        characters: dynamicRows.handle(),
        heavyStart: HEAVY_START,
    }, {
        length: CAPACITY,
        grainSize: 512,
    }));

    const staticChecksum = checksum(staticRows);
    const dynamicChecksum = checksum(dynamicRows);
    const result = {
        workload: {
            capacity: CAPACITY,
            heavyStart: HEAVY_START,
            workers: pool.size,
            warmupRounds: WARMUP_ROUNDS,
            measuredRounds: MEASURED_ROUNDS,
        },
        static: staticTiming,
        dynamic: dynamicTiming,
        pool: pool.stats(),
        checksum: {
            static: staticChecksum,
            dynamic: dynamicChecksum,
            match: staticChecksum === dynamicChecksum,
        },
    };
    console.log('[WorkerSchedulerV2Benchmark] ' + JSON.stringify(result));
    await pool.close();
    process.exit(result.checksum.match ? 0 : 1);
}

main().catch(error => {
    console.error('[WorkerSchedulerV2Benchmark] failed: ' + error.message);
    process.exit(1);
});
