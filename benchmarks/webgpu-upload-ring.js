// Repeated aligned uploads verify persistent staging reuse and queue ordering.
const WRITES_PER_FRAME = 32;
const WORDS_PER_WRITE = 1024;
const state = {
  name: 'webgpu-upload-ring',
  version: 1,
  frame: 0,
  writesPerFrame: WRITES_PER_FRAME,
  webgpu: null,
};
globalThis.__mystralRuntimeBenchmark = state;

async function main() {
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const data = new Uint32Array(WORDS_PER_WRITE);
  const destination = device.createBuffer({
    size: WRITES_PER_FRAME * data.byteLength,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
  });

  function frame() {
    for (let index = 0; index < WRITES_PER_FRAME; index++) {
      data[0] = state.frame * WRITES_PER_FRAME + index;
      device.queue.writeBuffer(destination, index * data.byteLength, data);
    }
    const encoder = device.createCommandEncoder();
    device.queue.submit([encoder.finish()]);

    state.frame++;
    state.webgpu = __mystralWebGpuStats();
    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
}

main().catch((error) => { throw error; });
