import { exposeWorkerTasks } from 'mystral/worker-pool';
import { V2Commands, V2Rows } from './worker-pool-v2-schema.mjs';

exposeWorkerTasks({
    update({ data, begin, end, workerIndex }) {
        const rows = V2Rows.attach(data.rows);
        const commands = V2Commands.attach(data.commands);
        for (let index = begin; index < end; index++) rows.energy[index] += 1;
        if (!commands.push(workerIndex, {
            order: begin,
            worker: workerIndex,
            count: end - begin,
        })) {
            throw new Error('WorkerPool v2 command lane is full');
        }
        return end - begin;
    },

    async inspect({ begin, end, workerIndex }) {
        await Promise.resolve();
        return { begin, end, workerIndex };
    },
});
