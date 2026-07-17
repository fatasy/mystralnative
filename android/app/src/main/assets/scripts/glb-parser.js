/**
 * Simple GLB Parser for the native runtime
 *
 * Parses GLB (binary GLTF) files and returns mesh data, materials, and images.
 * Works with basic GLB files - no Draco compression, no extensions.
 */

/**
 * Parse a GLB ArrayBuffer and return parsed data
 * @param {ArrayBuffer} buffer - The GLB file as ArrayBuffer
 * @returns {Object} Parsed GLTF data with meshes, materials, images, etc.
 */
function parseGLB(buffer) {
    const dataView = new DataView(buffer);
    let offset = 0;

    // GLB Header (12 bytes)
    const magic = dataView.getUint32(offset, true); offset += 4;
    const version = dataView.getUint32(offset, true); offset += 4;
    const length = dataView.getUint32(offset, true); offset += 4;

    if (magic !== 0x46546C67) { // 'glTF' in little-endian
        throw new Error('Invalid GLB magic number');
    }
    if (version !== 2) {
        throw new Error('Only GLTF 2.0 is supported, got version ' + version);
    }

    // Parse chunks
    let jsonChunk = null;
    let binChunk = null;

    while (offset < length) {
        const chunkLength = dataView.getUint32(offset, true); offset += 4;
        const chunkType = dataView.getUint32(offset, true); offset += 4;

        if (chunkType === 0x4E4F534A) { // 'JSON'
            const jsonBytes = new Uint8Array(buffer, offset, chunkLength);
            const jsonString = new TextDecoder('utf-8').decode(jsonBytes);
            jsonChunk = JSON.parse(jsonString);
        } else if (chunkType === 0x004E4942) { // 'BIN\0'
            binChunk = buffer.slice(offset, offset + chunkLength);
        }

        offset += chunkLength;
    }

    if (!jsonChunk) {
        throw new Error('No JSON chunk found in GLB');
    }

    return processGLTF(jsonChunk, binChunk);
}

/**
 * Process the GLTF JSON and extract usable data
 */
function processGLTF(gltf, binBuffer) {
    const result = {
        meshes: [],
        materials: [],
        textures: [],
        images: [],
        samplers: [],
        scenes: [],
        nodes: [],
    };

    // Helper to get accessor data
    function getAccessorData(accessorIndex, asUint32 = false) {
        const accessor = gltf.accessors[accessorIndex];
        const bufferView = gltf.bufferViews[accessor.bufferView];

        const byteOffset = (bufferView.byteOffset || 0) + (accessor.byteOffset || 0);
        const count = accessor.count;
        const componentType = accessor.componentType;
        const type = accessor.type;

        const componentCounts = {
            'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16
        };
        const components = componentCounts[type] || 1;
        const totalComponents = count * components;

        // Create appropriate typed array based on component type
        let data;
        if (componentType === 5126) { // FLOAT
            data = new Float32Array(binBuffer, byteOffset, totalComponents);
        } else if (componentType === 5123) { // UNSIGNED_SHORT
            data = new Uint16Array(binBuffer, byteOffset, totalComponents);
            if (asUint32) {
                // Convert to Uint32 for indices if needed
                const uint32Data = new Uint32Array(totalComponents);
                for (let i = 0; i < totalComponents; i++) uint32Data[i] = data[i];
                return uint32Data;
            }
        } else if (componentType === 5125) { // UNSIGNED_INT
            data = new Uint32Array(binBuffer, byteOffset, totalComponents);
        } else if (componentType === 5121) { // UNSIGNED_BYTE
            data = new Uint8Array(binBuffer, byteOffset, totalComponents);
            if (asUint32) {
                const uint32Data = new Uint32Array(totalComponents);
                for (let i = 0; i < totalComponents; i++) uint32Data[i] = data[i];
                return uint32Data;
            }
        } else {
            console.log('Unknown component type: ' + componentType);
            data = new Float32Array(binBuffer, byteOffset, totalComponents);
        }

        // Return a copy to avoid alignment issues
        if (data instanceof Float32Array) {
            return new Float32Array(data);
        } else if (data instanceof Uint32Array) {
            return new Uint32Array(data);
        } else if (data instanceof Uint16Array) {
            return new Uint16Array(data);
        }
        return data;
    }

    // Process materials
    if (gltf.materials) {
        for (let i = 0; i < gltf.materials.length; i++) {
            const mat = gltf.materials[i];
            const pbr = mat.pbrMetallicRoughness || {};

            result.materials.push({
                name: mat.name || 'Material_' + i,
                baseColorFactor: pbr.baseColorFactor || [1, 1, 1, 1],
                metallicFactor: pbr.metallicFactor !== undefined ? pbr.metallicFactor : 1,
                roughnessFactor: pbr.roughnessFactor !== undefined ? pbr.roughnessFactor : 1,
                baseColorTextureIndex: pbr.baseColorTexture?.index ?? -1,
                metallicRoughnessTextureIndex: pbr.metallicRoughnessTexture?.index ?? -1,
                normalTextureIndex: mat.normalTexture?.index ?? -1,
                emissiveTextureIndex: mat.emissiveTexture?.index ?? -1,
                occlusionTextureIndex: mat.occlusionTexture?.index ?? -1,
                emissiveFactor: mat.emissiveFactor || [0, 0, 0],
                alphaMode: mat.alphaMode || 'OPAQUE',
                alphaCutoff: mat.alphaCutoff || 0.5,
                doubleSided: mat.doubleSided || false,
            });
        }
    }

    // Process images
    if (gltf.images) {
        for (let i = 0; i < gltf.images.length; i++) {
            const img = gltf.images[i];
            let imageData = null;
            let mimeType = img.mimeType || 'image/png';

            if (img.bufferView !== undefined) {
                const bv = gltf.bufferViews[img.bufferView];
                const byteOffset = bv.byteOffset || 0;
                const byteLength = bv.byteLength;
                imageData = new Uint8Array(binBuffer, byteOffset, byteLength);
                // Make a copy
                imageData = new Uint8Array(imageData);
            }

            result.images.push({
                name: img.name || 'Image_' + i,
                uri: img.uri || '',
                mimeType: mimeType,
                data: imageData,
            });
        }
    }

    // Process samplers
    if (gltf.samplers) {
        for (let i = 0; i < gltf.samplers.length; i++) {
            const sampler = gltf.samplers[i];
            result.samplers.push({
                magFilter: sampler.magFilter || 9729, // LINEAR
                minFilter: sampler.minFilter || 9729, // LINEAR
                wrapS: sampler.wrapS || 10497, // REPEAT
                wrapT: sampler.wrapT || 10497, // REPEAT
            });
        }
    }

    // Process textures (maps texture index to image index)
    if (gltf.textures) {
        for (let i = 0; i < gltf.textures.length; i++) {
            const tex = gltf.textures[i];
            result.textures.push({
                name: tex.name || 'Texture_' + i,
                imageIndex: tex.source !== undefined ? tex.source : -1,
                samplerIndex: tex.sampler !== undefined ? tex.sampler : -1,
            });
        }
    }

    // Process meshes
    if (gltf.meshes) {
        for (let mi = 0; mi < gltf.meshes.length; mi++) {
            const mesh = gltf.meshes[mi];
            const meshData = {
                name: mesh.name || 'Mesh_' + mi,
                primitives: [],
            };

            for (let pi = 0; pi < mesh.primitives.length; pi++) {
                const prim = mesh.primitives[pi];
                const primData = {
                    materialIndex: prim.material !== undefined ? prim.material : -1,
                    positions: null,
                    normals: null,
                    texcoords: null,
                    indices: null,
                    vertexCount: 0,
                    indexCount: 0,
                };

                // Get attributes
                if (prim.attributes.POSITION !== undefined) {
                    primData.positions = getAccessorData(prim.attributes.POSITION);
                    primData.vertexCount = primData.positions.length / 3;
                }
                if (prim.attributes.NORMAL !== undefined) {
                    primData.normals = getAccessorData(prim.attributes.NORMAL);
                }
                if (prim.attributes.TEXCOORD_0 !== undefined) {
                    primData.texcoords = getAccessorData(prim.attributes.TEXCOORD_0);
                }

                // Get indices
                if (prim.indices !== undefined) {
                    primData.indices = getAccessorData(prim.indices, true);
                    primData.indexCount = primData.indices.length;
                }

                meshData.primitives.push(primData);
            }

            result.meshes.push(meshData);
        }
    }

    // Process nodes
    if (gltf.nodes) {
        for (let ni = 0; ni < gltf.nodes.length; ni++) {
            const node = gltf.nodes[ni];
            result.nodes.push({
                name: node.name || 'Node_' + ni,
                meshIndex: node.mesh !== undefined ? node.mesh : -1,
                children: node.children || [],
                translation: node.translation || [0, 0, 0],
                rotation: node.rotation || [0, 0, 0, 1],
                scale: node.scale || [1, 1, 1],
                matrix: node.matrix || null,
            });
        }
    }

    // Process scenes
    if (gltf.scenes) {
        for (let si = 0; si < gltf.scenes.length; si++) {
            const scene = gltf.scenes[si];
            result.scenes.push({
                name: scene.name || 'Scene_' + si,
                nodes: scene.nodes || [],
            });
        }
    }

    result.defaultScene = gltf.scene !== undefined ? gltf.scene : 0;

    return result;
}

/**
 * Load a GLB file from URL and parse it
 * @param {string} url - URL to the GLB file
 * @returns {Promise<Object>} Parsed GLTF data
 */
async function loadGLB(url) {
    console.log('loadGLB: Fetching ' + url);
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error('Failed to fetch GLB: ' + response.status);
    }
    const buffer = await response.arrayBuffer();
    console.log('loadGLB: Fetched ' + buffer.byteLength + ' bytes');
    return parseGLB(buffer);
}

// Export for use
globalThis.parseGLB = parseGLB;
globalThis.loadGLB = loadGLB;

console.log('GLB Parser loaded');
