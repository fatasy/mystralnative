// Fixed CPU workload for comparing MystralNative runtime builds.
// Keep this workload stable so reports remain comparable across commits.

const ENTITY_COUNT = 50_000;
const positionX = new Float32Array(ENTITY_COUNT);
const positionY = new Float32Array(ENTITY_COUNT);
const velocityX = new Float32Array(ENTITY_COUNT);
const velocityY = new Float32Array(ENTITY_COUNT);

for (let index = 0; index < ENTITY_COUNT; index++) {
    positionX[index] = (index % 1000) * 0.001;
    positionY[index] = ((index * 17) % 1000) * 0.001;
    velocityX[index] = (((index * 13) % 101) - 50) * 0.00001;
    velocityY[index] = (((index * 29) % 101) - 50) * 0.00001;
}

let frame = 0;
let checksum = 0;
const benchmarkState = {
    name: "runtime-core",
    version: 1,
    entityCount: ENTITY_COUNT,
    frame: 0,
    checksum: 0,
};
globalThis.__mystralRuntimeBenchmark = benchmarkState;

function update() {
    for (let index = 0; index < ENTITY_COUNT; index++) {
        let x = positionX[index] + velocityX[index];
        let y = positionY[index] + velocityY[index];

        if (x < 0 || x > 1) {
            velocityX[index] = -velocityX[index];
            x = Math.max(0, Math.min(1, x));
        }
        if (y < 0 || y > 1) {
            velocityY[index] = -velocityY[index];
            y = Math.max(0, Math.min(1, y));
        }

        positionX[index] = x;
        positionY[index] = y;
    }

    checksum += positionX[(frame * 8191) % ENTITY_COUNT];
    frame++;
    benchmarkState.frame = frame;
    benchmarkState.checksum = checksum;
    requestAnimationFrame(update);
}

requestAnimationFrame(update);
