#include "mystral/js/agent_module.h"

namespace mystral::js {

const char* getAgentModuleSource() {
    return R"JS(
const agent = globalThis.mystralAgent;
if (!agent) throw new Error('Mystral agent bridge is not initialized');

function required(value, name) {
  if (typeof value !== 'function') throw new TypeError(name + ' must be a function');
  return value;
}

function combined(disposers) {
  return () => { for (const dispose of disposers.splice(0)) dispose(); };
}

export function exposeScene(options = {}) {
  const prefix = options.id || 'scene';
  const disposers = [agent.exposeInspector(prefix + '.main', required(options.snapshot, 'scene.snapshot'), {
    label: options.label || 'Scene', kind: 'scene', description: options.description || 'Semantic scene snapshot'
  })];
  if (options.inspectNode) disposers.push(agent.exposeInspector(prefix + '.node', required(options.inspectNode, 'scene.inspectNode'), {
    label: 'Scene node', kind: 'scene-node', description: 'Inspect one scene node by stable ID'
  }));
  if (options.setVisible) disposers.push(agent.exposeAction(prefix + '.set-visible', required(options.setVisible, 'scene.setVisible'), {
    entityId: prefix + '.main', description: 'Set visibility for an exposed scene node',
    inputSchema: { type: 'object', required: ['id', 'visible'], properties: {
      id: { type: 'string' }, visible: { type: 'boolean' }
    } }
  }));
  return { dispose: combined(disposers), ids: { scene: prefix + '.main', node: prefix + '.node' } };
}

export function exposeCamera(options = {}) {
  const prefix = options.id || 'camera';
  const disposers = [agent.exposeInspector(prefix + '.active', required(options.snapshot, 'camera.snapshot'), {
    label: options.label || 'Active camera', kind: 'camera', description: options.description || 'Active camera state'
  })];
  for (const [suffix, handler, description] of [
    ['set-transform', options.setTransform, 'Set camera position or quaternion'],
    ['look-at', options.lookAt, 'Point the camera at a world position'],
    ['focus-entity', options.focusEntity, 'Point the camera at an exposed scene entity']
  ]) {
    if (!handler) continue;
    disposers.push(agent.exposeAction(prefix + '.' + suffix, required(handler, 'camera.' + suffix), {
      entityId: prefix + '.active', description, inputSchema: { type: 'object' }
    }));
  }
  return { dispose: combined(disposers), ids: { camera: prefix + '.active' } };
}

export function exposePerformance(options = {}) {
  const id = options.id || 'performance.live';
  const dispose = agent.exposeInspector(id, required(options.snapshot, 'performance.snapshot'), {
    label: options.label || 'Live performance', kind: 'performance',
    description: options.description || 'Runtime, renderer, and WebGPU metrics'
  });
  return { dispose, ids: { performance: id } };
}

function number(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function vector(value) {
  return value ? { x: number(value.x), y: number(value.y), z: number(value.z) } : null;
}

function quaternion(value) {
  return value ? { x: number(value.x), y: number(value.y), z: number(value.z), w: number(value.w) } : null;
}

function assignVector(target, value) {
  if (!target || !value) return;
  if (typeof target.set === 'function') target.set(value.x, value.y, value.z);
  else Object.assign(target, value);
}

function assignQuaternion(target, value) {
  if (!target || !value) return;
  if (typeof target.set === 'function') target.set(value.x, value.y, value.z, value.w);
  else Object.assign(target, value);
}

function rendererSnapshot(renderer) {
  const info = renderer && renderer.info;
  if (!info) return null;
  return {
    render: info.render ? {
      calls: number(info.render.calls), triangles: number(info.render.triangles),
      points: number(info.render.points), lines: number(info.render.lines)
    } : null,
    memory: info.memory ? {
      geometries: number(info.memory.geometries), textures: number(info.memory.textures)
    } : null,
    programs: Array.isArray(info.programs) ? info.programs.length : null
  };
}

export function exposeThree(options = {}) {
  const { scene, camera, renderer } = options;
  if (!scene) throw new TypeError('exposeThree requires scene');
  if (!camera) throw new TypeError('exposeThree requires camera');

  const sessionIds = new WeakMap();
  let nextSessionId = 1;
  const customId = typeof options.getId === 'function' ? options.getId : null;
  function identity(node) {
    const custom = customId && customId(node);
    if (typeof custom === 'string' && custom) return { id: custom, stability: 'authored' };
    if (custom && typeof custom.id === 'string') return custom;
    const authored = node && node.userData && node.userData.agentId;
    if (typeof authored === 'string' && authored) return { id: authored, stability: 'authored' };
    if (typeof node.uuid === 'string' && node.uuid) return { id: node.uuid, stability: 'session' };
    if (!sessionIds.has(node)) sessionIds.set(node, 'node-' + nextSessionId++);
    return { id: sessionIds.get(node), stability: 'session' };
  }
  const children = (node) => node && Array.isArray(node.children) ? node.children : [];

  function describe(node, parentId) {
    const key = identity(node);
    const geometry = node.geometry;
    const positions = geometry && geometry.attributes && geometry.attributes.position;
    const vertices = positions ? number(positions.count) : null;
    const indices = geometry && geometry.index ? number(geometry.index.count) : null;
    const materials = Array.isArray(node.material) ? node.material : node.material ? [node.material] : [];
    return {
      id: key.id, stability: key.stability || 'session', parentId: parentId || null,
      name: typeof node.name === 'string' ? node.name : '',
      kind: node.type || (node.constructor && node.constructor.name) || 'Object',
      visible: node.visible !== false, childCount: children(node).length,
      transform: { position: vector(node.position), quaternion: quaternion(node.quaternion), scale: vector(node.scale) },
      geometry: geometry ? {
        kind: geometry.type || null, vertices,
        triangles: indices !== null ? indices / 3 : vertices !== null ? vertices / 3 : null
      } : null,
      materials: materials.map((material) => ({
        name: material && typeof material.name === 'string' ? material.name : '',
        kind: material && material.type || null,
        visible: material ? material.visible !== false : null
      }))
    };
  }

  function findNode(id) {
    const stack = [scene];
    while (stack.length) {
      const node = stack.pop();
      if (identity(node).id === id) return node;
      const list = children(node);
      for (let index = list.length - 1; index >= 0; index--) stack.push(list[index]);
    }
    return null;
  }

  function sceneSnapshot(input = {}) {
    const limit = Math.max(1, Math.min(500, Number(input.limit) || 200));
    const offset = Math.max(0, Number(input.cursor) || 0);
    const maxDepth = Math.max(0, Math.min(8, input.depth === undefined ? 2 : Number(input.depth)));
    const root = input.rootId ? findNode(String(input.rootId)) : scene;
    if (!root) throw new Error('Unknown scene root: ' + input.rootId);
    const all = [];
    const stack = [{ node: root, parentId: null, depth: 0 }];
    while (stack.length) {
      const current = stack.pop();
      const isVisible = current.node.visible !== false;
      if (input.includeHidden || isVisible) all.push(describe(current.node, current.parentId));
      if (!input.includeHidden && !isVisible) continue;
      if (current.depth >= maxDepth) continue;
      const list = children(current.node);
      const parentId = identity(current.node).id;
      for (let index = list.length - 1; index >= 0; index--) {
        stack.push({ node: list[index], parentId, depth: current.depth + 1 });
      }
    }
    const nodes = all.slice(offset, offset + limit);
    return {
      schemaVersion: 1, rootId: identity(root).id, total: all.length, cursor: offset,
      nextCursor: offset + nodes.length < all.length ? offset + nodes.length : null, nodes
    };
  }

  function inspectNode(input) {
    const id = input && input.id;
    if (typeof id !== 'string' || !id) throw new TypeError('scene.node requires input.id');
    const node = findNode(id);
    if (!node) throw new Error('Unknown scene node: ' + id);
    return describe(node, node.parent ? identity(node.parent).id : null);
  }

  function cameraSnapshot() {
    return {
      schemaVersion: 1,
      kind: camera.type || (camera.isPerspectiveCamera ? 'PerspectiveCamera' : 'Camera'),
      position: vector(camera.position), quaternion: quaternion(camera.quaternion), up: vector(camera.up),
      fov: number(camera.fov), near: number(camera.near), far: number(camera.far),
      aspect: number(camera.aspect), zoom: number(camera.zoom)
    };
  }

  const sceneAdapter = exposeScene({
    snapshot: sceneSnapshot, inspectNode,
    setVisible: options.allowSceneMutations ? (input) => {
      const node = findNode(input && input.id);
      if (!node) throw new Error('Unknown scene node: ' + (input && input.id));
      node.visible = Boolean(input.visible);
      return { id: identity(node).id, visible: node.visible };
    } : null
  });
  const updateCamera = () => {
    if (typeof camera.updateProjectionMatrix === 'function') camera.updateProjectionMatrix();
    if (typeof camera.updateMatrixWorld === 'function') camera.updateMatrixWorld(true);
  };
  const cameraAdapter = exposeCamera({
    snapshot: cameraSnapshot,
    setTransform: options.allowCameraMutations ? (input = {}) => {
      assignVector(camera.position, input.position); assignQuaternion(camera.quaternion, input.quaternion);
      updateCamera(); return cameraSnapshot();
    } : null,
    lookAt: options.allowCameraMutations ? (input) => {
      const target = input && input.target;
      if (!target || typeof camera.lookAt !== 'function') throw new TypeError('camera.look-at requires target');
      camera.lookAt(target.x, target.y, target.z); updateCamera(); return cameraSnapshot();
    } : null,
    focusEntity: options.allowCameraMutations ? (input) => {
      const node = findNode(input && input.id);
      if (!node) throw new Error('Unknown scene node: ' + (input && input.id));
      let target = node.position;
      if (target && typeof target.clone === 'function') target = target.clone();
      if (target && typeof node.getWorldPosition === 'function') target = node.getWorldPosition(target);
      if (!target || typeof camera.lookAt !== 'function') throw new Error('Entity has no focusable position');
      camera.lookAt(target.x, target.y, target.z); updateCamera();
      return { entityId: identity(node).id, camera: cameraSnapshot() };
    } : null
  });
  const performanceAdapter = exposePerformance({ snapshot: () => ({
    schemaVersion: 1,
    runtime: typeof globalThis.__mystralRuntimeStats === 'function' ? globalThis.__mystralRuntimeStats() : null,
    webgpu: typeof globalThis.__mystralWebGpuStats === 'function' ? globalThis.__mystralWebGpuStats() : null,
    renderer: rendererSnapshot(renderer),
    game: typeof options.gameMetrics === 'function' ? options.gameMetrics() : null
  }) });
  return {
    ids: { ...sceneAdapter.ids, ...cameraAdapter.ids, ...performanceAdapter.ids },
    dispose: combined([sceneAdapter.dispose, cameraAdapter.dispose, performanceAdapter.dispose])
  };
}

export { agent as mystralAgent };
export default agent;
)JS";
}

}  // namespace mystral::js
