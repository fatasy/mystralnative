import { CharacterRows } from './worker-shared-schema.mjs';
import { exposeWorkerTask } from 'mystral/worker-pool';

let characters;
let attachedBufferId = 0;

exposeWorkerTask(({ data, begin, end }) => {
    const bufferId = data.characters.buffer.id;
    if (!characters || bufferId !== attachedBufferId) {
        characters = CharacterRows.attach(data.characters);
        attachedBufferId = bufferId;
    }

    const energy = characters.energy;
    for (let index = begin; index < end; index++) {
        energy[index] = (Math.imul(energy[index], 1664525) + 1013904223) >>> 0;
    }
    return end - begin;
});
