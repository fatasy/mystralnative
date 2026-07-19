import { WorkerPool } from 'mystral/worker-pool';

const poolPromise = WorkerPool.create('./tests/native/fixtures/nested-worker-task.mjs', {
    size: 3,
    name: 'nested-smoke',
});

self.onmessage = async ({ data }) => {
    try {
        const pool = await poolPromise;
        const results = await pool.parallelFor(
            'update',
            { rows: data.rows, fail: data.fail, hang: data.hang },
            { length: data.length, schedule: 'static' },
        );
        postMessage({ kind: 'nested-result', results });
    } catch (error) {
        postMessage({
            kind: 'nested-error',
            message: error && error.message ? error.message : String(error),
        });
    }
};
