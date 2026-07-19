import { SharedCommandBuffer, SharedTable } from 'mystral/shared';

export const V2Rows = SharedTable.define('test.worker-pool-v2.rows/v1', {
    energy: 'i32',
});

export const V2Commands = SharedCommandBuffer.define('test.worker-pool-v2.commands/v1', {
    order: 'u32',
    worker: 'u16',
    count: 'u16',
});
