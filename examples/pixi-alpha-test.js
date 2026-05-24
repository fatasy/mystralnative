// examples/internal/pixi-alpha-test/main.ts
var PNG_BASE64 = "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAHUlEQVR42mM8kWLUwEABYGKgEIwaMGrAqAGDxQAA1zMB/up+9CsAAAAASUVORK5CYII=";
var EXPECTED_PREMUL = [100, 50, 25, 128];
var EXPECTED_STRAIGHT = [200, 100, 50, 128];
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
async function readbackFirstPixel(device, bitmap, premultipliedAlpha) {
  const w = bitmap.width;
  const h = bitmap.height;
  const texture = device.createTexture({
    size: [w, h, 1],
    format: "rgba8unorm",
    usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING
  });
  device.queue.copyExternalImageToTexture({ source: bitmap, flipY: false }, { texture, premultipliedAlpha }, [w, h, 1]);
  const bytesPerRow = Math.ceil(w * 4 / 256) * 256;
  const readBuffer = device.createBuffer({
    size: bytesPerRow * h,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ
  });
  const encoder = device.createCommandEncoder();
  encoder.copyTextureToBuffer({ texture }, { buffer: readBuffer, bytesPerRow, rowsPerImage: h }, [w, h, 1]);
  device.queue.submit([encoder.finish()]);
  await readBuffer.mapAsync(GPUMapMode.READ);
  const data = new Uint8Array(readBuffer.getMappedRange());
  const pixel = [data[0], data[1], data[2], data[3]];
  readBuffer.unmap();
  return pixel;
}
async function main() {
  console.log("[pixi-alpha-test] starting");
  if (!navigator.gpu) {
    console.error("[pixi-alpha-test] FAIL: WebGPU unavailable");
    return;
  }
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    console.error("[pixi-alpha-test] FAIL: no adapter");
    return;
  }
  const device = await adapter.requestDevice();
  const bitmap = await createImageBitmap(base64ToArrayBuffer(PNG_BASE64));
  console.log("[pixi-alpha-test] decoded:", bitmap.width, "x", bitmap.height);
  const premulPixel = await readbackFirstPixel(device, bitmap, true);
  const straightPixel = await readbackFirstPixel(device, bitmap, false);
  console.log(`[pixi-alpha-test] premultipliedAlpha=true  got (${premulPixel.join(",")}), expected (${EXPECTED_PREMUL.join(",")})`);
  console.log(`[pixi-alpha-test] premultipliedAlpha=false got (${straightPixel.join(",")}), expected (${EXPECTED_STRAIGHT.join(",")})`);
  const premulOk = EXPECTED_PREMUL.every((v, i) => approxEquals(v, premulPixel[i]));
  const straightOk = EXPECTED_STRAIGHT.every((v, i) => approxEquals(v, straightPixel[i]));
  if (premulOk && straightOk) {
    console.log("[pixi-alpha-test] PASS");
  } else {
    console.error(`[pixi-alpha-test] FAIL — premultiplied path ${premulOk ? "ok" : "WRONG"}, straight path ${straightOk ? "ok" : "WRONG"}`);
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
          clearValue: premulOk && straightOk ? { r: 0, g: 1, b: 0, a: 1 } : { r: 1, g: 0, b: 0, a: 1 },
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
  console.error("[pixi-alpha-test] error:", err);
});
