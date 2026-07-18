// Configurable Worker message-limit benchmark.
// Keep sizes and round counts stable so reports remain comparable across commits.

const MiB = 1024 * 1024;
const CASES = [
    { sizeBytes: 16 * MiB, warmupRounds: 1, measuredRounds: 5 },
    { sizeBytes: 32 * MiB, warmupRounds: 1, measuredRounds: 4 },
    { sizeBytes: 64 * MiB, warmupRounds: 1, measuredRounds: 3 },
];

const workerSource = `
self.onmessage = ({ data }) => {
    const bytes = new Uint8Array(data.view.buffer);
    bytes[0] = (bytes[0] + 1) & 0xff;
    postMessage({ sequence: data.sequence, view: data.view }, [data.view.buffer]);
};
`;

function percentile(samples, ratio) {
    const sorted = samples.slice().sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * ratio))];
}

function roundTrip(worker, view, sequence, expected) {
    return new Promise((resolve, reject) => {
        worker.onmessage = ({ data }) => {
            if (data.sequence !== sequence || !(data.view instanceof Uint8Array)) {
                reject(new Error(`Invalid Worker response for round ${sequence}`));
                return;
            }
            const next = (expected + 1) & 0xff;
            if (data.view[0] !== next) {
                reject(new Error(`Transferred bytes were corrupted in round ${sequence}`));
                return;
            }
            resolve({ view: data.view, expected: next });
        };
        worker.onerror = event => reject(event.error || new Error(event.message));
        const buffer = view.buffer;
        worker.postMessage({ sequence, view }, [buffer]);
        if (buffer.byteLength !== 0) reject(new Error(`Sender was not detached in round ${sequence}`));
    });
}

async function runCase(worker, config, firstSequence) {
    let view = new Uint8Array(config.sizeBytes);
    view[0] = 0x21;
    let expected = view[0];
    const samples = [];
    const totalRounds = config.warmupRounds + config.measuredRounds;
    for (let round = 0; round < totalRounds; round++) {
        const sequence = firstSequence + round;
        const startedAt = performance.now();
        const returned = await roundTrip(worker, view, sequence, expected);
        const elapsed = performance.now() - startedAt;
        view = returned.view;
        expected = returned.expected;
        if (round >= config.warmupRounds) samples.push(elapsed);
    }
    return {
        ...config,
        roundTrip: {
            medianMs: percentile(samples, 0.5),
            p95Ms: percentile(samples, 0.95),
            p99Ms: percentile(samples, 0.99),
        },
    };
}

async function main() {
    let invalidLimitsRejected = false;
    try {
        new Worker(new Blob(['self.onmessage = () => {};'], { type: 'application/javascript' }), {
            maxMessageBytes: 2,
            maxQueuedBytes: 1,
        });
    } catch (error) {
        invalidLimitsRejected = error instanceof RangeError;
    }
    if (!invalidLimitsRejected) throw new Error('Invalid Worker limits were accepted');

    const worker = new Worker(
        new Blob([workerSource], { type: 'application/javascript' }),
        {
            name: 'worker-message-limits-benchmark',
            maxMessages: 8,
            maxMessageBytes: 80 * MiB,
            maxQueuedBytes: 96 * MiB,
        },
    );
    const cases = [];
    let sequence = 0;
    try {
        for (const config of CASES) {
            cases.push(await runCase(worker, config, sequence));
            sequence += config.warmupRounds + config.measuredRounds;
        }
    } finally {
        worker.terminate();
    }
    console.log('[WorkerMessageLimitBenchmark] ' + JSON.stringify({
        name: 'worker-configurable-message-limits',
        version: 1,
        limits: { maxMessages: 8, maxMessageBytes: 80 * MiB, maxQueuedBytes: 96 * MiB },
        cases,
        validation: {
            roundTrips: sequence,
            invalidLimitsRejected,
            senderDetachedEveryRound: true,
            typedViewPreservedEveryRound: true,
        },
    }));
    process.exit(0);
}

main().catch(error => {
    console.error('[WorkerMessageLimitBenchmark] failed: ' + error.message);
    process.exit(1);
});
