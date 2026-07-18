import { exposeThree } from 'mystral/agent';

function vec3(x, y, z) {
  return { x, y, z, set(nx, ny, nz) { this.x = nx; this.y = ny; this.z = nz; } };
}

function quat(x, y, z, w) {
  return { x, y, z, w, set(nx, ny, nz, nw) { this.x = nx; this.y = ny; this.z = nz; this.w = nw; } };
}

const sword = {
  uuid: 'session-sword', userData: { agentId: 'item.sword' }, name: 'Sword', type: 'Mesh', visible: true,
  position: vec3(0.5, 1, 0), quaternion: quat(0, 0, 0, 1), scale: vec3(1, 1, 1), children: [],
  geometry: { type: 'BufferGeometry', attributes: { position: { count: 36 } }, index: { count: 36 } },
  material: { name: 'Steel', type: 'MeshStandardMaterial', visible: true },
};
const player = {
  uuid: 'session-player', userData: { agentId: 'actor.player' }, name: 'Player', type: 'Group', visible: true,
  position: vec3(2, 0, 3), quaternion: quat(0, 0, 0, 1), scale: vec3(1, 1, 1), children: [sword],
};
const scene = {
  uuid: 'session-scene', userData: { agentId: 'world.root' }, name: 'World', type: 'Scene', visible: true,
  position: vec3(0, 0, 0), quaternion: quat(0, 0, 0, 1), scale: vec3(1, 1, 1), children: [player],
};
player.parent = scene;
sword.parent = player;

const camera = {
  type: 'PerspectiveCamera', position: vec3(0, 2, 5), quaternion: quat(0, 0, 0, 1), up: vec3(0, 1, 0),
  fov: 60, near: 0.1, far: 1000, aspect: 16 / 9, zoom: 1,
  lookAt(x, y, z) { this.lastTarget = { x, y, z }; },
  updateProjectionMatrix() { this.projectionUpdates = (this.projectionUpdates || 0) + 1; },
  updateMatrixWorld() { this.matrixUpdates = (this.matrixUpdates || 0) + 1; },
};
const renderer = {
  info: {
    render: { calls: 4, triangles: 12, points: 0, lines: 0 },
    memory: { geometries: 1, textures: 2 },
    programs: [{}, {}],
  },
};

exposeThree({
  scene,
  camera,
  renderer,
  allowCameraMutations: true,
  gameMetrics: () => ({ actors: 1 }),
});

function frame() {
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
console.log('Agent adapter fixture ready');
