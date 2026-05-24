/**
 * Three.js WebGPU GLTF + GLB Loading Example (Source)
 *
 * Loads a model in BOTH formats using Three.js' own GLTFLoader and renders
 * them side by side:
 *   - left:  DamagedHelmet.glb                 (binary glTF)
 *   - right: DamagedHelmet/glTF/DamagedHelmet.gltf (JSON glTF + .bin + JPG textures)
 *
 * This exercises the full Three.js asset pipeline on MystralNative:
 *   - FileLoader -> fetch() (which constructs an AbortController in r168+)
 *   - ImageBitmapLoader -> createImageBitmap() (JPEG decode via stb_image)
 *   - WebGPURenderer material/texture upload
 *
 * REQUIREMENTS:
 *   npm install three@0.182.0
 *
 * BUNDLING (required before running):
 *   npx esbuild examples/threejs-gltf-src.js --bundle --outfile=examples/threejs-gltf.js --format=esm --platform=browser
 *
 * RUN (from the mystralnative repo root, so the ./examples/assets paths resolve):
 *   mystral run examples/threejs-gltf.js
 *   mystral run examples/threejs-gltf.js --headless --screenshot out.png --frames 120
 *
 * Tested with: three@0.182.0
 */

import * as THREE from 'three/webgpu';
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js';

// MystralNative provides the canvas element globally.
// TypeScript users: declare const canvas: HTMLCanvasElement;

// Paths are relative to the directory mystral is run from (the repo root).
const GLB_PATH = './examples/assets/DamagedHelmet.glb';
const GLTF_PATH = './examples/assets/DamagedHelmet/glTF/DamagedHelmet.gltf';

function loadModel(loader, url) {
  return new Promise((resolve, reject) => {
    loader.load(
      url,
      (gltf) => resolve(gltf),
      undefined,
      (err) => reject(err),
    );
  });
}

async function main() {
  console.log('[Three.js GLTF] Starting WebGPU renderer...');

  const renderer = new THREE.WebGPURenderer({ canvas, antialias: false });
  await renderer.init();
  console.log('[Three.js GLTF] WebGPU initialized');

  const width = canvas.width || 1280;
  const height = canvas.height || 720;
  renderer.setSize(width, height, false);
  renderer.setPixelRatio(1);

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x1a1a2e);

  const camera = new THREE.PerspectiveCamera(50, width / height, 0.1, 100);
  camera.position.set(0, 0, 6);

  // Lighting: a hemisphere fill + a key directional light so PBR materials read well.
  scene.add(new THREE.HemisphereLight(0xffffff, 0x333344, 1.0));
  const keyLight = new THREE.DirectionalLight(0xffffff, 2.5);
  keyLight.position.set(5, 5, 5);
  scene.add(keyLight);

  const loader = new GLTFLoader();

  // Load both formats through Three.js. This is where r168+ touches
  // AbortController (FileLoader) and createImageBitmap (texture decode).
  let glbModel = null;
  let gltfModel = null;

  try {
    const glb = await loadModel(loader, GLB_PATH);
    glbModel = glb.scene;
    glbModel.position.x = -1.6;
    scene.add(glbModel);
    console.log('[Three.js GLTF] GLB loaded OK (' + GLB_PATH + ')');
  } catch (e) {
    console.error('[Three.js GLTF] GLB load FAILED:', e && e.message ? e.message : e);
  }

  try {
    const gltf = await loadModel(loader, GLTF_PATH);
    gltfModel = gltf.scene;
    gltfModel.position.x = 1.6;
    scene.add(gltfModel);
    console.log('[Three.js GLTF] GLTF loaded OK (' + GLTF_PATH + ')');
  } catch (e) {
    console.error('[Three.js GLTF] GLTF load FAILED:', e && e.message ? e.message : e);
  }

  if (!glbModel && !gltfModel) {
    throw new Error('Both GLB and GLTF failed to load');
  }

  console.log('[Three.js GLTF] Scene ready, starting render loop...');

  let frameCount = 0;
  function animate() {
    frameCount++;
    if (glbModel) glbModel.rotation.y += 0.01;
    if (gltfModel) gltfModel.rotation.y -= 0.01;
    renderer.render(scene, camera);
    if (frameCount % 60 === 0) console.log('[Three.js GLTF] Frame ' + frameCount);
    requestAnimationFrame(animate);
  }
  animate();
}

main().catch((e) => {
  console.error('[Three.js GLTF] Error:', e && e.message ? e.message : e);
  if (e && e.stack) console.error('[Three.js GLTF] Stack:', e.stack);
});
