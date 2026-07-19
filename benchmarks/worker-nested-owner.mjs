import { WorkerPool } from 'mystral/worker-pool';

const poolPromise = WorkerPool.create('./benchmarks/worker-shared-worker.mjs', {
    size: 4,
    name: 'nested-benchmark',
});

self.onmessage = async ({ data }) => {
    try {
        const pool = await poolPromise;
        await pool.parallelFor(
            'update',
            { characters: data.characters },
            { length: data.length, schedule: 'static' },
        );
        postMessage({ round: data.round, ok: true });
    } catch (error) {
        postMessage({
            round: data.round,
            ok: false,
            error: error && error.message ? error.message : String(error),
        });
    }
};
