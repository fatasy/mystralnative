// Simple Cube Example - Using hardcoded vertices in shader (like triangle.js)
// This tests that basic 3D rendering works before adding storage buffers
console.log("Simple cube example starting...");

// ============================================================================
// MATH UTILITIES
// ============================================================================

const mat4 = {
  create() {
    return new Float32Array(16);
  },

  identity(out) {
    out.fill(0);
    out[0] = out[5] = out[10] = out[15] = 1;
    return out;
  },

  perspective(out, fovy, aspect, near, far) {
    const f = 1.0 / Math.tan(fovy / 2);
    out.fill(0);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1;
    out[14] = (2 * far * near) / (near - far);
    return out;
  },

  lookAt(out, eye, center, up) {
    const zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
    let len = 1 / Math.sqrt(zx * zx + zy * zy + zz * zz);
    const z0 = zx * len, z1 = zy * len, z2 = zz * len;

    const xx = up[1] * z2 - up[2] * z1;
    const xy = up[2] * z0 - up[0] * z2;
    const xz = up[0] * z1 - up[1] * z0;
    len = Math.sqrt(xx * xx + xy * xy + xz * xz);
    const x0 = xx / len, x1 = xy / len, x2 = xz / len;

    const y0 = z1 * x2 - z2 * x1;
    const y1 = z2 * x0 - z0 * x2;
    const y2 = z0 * x1 - z1 * x0;

    out[0] = x0; out[1] = y0; out[2] = z0; out[3] = 0;
    out[4] = x1; out[5] = y1; out[6] = z1; out[7] = 0;
    out[8] = x2; out[9] = y2; out[10] = z2; out[11] = 0;
    out[12] = -(x0 * eye[0] + x1 * eye[1] + x2 * eye[2]);
    out[13] = -(y0 * eye[0] + y1 * eye[1] + y2 * eye[2]);
    out[14] = -(z0 * eye[0] + z1 * eye[1] + z2 * eye[2]);
    out[15] = 1;
    return out;
  },

  rotateY(out, a, rad) {
    const s = Math.sin(rad), c = Math.cos(rad);
    const a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
    const a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];

    for (let i = 0; i < 16; i++) out[i] = a[i];
    out[0] = a00 * c - a20 * s;
    out[1] = a01 * c - a21 * s;
    out[2] = a02 * c - a22 * s;
    out[3] = a03 * c - a23 * s;
    out[8] = a00 * s + a20 * c;
    out[9] = a01 * s + a21 * c;
    out[10] = a02 * s + a22 * c;
    out[11] = a03 * s + a23 * c;
    return out;
  },

  multiply(out, a, b) {
    const result = new Float32Array(16);
    for (let i = 0; i < 4; i++) {
      for (let j = 0; j < 4; j++) {
        result[j * 4 + i] =
          a[0 * 4 + i] * b[j * 4 + 0] +
          a[1 * 4 + i] * b[j * 4 + 1] +
          a[2 * 4 + i] * b[j * 4 + 2] +
          a[3 * 4 + i] * b[j * 4 + 3];
      }
    }
    for (let i = 0; i < 16; i++) out[i] = result[i];
    return out;
  }
};

// ============================================================================
// SHADERS - Hardcoded cube vertices in shader
// ============================================================================

const shaderCode = `
struct Uniforms {
    mvp: mat4x4f,
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec3f,
}

@vertex
fn vertexMain(@builtin(vertex_index) idx: u32) -> VertexOutput {
    // 36 vertices for cube (6 faces * 2 triangles * 3 vertices)
    var positions = array<vec3f, 36>(
        // Front face (Z+) - Red
        vec3f(-0.5, -0.5,  0.5), vec3f( 0.5, -0.5,  0.5), vec3f( 0.5,  0.5,  0.5),
        vec3f(-0.5, -0.5,  0.5), vec3f( 0.5,  0.5,  0.5), vec3f(-0.5,  0.5,  0.5),
        // Back face (Z-) - Cyan
        vec3f( 0.5, -0.5, -0.5), vec3f(-0.5, -0.5, -0.5), vec3f(-0.5,  0.5, -0.5),
        vec3f( 0.5, -0.5, -0.5), vec3f(-0.5,  0.5, -0.5), vec3f( 0.5,  0.5, -0.5),
        // Top face (Y+) - Green
        vec3f(-0.5,  0.5,  0.5), vec3f( 0.5,  0.5,  0.5), vec3f( 0.5,  0.5, -0.5),
        vec3f(-0.5,  0.5,  0.5), vec3f( 0.5,  0.5, -0.5), vec3f(-0.5,  0.5, -0.5),
        // Bottom face (Y-) - Magenta
        vec3f(-0.5, -0.5, -0.5), vec3f( 0.5, -0.5, -0.5), vec3f( 0.5, -0.5,  0.5),
        vec3f(-0.5, -0.5, -0.5), vec3f( 0.5, -0.5,  0.5), vec3f(-0.5, -0.5,  0.5),
        // Right face (X+) - Blue
        vec3f( 0.5, -0.5,  0.5), vec3f( 0.5, -0.5, -0.5), vec3f( 0.5,  0.5, -0.5),
        vec3f( 0.5, -0.5,  0.5), vec3f( 0.5,  0.5, -0.5), vec3f( 0.5,  0.5,  0.5),
        // Left face (X-) - Yellow
        vec3f(-0.5, -0.5, -0.5), vec3f(-0.5, -0.5,  0.5), vec3f(-0.5,  0.5,  0.5),
        vec3f(-0.5, -0.5, -0.5), vec3f(-0.5,  0.5,  0.5), vec3f(-0.5,  0.5, -0.5),
    );

    var colors = array<vec3f, 6>(
        vec3f(1.0, 0.0, 0.0), // Red
        vec3f(0.0, 1.0, 1.0), // Cyan
        vec3f(0.0, 1.0, 0.0), // Green
        vec3f(1.0, 0.0, 1.0), // Magenta
        vec3f(0.0, 0.0, 1.0), // Blue
        vec3f(1.0, 1.0, 0.0), // Yellow
    );

    var output: VertexOutput;
    output.position = uniforms.mvp * vec4f(positions[idx], 1.0);
    output.color = colors[idx / 6u]; // Each face has 6 vertices
    return output;
}

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
    return vec4f(input.color, 1.0);
}
`;

// ============================================================================
// MAIN
// ============================================================================

async function main() {
  if (!navigator.gpu) {
    console.error("WebGPU not supported!");
    return;
  }

  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    console.error("No adapter found!");
    return;
  }

  const device = await adapter.requestDevice();
  console.log("Device acquired");

  const context = canvas.getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();

  context.configure({
    device: device,
    format: format,
    alphaMode: "opaque",
  });

  // Create uniform buffer for MVP matrix (64 bytes)
  const uniformBuffer = device.createBuffer({
    size: 64,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  console.log("Uniform buffer created");

  // Create bind group layout
  const bindGroupLayout = device.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: GPUShaderStage.VERTEX,
        buffer: { type: "uniform" },
      },
    ],
  });

  // Create bind group
  const bindGroup = device.createBindGroup({
    layout: bindGroupLayout,
    entries: [
      { binding: 0, resource: { buffer: uniformBuffer } },
    ],
  });
  console.log("Bind group created");

  // Create pipeline layout
  const pipelineLayout = device.createPipelineLayout({
    bindGroupLayouts: [bindGroupLayout],
  });

  // Create shader module
  const shaderModule = device.createShaderModule({
    code: shaderCode,
  });

  // Create render pipeline (no depth for simplicity first)
  const pipeline = device.createRenderPipeline({
    layout: pipelineLayout,
    vertex: {
      module: shaderModule,
      entryPoint: "vertexMain",
    },
    fragment: {
      module: shaderModule,
      entryPoint: "fragmentMain",
      targets: [{ format: format }],
    },
    primitive: {
      topology: "triangle-list",
    },
  });
  console.log("Pipeline created");

  // Initialize matrices
  const projection = mat4.create();
  const view = mat4.create();
  const model = mat4.create();
  const mvp = mat4.create();
  const tempMat = mat4.create();

  const aspect = canvas.width / canvas.height;
  mat4.perspective(projection, (60 * Math.PI) / 180, aspect, 0.1, 100);
  mat4.lookAt(view, [0, 1, 3], [0, 0, 0], [0, 1, 0]);

  let frameCount = 0;
  let rotation = 0;

  function render() {
    rotation += 0.02;

    mat4.identity(model);
    mat4.rotateY(model, model, rotation);

    mat4.multiply(tempMat, view, model);
    mat4.multiply(mvp, projection, tempMat);

    // Upload MVP matrix
    device.queue.writeBuffer(uniformBuffer, 0, mvp);

    const commandEncoder = device.createCommandEncoder();

    const renderPass = commandEncoder.beginRenderPass({
      colorAttachments: [
        {
          view: context.getCurrentTexture().createView(),
          loadOp: "clear",
          storeOp: "store",
          clearValue: { r: 0.1, g: 0.1, b: 0.15, a: 1.0 },
        },
      ],
    });

    renderPass.setPipeline(pipeline);
    renderPass.setBindGroup(0, bindGroup);
    renderPass.draw(36); // 36 vertices for cube
    renderPass.end();

    device.queue.submit([commandEncoder.finish()]);

    frameCount++;
    if (frameCount % 120 === 0) {
      console.log("Frame:", frameCount);
    }

    requestAnimationFrame(render);
  }

  requestAnimationFrame(render);
  console.log("Render loop started - you should see a rotating colored cube!");
}

main().catch(console.error);
