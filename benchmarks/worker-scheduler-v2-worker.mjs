import { exposeWorkerTasks } from 'mystral/worker-pool';
import { CharacterRows } from './worker-shared-schema.mjs';

exposeWorkerTasks({
    skewed({ data, begin, end }) {
        const rows = CharacterRows.attach(data.characters);
        for (let index = begin; index < end; index++) {
            let value = rows.energy[index] >>> 0;
            const iterations = index >= data.heavyStart ? 64 : 4;
            for (let step = 0; step < iterations; step++) {
                value = (Math.imul(value ^ (index + step), 1664525) + 1013904223) >>> 0;
            }
            rows.energy[index] = value;
        }
        return end - begin;
    },
});
