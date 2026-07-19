import { WorkerPool } from 'mystral/worker-pool';
import { V2Commands } from './worker-pool-v2-schema.mjs';

const poolPromise = WorkerPool.create('./tests/native/fixtures/worker-pool-v2-task.mjs', {
    size: 3,
    name: 'worker-pool-v2',
});

self.onmessage = async ({ data }) => {
    try {
        const pool = await poolPromise;
        const update = pool.parallelFor('update', {
            rows: data.rows,
            commands: data.commands,
        }, {
            length: data.length,
            grainSize: 2,
            reducer: 'sum',
        });

        const queuedController = new AbortController();
        const queued = pool.parallelFor('inspect', {}, {
            length: 8,
            schedule: 'static',
            signal: queuedController.signal,
        });
        queuedController.abort();
        let queuedAbort = '';
        try {
            await queued;
        } catch (error) {
            queuedAbort = error && error.name ? error.name : String(error);
        }

        const updated = await update;
        const partitions = await pool.parallelFor('inspect', {}, {
            length: 8,
            schedule: 'static',
        });

        const activeController = new AbortController();
        const active = pool.forEach('inspect', {}, {
            length: 24_000,
            grainSize: 1,
            signal: activeController.signal,
        });
        activeController.abort();
        let activeAbort = '';
        try {
            await active;
        } catch (error) {
            activeAbort = error && error.name ? error.name : String(error);
        }

        const staticController = new AbortController();
        const staticActive = pool.parallelFor('inspect', {}, {
            length: 8,
            schedule: 'static',
            signal: staticController.signal,
        });
        staticController.abort();
        let staticAbort = '';
        try {
            await staticActive;
        } catch (error) {
            staticAbort = error && error.name ? error.name : String(error);
        }

        await pool.forEach('inspect', {}, {
            length: 24,
            grainSize: 1,
        });
        const commands = V2Commands.attach(data.commands).drain({ sortBy: 'order' });
        const stats = pool.stats();
        await pool.close();

        let startupTimeout = false;
        try {
            await WorkerPool.create(
                './tests/native/fixtures/worker-pool-no-handlers.mjs',
                { size: 1, startupTimeoutMs: 20 },
            );
        } catch (error) {
            startupTimeout = Boolean(error && String(error.message).includes('startup timed out'));
        }
        postMessage({
            kind: 'worker-pool-v2',
            updated,
            partitions,
            commands,
            queuedAbort,
            activeAbort,
            staticAbort,
            startupTimeout,
            stats,
        });
    } catch (error) {
        postMessage({
            kind: 'worker-pool-v2-error',
            message: error && error.message ? error.message : String(error),
        });
    }
};
