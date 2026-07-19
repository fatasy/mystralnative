import { NestedRows } from './nested-worker-schema.mjs';
import { exposeWorkerTasks } from 'mystral/worker-pool';

exposeWorkerTasks({
    update({ data, begin, end, workerIndex }) {
        if (data.hang && workerIndex === 0) {
            while (true) {}
        }
        if (data.fail && workerIndex === 1) {
            throw new Error('intentional nested Worker failure');
        }

        const rows = NestedRows.attach(data.rows);
        for (let index = begin; index < end; index++) {
            rows.energy[index] += workerIndex + 1;
        }
        return { begin, end, workerIndex };
    },
});
