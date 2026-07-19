/**
 * iOS GLTF Viewer with Touch Rotation
 *
 * Loads the DamagedHelmet model and allows touch-based rotation.
 * Uses the same PBR rendering approach as the desktop gltf-render example.
 */

console.log("iOS GLTF Viewer starting...");

// Skybox shader - renders the environment cube as background
// Uses a vertex buffer for broad shader compatibility.
const skyboxShaderCode = /* wgsl */`
    struct SceneUniforms {
        viewMatrix: mat4x4f,
        projectionMatrix: mat4x4f,
        cameraPosition: vec3f,
        environmentIntensity: f32,
    }

    @group(0) @binding(0) var<uniform> scene: SceneUniforms;
    @group(0) @binding(1) var envMap: texture_cube<f32>;
    @group(0) @binding(2) var envSampler: sampler;

    struct VertexOutput {
        @builtin(position) position: vec4f,
        @location(0) uvw: vec3f,
    }

    @vertex
    fn vs_main(@location(0) inPos: vec3f) -> VertexOutput {
        var output: VertexOutput;
        output.uvw = inPos;
        var view = scene.viewMatrix;
        view[3] = vec4f(0.0, 0.0, 0.0, 1.0);
        let clipPos = scene.projectionMatrix * view * vec4f(inPos, 1.0);
        output.position = clipPos.xyww;
        return output;
    }

    @fragment
    fn fs_main(input: VertexOutput) -> @location(0) vec4f {
        var color = textureSample(envMap, envSampler, normalize(input.uvw)).rgb;
        color *= scene.environmentIntensity;
        color = color / (color + vec3f(1.0));
        color = pow(color, vec3f(1.0 / 2.2));
        return vec4f(color, 1.0);
    }
`;

// Skybox cube vertex data (36 vertices for 6 faces)
const skyboxVertices = new Float32Array([
    -1,  1, -1,  -1, -1, -1,   1, -1, -1,
     1, -1, -1,   1,  1, -1,  -1,  1, -1,
    -1, -1,  1,  -1, -1, -1,  -1,  1, -1,
    -1,  1, -1,  -1,  1,  1,  -1, -1,  1,
     1, -1, -1,   1, -1,  1,   1,  1,  1,
     1,  1,  1,   1,  1, -1,   1, -1, -1,
    -1, -1,  1,  -1,  1,  1,   1,  1,  1,
     1,  1,  1,   1, -1,  1,  -1, -1,  1,
    -1,  1, -1,   1,  1, -1,   1,  1,  1,
     1,  1,  1,  -1,  1,  1,  -1,  1, -1,
    -1, -1, -1,  -1, -1,  1,   1, -1, -1,
     1, -1, -1,  -1, -1,  1,   1, -1,  1
]);

// PBR Shader with IBL
const pbrShaderCode = /* wgsl */`
    const PI = 3.14159265359;

    struct SceneUniforms {
        viewMatrix: mat4x4f,
        projectionMatrix: mat4x4f,
        cameraPosition: vec3f,
        environmentIntensity: f32,
    }

    struct ModelUniforms {
        modelMatrix: mat4x4f,
        normalMatrix: mat4x4f,
    }

    struct MaterialUniforms {
        baseColor: vec3f,
        metallic: f32,
        roughness: f32,
        emissiveFactor: f32,
        textureFlags: u32,
        _pad: f32,
    }

    @group(0) @binding(0) var<uniform> scene: SceneUniforms;
    @group(0) @binding(1) var envMap: texture_cube<f32>;
    @group(0) @binding(2) var envSampler: sampler;

    @group(1) @binding(0) var<uniform> model: ModelUniforms;

    @group(2) @binding(0) var<uniform> material: MaterialUniforms;
    @group(2) @binding(1) var texSampler: sampler;
    @group(2) @binding(2) var baseColorTex: texture_2d<f32>;
    @group(2) @binding(3) var normalTex: texture_2d<f32>;
    @group(2) @binding(4) var metallicRoughnessTex: texture_2d<f32>;
    @group(2) @binding(5) var emissiveTex: texture_2d<f32>;
    @group(2) @binding(6) var occlusionTex: texture_2d<f32>;

    struct VertexInput {
        @location(0) position: vec3f,
        @location(1) normal: vec3f,
        @location(2) uv: vec2f,
    }

    struct VertexOutput {
        @builtin(position) position: vec4f,
        @location(0) worldPosition: vec3f,
        @location(1) worldNormal: vec3f,
        @location(2) uv: vec2f,
    }

    @vertex
    fn vs_main(input: VertexInput) -> VertexOutput {
        var output: VertexOutput;
        let worldPos = model.modelMatrix * vec4f(input.position, 1.0);
        output.worldPosition = worldPos.xyz;
        output.position = scene.projectionMatrix * scene.viewMatrix * worldPos;
        output.worldNormal = (model.normalMatrix * vec4f(input.normal, 0.0)).xyz;
        output.uv = input.uv;
        return output;
    }

    fn distributionGGX(N: vec3f, H: vec3f, roughness: f32) -> f32 {
        let a = roughness * roughness;
        let a2 = a * a;
        let NdotH = max(dot(N, H), 0.0);
        let NdotH2 = NdotH * NdotH;
        let num = a2;
        let denom = NdotH2 * (a2 - 1.0) + 1.0;
        return num / (PI * denom * denom);
    }

    fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
        let r = roughness + 1.0;
        let k = (r * r) / 8.0;
        return NdotV / (NdotV * (1.0 - k) + k);
    }

    fn geometrySmith(N: vec3f, V: vec3f, L: vec3f, roughness: f32) -> f32 {
        let NdotV = max(dot(N, V), 0.0);
        let NdotL = max(dot(N, L), 0.0);
        return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
    }

    fn fresnelSchlick(cosTheta: f32, F0: vec3f) -> vec3f {
        return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    }

    const HAS_BASE_COLOR_TEX = 1u;
    const HAS_NORMAL_TEX = 2u;
    const HAS_METALLIC_ROUGHNESS_TEX = 4u;
    const HAS_EMISSIVE_TEX = 8u;
    const HAS_OCCLUSION_TEX = 16u;

    @fragment
    fn fs_main(input: VertexOutput) -> @location(0) vec4f {
        var N = normalize(input.worldNormal);
        let V = normalize(scene.cameraPosition - input.worldPosition);
        let uv = input.uv;

        var baseColor = material.baseColor;
        if ((material.textureFlags & HAS_BASE_COLOR_TEX) != 0u) {
            let texColor = textureSample(baseColorTex, texSampler, uv);
            baseColor = baseColor * texColor.rgb;
        }

        if ((material.textureFlags & HAS_NORMAL_TEX) != 0u) {
            let mapNormal = textureSample(normalTex, texSampler, uv).xyz * 2.0 - 1.0;
            let Q1 = dpdx(input.worldPosition);
            let Q2 = dpdy(input.worldPosition);
            let st1 = dpdx(uv);
            let st2 = dpdy(uv);
            let T = normalize(Q1 * st2.y - Q2 * st1.y);
            let B = -normalize(cross(N, T));
            let TBN = mat3x3f(T, B, N);
            N = normalize(TBN * mapNormal);
        }

        var metallic = material.metallic;
        var roughness = material.roughness;
        if ((material.textureFlags & HAS_METALLIC_ROUGHNESS_TEX) != 0u) {
            let mrSample = textureSample(metallicRoughnessTex, texSampler, uv);
            roughness = mrSample.g * material.roughness;
            metallic = mrSample.b * material.metallic;
        }
        roughness = max(roughness, 0.04);

        var emissive = vec3f(0.0);
        if ((material.textureFlags & HAS_EMISSIVE_TEX) != 0u) {
            emissive = textureSample(emissiveTex, texSampler, uv).rgb * material.emissiveFactor;
        }

        var occlusion = 1.0;
        if ((material.textureFlags & HAS_OCCLUSION_TEX) != 0u) {
            occlusion = textureSample(occlusionTex, texSampler, uv).r;
        }

        var F0 = vec3f(0.04);
        F0 = mix(F0, baseColor, metallic);

        let R = reflect(-V, N);
        let NdotV = max(dot(N, V), 0.0);

        let numLevels = f32(textureNumLevels(envMap));
        let lod = roughness * (numLevels - 1.0);
        let iblSpecular = textureSampleLevel(envMap, envSampler, R, lod).rgb;

        let diffuseLOD = numLevels - 2.0;
        let skyColor = textureSampleLevel(envMap, envSampler, vec3f(0.0, 1.0, 0.0), diffuseLOD).rgb;
        let groundColor = textureSampleLevel(envMap, envSampler, vec3f(0.0, -1.0, 0.0), diffuseLOD).rgb;
        let hemiMix = N.y * 0.5 + 0.5;
        let iblDiffuse = mix(groundColor, skyColor, hemiMix);

        let F_IBL = fresnelSchlick(NdotV, F0);
        let kS_IBL = F_IBL;
        var kD_IBL = vec3f(1.0) - kS_IBL;
        kD_IBL *= 1.0 - metallic;

        let ambientIBL = (kD_IBL * baseColor * iblDiffuse + iblSpecular * F_IBL) * scene.environmentIntensity * occlusion;

        // Direct sunlight
        var Lo = vec3f(0.0);
        let sunDir = normalize(vec3f(1.0, 1.0, 0.5));
        let sunColor = vec3f(1.0, 0.98, 0.95);
        let sunIntensity = 5.0;

        let L = sunDir;
        let H = normalize(V + L);
        let radiance = sunColor * sunIntensity;

        let NDF = distributionGGX(N, H, roughness);
        let G = geometrySmith(N, V, L, roughness);
        let F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        let numerator = NDF * G * F;
        let NdotL = max(dot(N, L), 0.0);
        let denominator = 4.0 * NdotV * NdotL + 0.0001;
        let specular = numerator / denominator;

        let kS = F;
        var kD = vec3f(1.0) - kS;
        kD *= 1.0 - metallic;

        Lo = (kD * baseColor / PI + specular) * radiance * NdotL;

        var color = ambientIBL + Lo + emissive;
        color = color / (color + vec3f(1.0));
        color = pow(color, vec3f(1.0 / 2.2));

        return vec4f(color, 1.0);
    }
`;

// Global state for touch rotation
let rotationX = 0;
let rotationY = 0;
let lastTouchX = 0;
let lastTouchY = 0;
let isDragging = false;

// Handle touch/mouse events
function setupTouchHandlers() {
    const canvasEl = document.getElementById('canvas');
    if (!canvasEl) {
        console.log("Canvas not found, touch handlers not set up");
        return;
    }

    // Touch events
    canvasEl.addEventListener('touchstart', (e) => {
        e.preventDefault();
        if (e.touches.length > 0) {
            isDragging = true;
            lastTouchX = e.touches[0].clientX;
            lastTouchY = e.touches[0].clientY;
        }
    });

    canvasEl.addEventListener('touchmove', (e) => {
        e.preventDefault();
        if (isDragging && e.touches.length > 0) {
            const deltaX = e.touches[0].clientX - lastTouchX;
            const deltaY = e.touches[0].clientY - lastTouchY;
            rotationY += deltaX * 0.01;
            rotationX += deltaY * 0.01;
            // Clamp vertical rotation
            rotationX = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, rotationX));
            lastTouchX = e.touches[0].clientX;
            lastTouchY = e.touches[0].clientY;
        }
    });

    canvasEl.addEventListener('touchend', () => {
        isDragging = false;
    });

    // Mouse events (for simulator)
    canvasEl.addEventListener('mousedown', (e) => {
        isDragging = true;
        lastTouchX = e.clientX;
        lastTouchY = e.clientY;
    });

    canvasEl.addEventListener('mousemove', (e) => {
        if (isDragging) {
            const deltaX = e.clientX - lastTouchX;
            const deltaY = e.clientY - lastTouchY;
            rotationY += deltaX * 0.01;
            rotationX += deltaY * 0.01;
            rotationX = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, rotationX));
            lastTouchX = e.clientX;
            lastTouchY = e.clientY;
        }
    });

    canvasEl.addEventListener('mouseup', () => {
        isDragging = false;
    });

    console.log("Touch handlers set up");
}

// HDR Loader
function parseHDR(buffer) {
    const data = new Uint8Array(buffer);
    let pos = 0;

    while (pos < data.length) {
        let line = '';
        while (pos < data.length && data[pos] !== 0x0a) {
            line += String.fromCharCode(data[pos]);
            pos++;
        }
        pos++;
        if (line.endsWith('\r')) line = line.slice(0, -1);
        if (line.length === 0) break;
    }

    let width = 0;
    let height = 0;
    let resLine = '';
    while (pos < data.length && data[pos] !== 0x0a) {
        resLine += String.fromCharCode(data[pos]);
        pos++;
    }
    pos++;
    if (resLine.endsWith('\r')) resLine = resLine.slice(0, -1);

    const parts = resLine.trim().split(/\s+/);
    for (let i = 0; i < parts.length; i++) {
        if (parts[i] === '-Y' || parts[i] === '+Y') height = parseInt(parts[i+1]);
        if (parts[i] === '+X' || parts[i] === '-X') width = parseInt(parts[i+1]);
    }

    if (width === 0 || height === 0) {
        throw new Error(`HDR: Invalid resolution: "${resLine}"`);
    }

    const floatData = new Float32Array(width * height * 4);
    const scanlineBuffer = new Uint8Array(width * 4);
    let ptr = pos;

    for (let y = 0; y < height; y++) {
        const idxStart = y * width * 4;

        if (ptr + 4 <= data.length && data[ptr] === 2 && data[ptr+1] === 2 &&
            data[ptr+2] === ((width >> 8) & 0xFF) && data[ptr+3] === (width & 0xFF)) {
            ptr += 4;

            for (let ch = 0; ch < 4; ch++) {
                let extracted = 0;
                while (extracted < width) {
                    const val = data[ptr++];
                    if (val > 128) {
                        const count = val - 128;
                        const runVal = data[ptr++];
                        for (let i = 0; i < count; i++) {
                            scanlineBuffer[extracted + ch * width] = runVal;
                            extracted++;
                        }
                    } else {
                        const count = val;
                        for (let i = 0; i < count; i++) {
                            scanlineBuffer[extracted + ch * width] = data[ptr++];
                            extracted++;
                        }
                    }
                }
            }

            for (let x = 0; x < width; x++) {
                const r = scanlineBuffer[x];
                const g = scanlineBuffer[x + width];
                const b = scanlineBuffer[x + 2 * width];
                const e = scanlineBuffer[x + 3 * width];

                const outIdx = idxStart + x * 4;
                if (e === 0) {
                    floatData[outIdx] = 0; floatData[outIdx+1] = 0; floatData[outIdx+2] = 0; floatData[outIdx+3] = 1;
                } else {
                    const f = Math.pow(2.0, e - 128 - 8);
                    floatData[outIdx] = r * f;
                    floatData[outIdx+1] = g * f;
                    floatData[outIdx+2] = b * f;
                    floatData[outIdx+3] = 1;
                }
            }
        } else {
            for (let x = 0; x < width; x++) {
                const r = data[ptr++];
                const g = data[ptr++];
                const b = data[ptr++];
                const e = data[ptr++];
                const outIdx = idxStart + x * 4;

                if (e === 0) {
                    floatData[outIdx] = 0; floatData[outIdx+1] = 0; floatData[outIdx+2] = 0; floatData[outIdx+3] = 1;
                } else {
                    const f = Math.pow(2.0, e - 128 - 8);
                    floatData[outIdx] = r * f;
                    floatData[outIdx+1] = g * f;
                    floatData[outIdx+2] = b * f;
                    floatData[outIdx+3] = 1;
                }
            }
        }
    }

    return { width, height, data: floatData };
}

function packToHalf(data) {
    const out = new Uint16Array(data.length);
    const floatView = new Float32Array(1);
    const int32View = new Int32Array(floatView.buffer);
    const HALF_MAX = 0x7bff;

    for (let i = 0; i < data.length; i++) {
        const val = data[i];
        if (Number.isNaN(val)) { out[i] = 0; continue; }

        floatView[0] = val;
        const x = int32View[0];
        const bits = (x >> 16) & 0x8000;
        let m = (x >> 12) & 0x07ff;
        const e = (x >> 23) & 0xff;

        if (e < 103) { out[i] = bits; continue; }
        if (e > 142) { out[i] = bits | HALF_MAX; continue; }
        if (e < 113) { m |= 0x0800; out[i] = bits | ((m >> (114 - e)) + ((m >> (113 - e)) & 1)); continue; }
        out[i] = bits | ((e - 112) << 10) | ((m >> 1) + (m & 1));
    }
    return out;
}

async function loadEnvironmentMap(device, url, resolution = 512) {
    console.log("Loading HDR environment map: " + url);

    const response = await fetch(url);
    const buffer = await response.arrayBuffer();
    const hdr = parseHDR(buffer);
    const width = hdr.width;
    const height = hdr.height;
    const equirectData = hdr.data;

    console.log("Loaded HDR: " + width + "x" + height);

    function getDirection(face, u, v) {
        const uc = 2.0 * u - 1.0;
        const vc = 2.0 * v - 1.0;
        switch (face) {
            case 0: return [1, -vc, -uc];
            case 1: return [-1, -vc, uc];
            case 2: return [uc, 1, vc];
            case 3: return [uc, -1, -vc];
            case 4: return [uc, -vc, 1];
            case 5: return [-uc, -vc, -1];
        }
        return [0, 0, 0];
    }

    function normalize(v) {
        const len = Math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        return len > 0 ? [v[0]/len, v[1]/len, v[2]/len] : [0, 0, 0];
    }

    function dirToEquirect(dir) {
        const d = normalize(dir);
        const phi = Math.atan2(d[2], d[0]);
        const theta = Math.acos(Math.max(-1, Math.min(1, d[1])));
        const u = (phi + Math.PI) / (2 * Math.PI);
        const v = theta / Math.PI;
        return [u, v];
    }

    function sampleEquirect(u, v) {
        const fx = u * width - 0.5;
        const fy = v * height - 0.5;
        const x0 = Math.floor(fx);
        const y0 = Math.floor(fy);
        const x1 = (x0 + 1) % width;
        const y1 = Math.min(y0 + 1, height - 1);
        const wx = fx - x0;
        const wy = fy - y0;

        const getPixel = (x, y) => {
            const px = ((x % width) + width) % width;
            const py = Math.max(0, Math.min(height - 1, y));
            const idx = (py * width + px) * 4;
            return [equirectData[idx], equirectData[idx+1], equirectData[idx+2], equirectData[idx+3]];
        };

        const p00 = getPixel(x0, y0);
        const p10 = getPixel(x1, y0);
        const p01 = getPixel(x0, y1);
        const p11 = getPixel(x1, y1);

        const r = p00[0]*(1-wx)*(1-wy) + p10[0]*wx*(1-wy) + p01[0]*(1-wx)*wy + p11[0]*wx*wy;
        const g = p00[1]*(1-wx)*(1-wy) + p10[1]*wx*(1-wy) + p01[1]*(1-wx)*wy + p11[1]*wx*wy;
        const b = p00[2]*(1-wx)*(1-wy) + p10[2]*wx*(1-wy) + p01[2]*(1-wx)*wy + p11[2]*wx*wy;
        const a = p00[3]*(1-wx)*(1-wy) + p10[3]*wx*(1-wy) + p01[3]*(1-wx)*wy + p11[3]*wx*wy;

        return [r, g, b, a];
    }

    const mipLevels = Math.floor(Math.log2(resolution)) + 1;
    const cubeTexture = device.createTexture({
        label: 'Environment Cubemap HDR',
        size: [resolution, resolution, 6],
        format: 'rgba16float',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        mipLevelCount: mipLevels,
    });

    const basePixels = [];

    for (let face = 0; face < 6; face++) {
        const faceData = new Float32Array(resolution * resolution * 4);

        for (let y = 0; y < resolution; y++) {
            for (let x = 0; x < resolution; x++) {
                const u = (x + 0.5) / resolution;
                const v = (y + 0.5) / resolution;
                const dir = getDirection(face, u, v);
                const [eu, ev] = dirToEquirect(dir);
                const [r, g, b, a] = sampleEquirect(eu, ev);

                const idx = (y * resolution + x) * 4;
                faceData[idx] = r;
                faceData[idx + 1] = g;
                faceData[idx + 2] = b;
                faceData[idx + 3] = a;
            }
        }

        basePixels.push(faceData);

        const halfData = packToHalf(faceData);
        device.queue.writeTexture(
            { texture: cubeTexture, origin: [0, 0, face], mipLevel: 0 },
            halfData,
            { bytesPerRow: resolution * 8, rowsPerImage: resolution },
            [resolution, resolution, 1]
        );
    }

    // Generate mipmaps
    let currentRes = resolution;
    let currentPixels = basePixels;

    for (let level = 1; level < mipLevels; level++) {
        const nextRes = Math.floor(currentRes / 2);
        if (nextRes < 1) break;

        const nextPixels = [];

        for (let face = 0; face < 6; face++) {
            const src = currentPixels[face];
            const dst = new Float32Array(nextRes * nextRes * 4);

            for (let y = 0; y < nextRes; y++) {
                for (let x = 0; x < nextRes; x++) {
                    const srcX = x * 2;
                    const srcY = y * 2;
                    let r = 0, g = 0, b = 0, a = 0;

                    for (let dy = 0; dy < 2; dy++) {
                        for (let dx = 0; dx < 2; dx++) {
                            const sIdx = ((srcY + dy) * currentRes + (srcX + dx)) * 4;
                            r += src[sIdx]; g += src[sIdx + 1]; b += src[sIdx + 2]; a += src[sIdx + 3];
                        }
                    }

                    const idx = (y * nextRes + x) * 4;
                    dst[idx] = r / 4;
                    dst[idx + 1] = g / 4;
                    dst[idx + 2] = b / 4;
                    dst[idx + 3] = a / 4;
                }
            }

            nextPixels.push(dst);

            const halfData = packToHalf(dst);
            device.queue.writeTexture(
                { texture: cubeTexture, origin: [0, 0, face], mipLevel: level },
                halfData,
                { bytesPerRow: nextRes * 8, rowsPerImage: nextRes },
                [nextRes, nextRes, 1]
            );
        }

        currentRes = nextRes;
        currentPixels = nextPixels;
    }

    const view = device.createTextureView(cubeTexture, {
        dimension: 'cube',
        mipLevelCount: mipLevels,
        arrayLayerCount: 6,
    });
    const sampler = device.createSampler({
        magFilter: 'linear',
        minFilter: 'linear',
        mipmapFilter: 'linear',
    });

    console.log("Created environment cubemap: " + resolution + "x" + resolution + " with " + mipLevels + " mip levels");

    return { texture: cubeTexture, view, sampler };
}

// Matrix utilities
function identity() {
    return new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    ]);
}

function multiply(a, b) {
    const result = new Float32Array(16);
    for (let i = 0; i < 4; i++) {
        for (let j = 0; j < 4; j++) {
            let sum = 0;
            for (let k = 0; k < 4; k++) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            result[i * 4 + j] = sum;
        }
    }
    return result;
}

function rotateX(angle) {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return new Float32Array([
        1, 0, 0, 0,
        0, c, -s, 0,
        0, s, c, 0,
        0, 0, 0, 1,
    ]);
}

function rotateY(angle) {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return new Float32Array([
        c, 0, s, 0,
        0, 1, 0, 0,
        -s, 0, c, 0,
        0, 0, 0, 1,
    ]);
}

function perspective(fov, aspect, near, far) {
    const f = 1.0 / Math.tan(fov / 2);
    const nf = 1 / (near - far);
    return new Float32Array([
        f / aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far + near) * nf, -1,
        0, 0, 2 * far * near * nf, 0,
    ]);
}

function lookAt(eye, target, up) {
    const zAxis = normalize3(subtract3(eye, target));
    const xAxis = normalize3(cross3(up, zAxis));
    const yAxis = cross3(zAxis, xAxis);

    return new Float32Array([
        xAxis[0], yAxis[0], zAxis[0], 0,
        xAxis[1], yAxis[1], zAxis[1], 0,
        xAxis[2], yAxis[2], zAxis[2], 0,
        -dot3(xAxis, eye), -dot3(yAxis, eye), -dot3(zAxis, eye), 1,
    ]);
}

function subtract3(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function cross3(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function dot3(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function normalize3(v) {
    const len = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return len > 0 ? [v[0] / len, v[1] / len, v[2] / len] : [0, 0, 0];
}

// Main initialization
async function init() {
    setupTouchHandlers();

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error("No WebGPU adapter");

    const device = await adapter.requestDevice();
    console.log("WebGPU device acquired");

    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('webgpu');
    const format = navigator.gpu.getPreferredCanvasFormat();
    ctx.configure({ device, format });

    // Load GLTF - path will be set by the native app
    const helmetPath = globalThis.__GLTF_PATH__ || "DamagedHelmet.glb";
    console.log("Loading GLTF: " + helmetPath);

    const gltf = await loadGLTF(helmetPath);
    console.log("Loaded GLTF: " + gltf.meshes.length + " meshes");

    // Load environment map - path will be set by the native app
    const envMapPath = globalThis.__ENV_MAP_PATH__ || "sunny_rose_garden_2k.hdr";
    const envCube = await loadEnvironmentMap(device, envMapPath, 256);

    const mesh = gltf.meshes[0];
    const primitive = mesh.primitives[0];
    const mat = gltf.materials[primitive.materialIndex];

    console.log("Mesh: " + mesh.name + " with " + primitive.vertexCount + " vertices");

    // Create typed array views from ArrayBuffer data
    const positions = new Float32Array(primitive.positions);
    const normals = primitive.normals ? new Float32Array(primitive.normals) : null;
    const texcoords = primitive.texcoords ? new Float32Array(primitive.texcoords) : null;

    // Create vertex buffer
    const vertexCount = primitive.vertexCount;
    const vertexData = new Float32Array(vertexCount * 8);

    for (let i = 0; i < vertexCount; i++) {
        vertexData[i * 8 + 0] = positions[i * 3 + 0];
        vertexData[i * 8 + 1] = positions[i * 3 + 1];
        vertexData[i * 8 + 2] = positions[i * 3 + 2];
        if (normals) {
            vertexData[i * 8 + 3] = normals[i * 3 + 0];
            vertexData[i * 8 + 4] = normals[i * 3 + 1];
            vertexData[i * 8 + 5] = normals[i * 3 + 2];
        }
        if (texcoords) {
            vertexData[i * 8 + 6] = texcoords[i * 2 + 0];
            vertexData[i * 8 + 7] = texcoords[i * 2 + 1];
        }
    }

    const vertexBuffer = device.createBuffer({
        size: vertexData.byteLength,
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(vertexBuffer, 0, vertexData);

    const indexBuffer = device.createBuffer({
        size: primitive.indices.byteLength,
        usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(indexBuffer, 0, primitive.indices);

    // Create textures from GLTF images
    async function createTextureFromImage(imageIndex) {
        if (imageIndex === undefined || imageIndex < 0 || imageIndex >= gltf.images.length) {
            console.log("Image index invalid or out of range: " + imageIndex);
            return null;
        }

        const img = gltf.images[imageIndex];
        if (!img) {
            console.log("Image " + imageIndex + " is null/undefined");
            return null;
        }

        console.log("Image " + imageIndex + ": name=" + (img.name || "") + " uri=" + (img.uri || "") + " mimeType=" + (img.mimeType || "") + " hasData=" + (img.data ? "yes (" + img.data.byteLength + " bytes)" : "no"));

        if (!img.data) {
            console.log("Image " + imageIndex + " has no embedded data, creating white texture");
            return createWhiteTexture();
        }

        const blob = new Blob([img.data], { type: img.mimeType || 'image/png' });
        const bitmap = await createImageBitmap(blob);

        const texture = device.createTexture({
            size: [bitmap.width, bitmap.height],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
        });

        device.queue.copyExternalImageToTexture(
            { source: bitmap },
            { texture },
            [bitmap.width, bitmap.height]
        );

        return texture;
    }

    function createWhiteTexture() {
        const texture = device.createTexture({
            size: [1, 1],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        });
        device.queue.writeTexture({ texture }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
        return texture;
    }

    function createFlatNormalTexture() {
        const texture = device.createTexture({
            size: [1, 1],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        });
        device.queue.writeTexture({ texture }, new Uint8Array([128, 128, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
        return texture;
    }

    const baseColorTex = await createTextureFromImage(mat.baseColorTextureIndex) || createWhiteTexture();
    const normalTex = await createTextureFromImage(mat.normalTextureIndex) || createFlatNormalTexture();
    const metallicRoughnessTex = await createTextureFromImage(mat.metallicRoughnessTextureIndex) || createWhiteTexture();
    const emissiveTex = await createTextureFromImage(mat.emissiveTextureIndex) || createWhiteTexture();
    const occlusionTex = await createTextureFromImage(mat.occlusionTextureIndex) || createWhiteTexture();

    const textureSampler = device.createSampler({
        magFilter: 'linear',
        minFilter: 'linear',
        mipmapFilter: 'linear',
        addressModeU: 'repeat',
        addressModeV: 'repeat',
    });

    const depthTexture = device.createTexture({
        size: [canvas.width, canvas.height],
        format: 'depth24plus',
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
    });

    // Create bind group layouts and pipelines
    // SceneUniforms: viewMatrix(64) + projMatrix(64) + cameraPos(12) + envIntensity(4) + padding(4) = 148
    // Aligned to 16 bytes = 160 bytes to be safe
    const sceneUniformBuffer = device.createBuffer({
        size: 160,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    const sceneBindGroupLayout = device.createBindGroupLayout({
        entries: [
            { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: 'cube' } },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        ],
    });

    const sceneBindGroup = device.createBindGroup({
        layout: sceneBindGroupLayout,
        entries: [
            { binding: 0, resource: { buffer: sceneUniformBuffer } },
            { binding: 1, resource: envCube.view },
            { binding: 2, resource: envCube.sampler },
        ],
    });

    // Skybox vertex buffer
    const skyboxVertexBuffer = device.createBuffer({
        size: skyboxVertices.byteLength,
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(skyboxVertexBuffer, 0, skyboxVertices);

    // Skybox pipeline
    const skyboxShaderModule = device.createShaderModule({ code: skyboxShaderCode });
    const skyboxPipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [sceneBindGroupLayout] });
    const skyboxPipeline = device.createRenderPipeline({
        layout: skyboxPipelineLayout,
        vertex: {
            module: skyboxShaderModule,
            entryPoint: 'vs_main',
            buffers: [{
                arrayStride: 12,  // 3 floats per vertex
                attributes: [
                    { shaderLocation: 0, offset: 0, format: 'float32x3' },
                ],
            }],
        },
        fragment: { module: skyboxShaderModule, entryPoint: 'fs_main', targets: [{ format }] },
        primitive: { topology: 'triangle-list', cullMode: 'none' },
        depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'less-equal' },
    });

    // PBR pipeline
    const modelUniformBuffer = device.createBuffer({ size: 128, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    const materialUniformBuffer = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

    const modelBindGroupLayout = device.createBindGroupLayout({
        entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } }],
    });

    const materialBindGroupLayout = device.createBindGroupLayout({
        entries: [
            { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 4, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 5, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
            { binding: 6, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        ],
    });

    const modelBindGroup = device.createBindGroup({
        layout: modelBindGroupLayout,
        entries: [{ binding: 0, resource: { buffer: modelUniformBuffer } }],
    });

    const materialBindGroup = device.createBindGroup({
        layout: materialBindGroupLayout,
        entries: [
            { binding: 0, resource: { buffer: materialUniformBuffer } },
            { binding: 1, resource: textureSampler },
            { binding: 2, resource: device.createTextureView(baseColorTex) },
            { binding: 3, resource: device.createTextureView(normalTex) },
            { binding: 4, resource: device.createTextureView(metallicRoughnessTex) },
            { binding: 5, resource: device.createTextureView(emissiveTex) },
            { binding: 6, resource: device.createTextureView(occlusionTex) },
        ],
    });

    const pbrPipelineLayout = device.createPipelineLayout({
        bindGroupLayouts: [sceneBindGroupLayout, modelBindGroupLayout, materialBindGroupLayout],
    });

    const pbrShaderModule = device.createShaderModule({ code: pbrShaderCode });
    const pbrPipeline = device.createRenderPipeline({
        layout: pbrPipelineLayout,
        vertex: {
            module: pbrShaderModule,
            entryPoint: 'vs_main',
            buffers: [{
                arrayStride: 32,
                attributes: [
                    { shaderLocation: 0, offset: 0, format: 'float32x3' },
                    { shaderLocation: 1, offset: 12, format: 'float32x3' },
                    { shaderLocation: 2, offset: 24, format: 'float32x2' },
                ],
            }],
        },
        fragment: { module: pbrShaderModule, entryPoint: 'fs_main', targets: [{ format }] },
        primitive: { topology: 'triangle-list', cullMode: 'back' },
        depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
    });

    // Material uniforms
    let textureFlags = 0;
    if (mat.baseColorTextureIndex >= 0) textureFlags |= 1;
    if (mat.normalTextureIndex >= 0) textureFlags |= 2;
    if (mat.metallicRoughnessTextureIndex >= 0) textureFlags |= 4;
    if (mat.emissiveTextureIndex >= 0) textureFlags |= 8;
    if (mat.occlusionTextureIndex >= 0) textureFlags |= 16;

    const materialData = new Float32Array([
        mat.baseColorFactor[0], mat.baseColorFactor[1], mat.baseColorFactor[2],
        mat.metallicFactor, mat.roughnessFactor, 1.0, 0, 0
    ]);
    const materialDataView = new DataView(materialData.buffer);
    materialDataView.setUint32(24, textureFlags, true);
    device.queue.writeBuffer(materialUniformBuffer, 0, materialData);

    const cameraFOV = 60 * Math.PI / 180;
    const environmentIntensity = 0.4;
    const cameraDistance = 2.5;

    function frame() {
        // Camera position based on touch rotation
        const cameraX = Math.sin(rotationY) * Math.cos(rotationX) * cameraDistance;
        const cameraY = Math.sin(rotationX) * cameraDistance;
        const cameraZ = Math.cos(rotationY) * Math.cos(rotationX) * cameraDistance;
        const cameraPos = [cameraX, cameraY, cameraZ];

        const viewMatrix = lookAt(cameraPos, [0, 0, 0], [0, 1, 0]);
        const aspect = canvas.width / canvas.height;
        const projMatrix = perspective(cameraFOV, aspect, 0.1, 1000);

        // Scene uniforms: viewMatrix(64) + projMatrix(64) + cameraPos(12) + envIntensity(4) = 144 bytes
        // Using 40 floats (160 bytes) for alignment
        const sceneData = new Float32Array(40);
        sceneData.set(viewMatrix, 0);
        sceneData.set(projMatrix, 16);
        sceneData[32] = cameraPos[0];
        sceneData[33] = cameraPos[1];
        sceneData[34] = cameraPos[2];
        sceneData[35] = environmentIntensity;
        device.queue.writeBuffer(sceneUniformBuffer, 0, sceneData);

        // Model matrix with base rotation
        const modelMatrix = multiply(rotateY(Math.PI), identity());
        const normalMatrix = identity();
        const modelData = new Float32Array(32);
        modelData.set(modelMatrix, 0);
        modelData.set(normalMatrix, 16);
        device.queue.writeBuffer(modelUniformBuffer, 0, modelData);

        // Render
        const commandEncoder = device.createCommandEncoder();
        const textureView = ctx.getCurrentTexture().createView();
        const depthView = device.createTextureView(depthTexture);

        const renderPass = commandEncoder.beginRenderPass({
            colorAttachments: [{
                view: textureView,
                clearValue: { r: 0.1, g: 0.1, b: 0.1, a: 1.0 },
                loadOp: 'clear',
                storeOp: 'store',
            }],
            depthStencilAttachment: {
                view: depthView,
                depthClearValue: 1.0,
                depthLoadOp: 'clear',
                depthStoreOp: 'store',
            },
        });

        // Draw skybox
        renderPass.setPipeline(skyboxPipeline);
        renderPass.setBindGroup(0, sceneBindGroup);
        renderPass.setVertexBuffer(0, skyboxVertexBuffer);
        renderPass.draw(36);

        // Draw model
        renderPass.setPipeline(pbrPipeline);
        renderPass.setBindGroup(0, sceneBindGroup);
        renderPass.setBindGroup(1, modelBindGroup);
        renderPass.setBindGroup(2, materialBindGroup);
        renderPass.setVertexBuffer(0, vertexBuffer);
        renderPass.setIndexBuffer(indexBuffer, 'uint32');
        renderPass.drawIndexed(primitive.indexCount);

        renderPass.end();
        device.queue.submit([commandEncoder.finish()]);

        requestAnimationFrame(frame);
    }

    console.log("Starting render loop - touch/drag to rotate");
    frame();
}

init().catch((e) => {
    console.log("Error: " + (e.message || e));
});
