// CPU-heavy WebGPU bridge workload. Keep stable for build-to-build comparisons.
const DRAWS_PER_FRAME = 1000;

const shaderCode = `
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  var p = array<vec2f, 3>(vec2f(0.0, 0.5), vec2f(-0.5, -0.5), vec2f(0.5, -0.5));
  return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(0.2, 0.6, 1.0, 1.0); }
`;

const state = {
  name: 'webgpu-bridge',
  version: 1,
  drawsPerFrame: DRAWS_PER_FRAME,
  frame: 0,
  webgpu: null,
};
globalThis.__mystralRuntimeBenchmark = state;

async function main() {
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });

  const module = device.createShaderModule({ code: shaderCode });
  const pipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module, entryPoint: 'vs' },
    fragment: { module, entryPoint: 'fs', targets: [{ format }] },
  });

  function frame() {
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: context.getCurrentTexture().createView(),
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear',
        storeOp: 'store',
      }],
    });
    pass.setPipeline(pipeline);
    for (let index = 0; index < DRAWS_PER_FRAME; index++) pass.draw(3);
    pass.end();
    device.queue.submit([encoder.finish()]);

    state.frame++;
    if (state.frame % 60 === 0) state.webgpu = __mystralWebGpuStats();
    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
}

main().catch((error) => { throw error; });
