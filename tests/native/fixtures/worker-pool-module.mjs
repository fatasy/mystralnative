import { exposeWorkerTask, transferResult } from 'mystral/worker-pool';

exposeWorkerTask(({ begin, end, workerIndex, data }) => {
    if (data.binary instanceof ArrayBuffer) {
        const bytes = new Uint8Array(data.binary);
        bytes[0] += 1;
        return transferResult({ binary: data.binary }, [data.binary]);
    }
    return { begin, end, workerIndex, tag: data.tag };
});
