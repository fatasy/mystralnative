# MystralNative - Agent Instructions

Instructions for AI agents working on MystralNative.

## Critical: No Native GLTF/GLB Code

**DO NOT** use, modify, or reference the native C++ GLTF/GLB loading code:

| File | Status |
|------|--------|
| `src/gltf/gltf_loader.cpp` | DEPRECATED - compile error |
| `src/utils/cgltf_impl.cpp` | DEPRECATED - compile error |
| `include/mystral/gltf/gltf_loader.h` | DEPRECATED |
| `third_party/cgltf/` | DEPRECATED |

**Use the JavaScript loaders instead** - they run on V8 within MystralNative and provide the same functionality as the web engine.

## Quick Reference

### Loading 3D Models (Correct Way)

```javascript
// In your game.js / game.ts
import { loadGLBModel } from 'mystral';

const model = await loadGLBModel(device, './assets/model.glb');
scene.addChild(model);
```

### Running Examples

```bash
./build/mystral run examples/mystral-helmet.js
./build/mystral run examples/sponza.js
```

### Building

```bash
# V8 + Dawn (recommended)
cmake -B build
cmake --build build --parallel
```

## Known Issues

1. **Draco compression** - Not supported. Use uncompressed GLTF/GLB files.
2. **V8 Heap OOM** - Complex scenes may crash after ~1 minute. Increase heap size.
3. **Dawn shaders** - Keep the pinned Dawn version aligned across platforms.

See `tasks/mystralnativeprelaunch.md` for full issue list.

## Documentation

When creating new docs pages in `docs/docs/`:

1. Create your `.mdx` file in the appropriate directory
2. **Add it to the sidebar** in `docs/src/pages/DocPage.tsx` (in `sidebarItems`)

Without adding to the sidebar, users cannot navigate to your page!
