// examples/internal/pixi-texture-rb-test/main.ts
var PNG_BASE64 = "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAGUlEQVR4nGM4UaHxnxLMMGrAqAGjBgwXAwBav2cfkp7y4AAAAABJRU5ErkJggg==";
var SOURCE_COLOR = [200, 120, 40, 255];
function base64ToArrayBuffer(b64) {
  const table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const lookup = new Uint8Array(256);
  for (let i = 0;i < table.length; i++)
    lookup[table.charCodeAt(i)] = i;
  const clean = b64.replace(/=+$/, "");
  const outLen = clean.length * 3 >> 2;
  const bytes = new Uint8Array(outLen);
  let o = 0;
  for (let i = 0;i < clean.length; i += 4) {
    const a = lookup[clean.charCodeAt(i)];
    const b = lookup[clean.charCodeAt(i + 1)];
    const c = lookup[clean.charCodeAt(i + 2)];
    const d = lookup[clean.charCodeAt(i + 3)];
    bytes[o++] = a << 2 | b >> 4;
    if (i + 2 < clean.length)
      bytes[o++] = (b & 15) << 4 | c >> 2;
    if (i + 3 < clean.length)
      bytes[o++] = (c & 3) << 6 | d;
  }
  return bytes.buffer;
}
function approxEquals(a, b, tol = 2) {
  return Math.abs(a - b) <= tol;
}
var SAMPLE_SHADER = `
struct VSOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vid: u32) -> VSOut {
  // Fullscreen triangle.
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out: VSOut;
  out.pos = vec4f(p[vid], 0.0, 1.0);
  out.uv = p[vid] * vec2f(0.5, 0.5) + vec2f(0.5, 0.5);
  return out;
}

@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

@fragment
fn fs(in: VSOut) -> @location(0) vec4f {
  return textureSampleLevel(src, samp, in.uv, 0.0);
}
`;
async function sampleUploadedColor(device, bitmap, format) {
  const w = bitmap.width;
  const h = bitmap.height;
  const srcTexture = device.createTexture({
    size: [w, h, 1],
    format,
    usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT
  });
  device.queue.copyExternalImageToTexture({ source: bitmap, flipY: false }, { texture: srcTexture, premultipliedAlpha: true }, [w, h, 1]);
  const TARGET = 4;
  const target = device.createTexture({
    size: [TARGET, TARGET, 1],
    format: "rgba8unorm",
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC
  });
  const module = device.createShaderModule({ code: SAMPLE_SHADER });
  const pipeline = device.createRenderPipeline({
    layout: "auto",
    vertex: { module, entryPoint: "vs" },
    fragment: { module, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] },
    primitive: { topology: "triangle-list" }
  });
  const sampler = device.createSampler({ magFilter: "nearest", minFilter: "nearest" });
  const bindGroup = device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: [
      { binding: 0, resource: srcTexture.createView() },
      { binding: 1, resource: sampler }
    ]
  });
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [
      { view: target.createView(), clearValue: { r: 0, g: 0, b: 0, a: 1 }, loadOp: "clear", storeOp: "store" }
    ]
  });
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.draw(3);
  pass.end();
  const bytesPerRow = Math.ceil(TARGET * 4 / 256) * 256;
  const readBuffer = device.createBuffer({
    size: bytesPerRow * TARGET,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ
  });
  encoder.copyTextureToBuffer({ texture: target }, { buffer: readBuffer, bytesPerRow, rowsPerImage: TARGET }, [TARGET, TARGET, 1]);
  device.queue.submit([encoder.finish()]);
  await readBuffer.mapAsync(GPUMapMode.READ);
  const data = new Uint8Array(readBuffer.getMappedRange());
  const pixel = [data[0], data[1], data[2], data[3]];
  readBuffer.unmap();
  return pixel;
}
async function main() {
  console.log("[pixi-texture-rb-test] starting");
  if (!navigator.gpu) {
    console.error("[pixi-texture-rb-test] FAIL: WebGPU unavailable");
    return;
  }
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    console.error("[pixi-texture-rb-test] FAIL: no adapter");
    return;
  }
  const device = await adapter.requestDevice();
  const bitmap = await createImageBitmap(base64ToArrayBuffer(PNG_BASE64));
  console.log("[pixi-texture-rb-test] decoded:", bitmap.width, "x", bitmap.height);
  const bgraPixel = await sampleUploadedColor(device, bitmap, "bgra8unorm");
  const rgbaPixel = await sampleUploadedColor(device, bitmap, "rgba8unorm");
  console.log(`[pixi-texture-rb-test] bgra8unorm sampled (${bgraPixel.join(",")}), expected (${SOURCE_COLOR.join(",")})`);
  console.log(`[pixi-texture-rb-test] rgba8unorm sampled (${rgbaPixel.join(",")}), expected (${SOURCE_COLOR.join(",")})`);
  const bgraOk = SOURCE_COLOR.every((v, i) => approxEquals(v, bgraPixel[i]));
  const rgbaOk = SOURCE_COLOR.every((v, i) => approxEquals(v, rgbaPixel[i]));
  if (bgraOk && rgbaOk) {
    console.log("[pixi-texture-rb-test] PASS");
  } else {
    console.error(`[pixi-texture-rb-test] FAIL — bgra8unorm ${bgraOk ? "ok" : "WRONG (R/B swapped?)"}, rgba8unorm ${rgbaOk ? "ok" : "WRONG"}`);
  }
  const canvasEl = document.createElement("canvas");
  canvasEl.width = 64;
  canvasEl.height = 64;
  document.body.appendChild(canvasEl);
  const ctx = canvasEl.getContext("webgpu");
  if (ctx) {
    ctx.configure({ device, format: navigator.gpu.getPreferredCanvasFormat(), alphaMode: "opaque" });
    const view = ctx.getCurrentTexture().createView();
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [
        {
          view,
          clearValue: bgraOk && rgbaOk ? { r: 0, g: 1, b: 0, a: 1 } : { r: 1, g: 0, b: 0, a: 1 },
          loadOp: "clear",
          storeOp: "store"
        }
      ]
    });
    pass.end();
    device.queue.submit([enc.finish()]);
  }
}
main().catch((err) => {
  console.error("[pixi-texture-rb-test] error:", err);
});
