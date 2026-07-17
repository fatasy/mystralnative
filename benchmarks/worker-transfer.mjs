// Fixed transferable ArrayBuffer benchmark.
// Keep sizes and round counts stable so reports remain comparable across commits.

const CASES = [
    { sizeBytes: 64 * 1024, warmupRounds: 10, measuredRounds: 100 },
    { sizeBytes: 1024 * 1024, warmupRounds: 5, measuredRounds: 30 },
    { sizeBytes: 8 * 1024 * 1024, warmupRounds: 3, measuredRounds: 10 },
];

const workerSource = `
self.onmessage = ({ data }) => {
    const bytes = new Uint8Array(data.buffer);
    bytes[0] = (bytes[0] + 1) & 0xff;
    bytes[bytes.length - 1] = (bytes[bytes.length - 1] ^ 0xff) & 0xff;
    postMessage({ sequence: data.sequence, buffer: data.buffer }, [data.buffer]);
};
`;

function percentile(samples, ratio) {
    const sorted = samples.slice().sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * ratio))];
}

function summarize(samples) {
    return {
        medianMs: percentile(samples, 0.5),
        p95Ms: percentile(samples, 0.95),
        p99Ms: percentile(samples, 0.99),
    };
}

function roundTrip(worker, buffer, sequence, expectedFirst, expectedLast) {
    return new Promise((resolve, reject) => {
        worker.onmessage = ({ data }) => {
            if (data.sequence !== sequence || !(data.buffer instanceof ArrayBuffer)) {
                reject(new Error(`Invalid Worker response for round ${sequence}`));
                return;
            }

            const bytes = new Uint8Array(data.buffer);
            const nextFirst = (expectedFirst + 1) & 0xff;
            const nextLast = (expectedLast ^ 0xff) & 0xff;
            if (bytes[0] !== nextFirst || bytes[bytes.length - 1] !== nextLast) {
                reject(new Error(`Transferred bytes were corrupted in round ${sequence}`));
                return;
            }
            resolve({ buffer: data.buffer, first: nextFirst, last: nextLast });
        };
        worker.onerror = event => reject(event.error || new Error(event.message));

        worker.postMessage({ sequence, buffer }, [buffer]);
        if (buffer.byteLength !== 0) {
            reject(new Error(`Sender buffer was not detached in round ${sequence}`));
        }
    });
}

async function runCase(worker, config, firstSequence) {
    let buffer = new ArrayBuffer(config.sizeBytes);
    let bytes = new Uint8Array(buffer);
    bytes[0] = 0x31;
    bytes[bytes.length - 1] = 0xa7;
    let expectedFirst = bytes[0];
    let expectedLast = bytes[bytes.length - 1];
    const samples = [];
    const totalRounds = config.warmupRounds + config.measuredRounds;

    for (let round = 0; round < totalRounds; round++) {
        const sequence = firstSequence + round;
        const startedAt = performance.now();
        const returned = await roundTrip(
            worker,
            buffer,
            sequence,
            expectedFirst,
            expectedLast,
        );
        const elapsed = performance.now() - startedAt;
        buffer = returned.buffer;
        expectedFirst = returned.first;
        expectedLast = returned.last;
        if (round >= config.warmupRounds) samples.push(elapsed);
    }

    const totalMeasuredMs = samples.reduce((total, sample) => total + sample, 0);
    const transferredMiB = (config.sizeBytes * config.measuredRounds * 2) / (1024 * 1024);
    return {
        sizeBytes: config.sizeBytes,
        warmupRounds: config.warmupRounds,
        measuredRounds: config.measuredRounds,
        roundTrip: summarize(samples),
        throughputMiBPerSecond: transferredMiB / (totalMeasuredMs / 1000),
        finalSentinels: { first: expectedFirst, last: expectedLast },
    };
}

async function main() {
    const worker = new Worker(
        new Blob([workerSource], { type: 'application/javascript' }),
        { name: 'arraybuffer-transfer-benchmark' },
    );
    const memoryStart = process.memoryUsage();
    const results = [];
    let sequence = 0;

    try {
        for (const config of CASES) {
            results.push(await runCase(worker, config, sequence));
            sequence += config.warmupRounds + config.measuredRounds;
        }
    } finally {
        worker.terminate();
    }
    await new Promise(resolve => setTimeout(resolve, 0));

    const report = {
        name: 'worker-arraybuffer-transfer',
        version: 1,
        direction: 'main-worker-main',
        directionsPerRound: 2,
        cases: results,
        memory: {
            gcForced: false,
            start: memoryStart,
            end: process.memoryUsage(),
        },
        validation: {
            roundTrips: sequence,
            senderDetachedEveryRound: true,
            sentinelsPreservedEveryRound: true,
        },
    };
    console.log('[WorkerTransferBenchmark] ' + JSON.stringify(report));
    process.exit(0);
}

main().catch(error => {
    console.error('[WorkerTransferBenchmark] failed: ' + error.message);
    process.exit(1);
});
