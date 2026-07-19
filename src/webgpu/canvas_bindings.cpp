#include "webgpu/canvas_bindings.h"

#include "mystral/canvas/canvas2d.h"
#include "webgpu/binding_internal.h"
#include "webgpu/format_conversions.h"
#include "mystral/webgpu_compat.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace mystral {
namespace canvas {
js::JSValueHandle createCanvas2DContext(js::Engine* engine, int width, int height);
}

namespace webgpu {

using bridge::formatToString;
using bridge::stringToFormat;

static WGPUTexture getCurrentSwapchainTexture() {
    if (!g_surface) {
        if (g_offscreenTexture) return g_offscreenTexture;
        std::cerr << "[WebGPU] No surface and no offscreen texture available" << std::endl;
        return nullptr;
    }

    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(g_surface, &surfaceTexture);
    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        std::cerr << "[WebGPU] Failed to get current texture" << std::endl;
        return nullptr;
    }
    return surfaceTexture.texture;
}

void installCanvasBindings(js::Engine* engine) {    // ========================================================================
    // Create a mock parent element for the canvas (needed by Debugger)
    // ========================================================================
    auto parentElement = engine->newObject();
    engine->setProperty(parentElement, "style", engine->newObject());
    engine->setProperty(parentElement, "appendChild",
        engine->newFunction("appendChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // No-op in native runtime
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );
    engine->setProperty(parentElement, "removeChild",
        engine->newFunction("removeChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );

    // ========================================================================
    // Get existing canvas from runtime.cpp's document.getElementById
    // The canvas was created by setupDOMEvents() with addEventListener, style, etc.
    // We just need to add WebGPU-specific methods (getContext) to it.
    // ========================================================================
    auto existingDocument = engine->getGlobalProperty("document");
    auto getElementByIdFunc = engine->getProperty(existingDocument, "getElementById");

    // Call document.getElementById('canvas') to get the existing canvas
    std::vector<js::JSValueHandle> args;
    args.push_back(engine->newString("canvas"));
    auto canvasObject = engine->call(getElementByIdFunc, existingDocument, args);

    if (engine->isNull(canvasObject) || engine->isUndefined(canvasObject)) {
        std::cerr << "[WebGPU] Warning: No existing canvas found, creating new one" << std::endl;
        canvasObject = engine->newObject();
        engine->setProperty(canvasObject, "width", engine->newNumber(g_canvasWidth));
        engine->setProperty(canvasObject, "height", engine->newNumber(g_canvasHeight));
        engine->setProperty(canvasObject, "clientWidth", engine->newNumber(g_canvasWidth));
        engine->setProperty(canvasObject, "clientHeight", engine->newNumber(g_canvasHeight));
    }

    // Update canvas dimensions (in case they differ)
    engine->setProperty(canvasObject, "width", engine->newNumber(g_canvasWidth));
    engine->setProperty(canvasObject, "height", engine->newNumber(g_canvasHeight));
    engine->setProperty(canvasObject, "clientWidth", engine->newNumber(g_canvasWidth));
    engine->setProperty(canvasObject, "clientHeight", engine->newNumber(g_canvasHeight));

    // canvas.parentElement - mock parent element (for Debugger compatibility)
    engine->setProperty(canvasObject, "parentElement", parentElement);

    // canvas.getContext('webgpu') -> GPUCanvasContext
    // This is the WebGPU-specific method we add to the existing canvas
    engine->setProperty(canvasObject, "getContext",
        engine->newFunction("getContext", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return g_engine->newNull();
            }

            std::string contextType = g_engine->toString(args[0]);

            // Handle Canvas 2D context
            if (contextType == "2d") {
                if (g_verboseLogging) std::cout << "[Canvas] Creating 2D context (" << g_canvasWidth << "x" << g_canvasHeight << ")" << std::endl;
                auto ctx2d = canvas::createCanvas2DContext(g_engine, g_canvasWidth, g_canvasHeight);

                // Set reference back to canvas
                auto canvas = g_engine->getGlobalProperty("canvas");
                g_engine->setProperty(ctx2d, "canvas", canvas);

                // Store the native context for Canvas 2D to WebGPU compositing
                g_canvasCompositor.setContext(
                    static_cast<canvas::Canvas2DContext*>(g_engine->getPrivateData(ctx2d)));
                if (g_verboseLogging) std::cout << "[Canvas] Main canvas using 2D context - will composite to WebGPU" << std::endl;

                return ctx2d;
            }

            if (contextType != "webgpu") {
                std::cerr << "[Canvas] Unknown context type: " << contextType << std::endl;
                return g_engine->newNull();
            }

            // Create GPUCanvasContext
            auto canvasContext = g_engine->newObject();

            // Store reference to our surface
            g_engine->setPrivateData(canvasContext, g_surface);

            // context.canvas - reference back to canvas
            auto canvas = g_engine->getGlobalProperty("canvas");
            g_engine->setProperty(canvasContext, "canvas", canvas);

            // context.configure({ device, format, alphaMode })
            g_engine->setProperty(canvasContext, "configure",
                g_engine->newFunction("configure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.empty()) {
                        g_engine->throwException("configure requires a descriptor");
                        return g_engine->newUndefined();
                    }

                    auto descriptor = args[0];

                    // Get format
                    std::string format = g_engine->toString(g_engine->getProperty(descriptor, "format"));
                    g_surfaceFormat = stringToFormat(format);
                    g_canvasCompositor.setSurfaceFormat(g_surfaceFormat);
                    // Note: alphaMode and device are stored but surface is already configured

                    g_contextConfigured = true;
                    if (g_verboseLogging) std::cout << "[Canvas] Context configured with format: " << format << std::endl;

                    return g_engine->newUndefined();
                })
            );

            // context.unconfigure()
            g_engine->setProperty(canvasContext, "unconfigure",
                g_engine->newFunction("unconfigure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    g_contextConfigured = false;
                    return g_engine->newUndefined();
                })
            );

            // context.getCurrentTexture() -> GPUTexture
            g_engine->setProperty(canvasContext, "getCurrentTexture",
                g_engine->newFunction("getCurrentTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    // Get current swapchain texture
                    WGPUTexture texture = getCurrentSwapchainTexture();
                    if (!texture) {
                        g_engine->throwException("Failed to get current texture");
                        return g_engine->newUndefined();
                    }

                    g_currentTexture = texture;
                    static int frameCount = 0;
                    if (frameCount++ < 3) {
                        if (g_verboseLogging) std::cout << "[Canvas] Got texture: " << texture << std::endl;
                    }

                    // Register in texture registry so createView can find it
                    uint64_t textureId = g_nextTextureId++;
                    const bool ownsSurfaceReference = g_surface != nullptr;
                    g_textureRegistry[textureId] = {
                        texture, g_surfaceFormat, g_canvasWidth, g_canvasHeight, 1, 1,
                        WGPUTextureDimension_2D, ownsSurfaceReference, false
                    };
                    g_currentTextureId = textureId;

                    // Create JS wrapper for texture
                    auto jsTexture = g_engine->newObject();
                    g_engine->setPrivateData(jsTexture, texture);

                    // texture.width / height / depthOrArrayLayers
                    g_engine->setProperty(jsTexture, "width", g_engine->newNumber(g_canvasWidth));
                    g_engine->setProperty(jsTexture, "height", g_engine->newNumber(g_canvasHeight));
                    g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(1));

                    // texture.format
                    g_engine->setProperty(jsTexture, "format", g_engine->newString(formatToString(g_surfaceFormat)));
                    g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

                    // texture.createView(descriptor?) -> GPUTextureView
                    // Capture textureId to look up the correct texture (not g_currentTexture which may change)
                    g_engine->setProperty(jsTexture, "createView",
                        g_engine->newFunction("createView", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // Look up texture from registry using captured textureId
                            auto it = g_textureRegistry.find(textureId);
                            if (it == g_textureRegistry.end()) {
                                std::cerr << "[WebGPU] Canvas createView: Texture " << textureId << " not found in registry" << std::endl;
                                g_engine->throwException("Texture not found in registry");
                                return g_engine->newUndefined();
                            }

                            WGPUTexture texture = it->second.texture;
                            if (!texture) {
                                g_engine->throwException("No current texture");
                                return g_engine->newUndefined();
                            }

                            // Create texture view
                            WGPUTextureViewDescriptor viewDesc = {};
                            viewDesc.format = it->second.format;
                            viewDesc.dimension = WGPUTextureViewDimension_2D;
                            viewDesc.baseMipLevel = 0;
                            viewDesc.mipLevelCount = 1;
                            viewDesc.baseArrayLayer = 0;
                            viewDesc.arrayLayerCount = 1;
                            viewDesc.aspect = WGPUTextureAspect_All;

                            WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);

                            g_currentTextureView = view;
                            g_currentViewSourceTexture = texture;  // Track which texture the view was created from

                            // Create JS wrapper
                            auto jsView = g_engine->newObject();
                            g_engine->setPrivateData(jsView, view);
                            g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));

                            return jsView;
                        })
                    );

                    // texture.destroy()
                    g_engine->setProperty(jsTexture, "destroy",
                        g_engine->newFunction("destroy", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // Swapchain textures are managed by the surface, but remove from registry
                            g_textureRegistry.erase(textureId);
                            return g_engine->newUndefined();
                        })
                    );

                    return jsTexture;
                })
            );

            if (g_verboseLogging) std::cout << "[Canvas] WebGPU context created" << std::endl;
            return canvasContext;
        })
    );

    // Set global canvas - this is the SAME object as document.getElementById('canvas')
    // so it now has both WebGPU getContext AND event listener support
    engine->setGlobalProperty("canvas", canvasObject);

    // ========================================================================
    // Add missing methods to the existing document (from runtime.cpp)
    // We DON'T create a new document - just augment the existing one
    // ========================================================================

    // Add querySelector to existing document (if not present)
    engine->setProperty(existingDocument, "querySelector",
        engine->newFunction("querySelector", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // Check if querying for canvas
            if (!args.empty()) {
                std::string selector = g_engine->toString(args[0]);
                if (selector == "canvas" || selector.find("canvas") != std::string::npos) {
                    return g_engine->getGlobalProperty("canvas");
                }
            }
            return g_engine->newNull();
        })
    );

    // Add createElement to existing document
    // NOTE: runtime.cpp sets up a createElement with canvas support (toDataURL) for @loaders.gl WebP detection
    // Preserve the runtime's text controls; this override adds Canvas 2D support.
    engine->setGlobalProperty("__mystralCreateElement",
        engine->getProperty(existingDocument, "createElement"));
    engine->setProperty(existingDocument, "createElement",
        engine->newFunction("createElement", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // Get tag name if provided
            std::string tagName = "";
            if (!args.empty()) {
                tagName = g_engine->toString(args[0]);
            }

            if (tagName == "input" || tagName == "INPUT" ||
                tagName == "textarea" || tagName == "TEXTAREA") {
                auto fallback = g_engine->getGlobalProperty("__mystralCreateElement");
                auto document = g_engine->getGlobalProperty("document");
                if (g_engine->isFunction(fallback)) {
                    return g_engine->call(fallback, document, args);
                }
            }

            auto element = g_engine->newObject();

            // Add basic DOM element properties
            g_engine->setProperty(element, "style", g_engine->newObject());
            g_engine->setProperty(element, "className", g_engine->newString(""));
            g_engine->setProperty(element, "innerHTML", g_engine->newString(""));
            g_engine->setProperty(element, "textContent", g_engine->newString(""));
            g_engine->setProperty(element, "tagName", g_engine->newString(tagName.c_str()));
            g_engine->setProperty(element, "appendChild",
                g_engine->newFunction("appendChild", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return a.empty() ? g_engine->newUndefined() : a[0];
                })
            );
            g_engine->setProperty(element, "removeChild",
                g_engine->newFunction("removeChild", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return a.empty() ? g_engine->newUndefined() : a[0];
                })
            );
            g_engine->setProperty(element, "remove",
                g_engine->newFunction("remove", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // No-op in native runtime - element is not attached to DOM
                    return g_engine->newUndefined();
                })
            );
            g_engine->setProperty(element, "addEventListener",
                g_engine->newFunction("addEventListener", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // No-op in native runtime
                    return g_engine->newUndefined();
                })
            );
            g_engine->setProperty(element, "removeEventListener",
                g_engine->newFunction("removeEventListener", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return g_engine->newUndefined();
                })
            );

            // Special handling for canvas elements - add Canvas 2D support
            if (tagName == "canvas" || tagName == "CANVAS") {
                // Create OffscreenCanvas struct to store state
                int canvasId = g_nextOffscreenCanvasId++;
                auto offscreenCanvas = std::make_unique<bridge::OffscreenCanvasState>();
                bridge::OffscreenCanvasState* canvasPtr = offscreenCanvas.get();
                g_offscreenCanvases[canvasId] = std::move(offscreenCanvas);

                // Store the canvas ID as private data for getContext lookup
                g_engine->setPrivateData(element, reinterpret_cast<void*>(static_cast<intptr_t>(canvasId)));

                // Also store as property for debugging
                g_engine->setProperty(element, "_offscreenCanvasId", g_engine->newNumber(canvasId));

                // Default canvas dimensions (stored in struct)
                g_engine->setProperty(element, "width", g_engine->newNumber(canvasPtr->width));
                g_engine->setProperty(element, "height", g_engine->newNumber(canvasPtr->height));

                // Store reference to element globally so getContext can find it
                std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                g_engine->setGlobalProperty(globalName.c_str(), element);

                // Create getContext function
                // Capture canvasId to ensure each canvas element's getContext uses its own canvas
                // This fixes the bug where all canvases shared the same context
                auto getContextFn = g_engine->newFunction("getContext", [canvasId, canvasPtr](void* c, const std::vector<js::JSValueHandle>& contextArgs) {
                    if (contextArgs.empty()) {
                        return g_engine->newNull();
                    }

                    std::string contextType = g_engine->toString(contextArgs[0]);

                    // Use the captured canvasId to find the correct canvas
                    // This ensures each canvas element's getContext returns its own context
                    auto it = g_offscreenCanvases.find(canvasId);
                    if (it == g_offscreenCanvases.end()) {
                        std::cerr << "[Canvas] Canvas not found: " << canvasId << std::endl;
                        return g_engine->newNull();
                    }

                    bridge::OffscreenCanvasState* canvas = it->second.get();

                    if (contextType == "2d") {
                        // Return cached context if already created
                        if (canvas->hasContext2d) {
                            return canvas->context2d;
                        }

                        // Get current dimensions from the canvas element (in case they were changed)
                        std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                        auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
                        if (!g_engine->isNull(canvasElement) && !g_engine->isUndefined(canvasElement)) {
                            auto widthProp = g_engine->getProperty(canvasElement, "width");
                            auto heightProp = g_engine->getProperty(canvasElement, "height");
                            if (!g_engine->isUndefined(widthProp)) {
                                canvas->width = static_cast<int>(g_engine->toNumber(widthProp));
                            }
                            if (!g_engine->isUndefined(heightProp)) {
                                canvas->height = static_cast<int>(g_engine->toNumber(heightProp));
                            }
                        }

                        // Create Canvas 2D context
                        if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D context (" << canvas->width << "x" << canvas->height << ")" << std::endl;
                        canvas->context2d = canvas::createCanvas2DContext(g_engine, canvas->width, canvas->height);
                        canvas->hasContext2d = true;
                        g_engine->protect(canvas->context2d);
                        return canvas->context2d;
                    }

                    if (contextType == "webgpu") {
                        // Create GPUCanvasContext for offscreen canvas
                        // This shares the main surface/device for simplicity
                        if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen WebGPU context" << std::endl;

                        auto canvasContext = g_engine->newObject();

                        // Store reference to our surface
                        g_engine->setPrivateData(canvasContext, g_surface);

                        // context.canvas - reference back to canvas element
                        std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                        auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
                        g_engine->setProperty(canvasContext, "canvas", canvasElement);

                        // context.configure({ device, format, alphaMode })
                        g_engine->setProperty(canvasContext, "configure",
                            g_engine->newFunction("configure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                if (args.empty()) {
                                    g_engine->throwException("configure requires a descriptor");
                                    return g_engine->newUndefined();
                                }
                                auto descriptor = args[0];
                                std::string format = g_engine->toString(g_engine->getProperty(descriptor, "format"));
                                g_surfaceFormat = stringToFormat(format);
                                g_canvasCompositor.setSurfaceFormat(g_surfaceFormat);
                                g_contextConfigured = true;
                                if (g_verboseLogging) std::cout << "[Canvas] Offscreen context configured with format: " << format << std::endl;
                                return g_engine->newUndefined();
                            })
                        );

                        // context.unconfigure()
                        g_engine->setProperty(canvasContext, "unconfigure",
                            g_engine->newFunction("unconfigure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                return g_engine->newUndefined();
                            })
                        );

                        // context.getCurrentTexture() -> GPUTexture
                        g_engine->setProperty(canvasContext, "getCurrentTexture",
                            g_engine->newFunction("getCurrentTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                WGPUTexture texture = getCurrentSwapchainTexture();
                                if (!texture) {
                                    g_engine->throwException("Failed to get current texture");
                                    return g_engine->newUndefined();
                                }
                                g_currentTexture = texture;
                                if (g_verboseLogging) std::cout << "[Canvas] Offscreen got texture: " << (void*)texture << std::endl;

                                // Register in texture registry so createView can find it
                                uint64_t textureId = g_nextTextureId++;
                                const bool ownsSurfaceReference = g_surface != nullptr;
                                g_textureRegistry[textureId] = {
                                    texture, g_surfaceFormat, g_canvasWidth, g_canvasHeight, 1, 1,
                                    WGPUTextureDimension_2D, ownsSurfaceReference, false
                                };
                                g_currentTextureId = textureId;

                                // Create JS wrapper for texture
                                auto jsTexture = g_engine->newObject();
                                g_engine->setPrivateData(jsTexture, texture);

                                // texture.width / height / depthOrArrayLayers
                                g_engine->setProperty(jsTexture, "width", g_engine->newNumber(g_canvasWidth));
                                g_engine->setProperty(jsTexture, "height", g_engine->newNumber(g_canvasHeight));
                                g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(1));

                                // texture.format
                                g_engine->setProperty(jsTexture, "format", g_engine->newString(formatToString(g_surfaceFormat)));
                                g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

                                // texture.createView(descriptor?) -> GPUTextureView
                                // Capture textureId to look up the correct texture (not g_currentTexture which may change)
                                g_engine->setProperty(jsTexture, "createView",
                                    g_engine->newFunction("createView", [textureId](void* c, const std::vector<js::JSValueHandle>& a) {
                                        // Look up texture from registry using captured textureId
                                        auto it = g_textureRegistry.find(textureId);
                                        if (it == g_textureRegistry.end()) {
                                            std::cerr << "[WebGPU] Offscreen createView: Texture " << textureId << " not found in registry" << std::endl;
                                            g_engine->throwException("Texture not found in registry");
                                            return g_engine->newUndefined();
                                        }

                                        WGPUTexture texture = it->second.texture;
                                        if (!texture) {
                                            g_engine->throwException("No current texture");
                                            return g_engine->newUndefined();
                                        }

                                        WGPUTextureViewDescriptor viewDesc = {};
                                        viewDesc.format = it->second.format;
                                        viewDesc.dimension = WGPUTextureViewDimension_2D;
                                        viewDesc.baseMipLevel = 0;
                                        viewDesc.mipLevelCount = 1;
                                        viewDesc.baseArrayLayer = 0;
                                        viewDesc.arrayLayerCount = 1;
                                        viewDesc.aspect = WGPUTextureAspect_All;

                                        WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
                                        g_currentTextureView = view;
                                        g_currentViewSourceTexture = texture;  // Track which texture the view was created from
                                        if (g_verboseLogging) std::cout << "[Canvas] Offscreen createView: texture=" << (void*)texture
                                                  << ", view=" << (void*)view << std::endl;

                                        auto jsView = g_engine->newObject();
                                        g_engine->setPrivateData(jsView, view);
                                        g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));

                                        return jsView;
                                    })
                                );

                                // texture.destroy()
                                g_engine->setProperty(jsTexture, "destroy",
                                    g_engine->newFunction("destroy", [textureId](void* c, const std::vector<js::JSValueHandle>& a) {
                                        // Swapchain textures are managed by the surface, but remove from registry
                                        g_textureRegistry.erase(textureId);
                                        return g_engine->newUndefined();
                                    })
                                );

                                return jsTexture;
                            })
                        );

                        return canvasContext;
                    }

                    // Ignore webgl requests silently (PixiJS feature detection)
                    if (contextType == "webgl" || contextType == "webgl2" || contextType == "experimental-webgl") {
                        return g_engine->newNull();
                    }

                    std::cerr << "[Canvas] Unsupported context type: " << contextType << std::endl;
                    return g_engine->newNull();
                });

                g_engine->setProperty(element, "getContext", getContextFn);
                if (g_verboseLogging) std::cout << "[Canvas] Created offscreen canvas " << canvasId << std::endl;

                // toDataURL for compatibility (returns empty data URI)
                g_engine->setProperty(element, "toDataURL",
                    g_engine->newFunction("toDataURL", [](void* c, const std::vector<js::JSValueHandle>& a) {
                        std::string mimeType = "image/png";
                        if (!a.empty()) {
                            mimeType = g_engine->toString(a[0]);
                        }
                        // Return a minimal data URI (for @loaders.gl WebP detection)
                        if (mimeType.find("webp") != std::string::npos) {
                            return g_engine->newString("data:image/webp;base64,");
                        }
                        return g_engine->newString("data:image/png;base64,");
                    })
                );

                // getBoundingClientRect - return canvas dimensions
                g_engine->setProperty(element, "getBoundingClientRect",
                    g_engine->newFunction("getBoundingClientRect", [](void* c, const std::vector<js::JSValueHandle>& a) {
                        // Get dimensions from the main canvas if available
                        auto rect = g_engine->newObject();
                        g_engine->setProperty(rect, "x", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "y", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "width", g_engine->newNumber(g_canvasWidth));
                        g_engine->setProperty(rect, "height", g_engine->newNumber(g_canvasHeight));
                        g_engine->setProperty(rect, "top", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "left", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "right", g_engine->newNumber(g_canvasWidth));
                        g_engine->setProperty(rect, "bottom", g_engine->newNumber(g_canvasHeight));
                        return rect;
                    })
                );
            }

            return element;
        })
    );

    // Add document.body if not present, or enhance existing body with required methods
    auto existingBody = engine->getProperty(existingDocument, "body");
    if (engine->isUndefined(existingBody) || engine->isNull(existingBody)) {
        existingBody = engine->newObject();
        engine->setProperty(existingDocument, "body", existingBody);
    }
    // Always add/update these methods on body
    engine->setProperty(existingBody, "style", engine->newObject());
    engine->setProperty(existingBody, "appendChild",
        engine->newFunction("appendChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );
    engine->setProperty(existingBody, "removeChild",
        engine->newFunction("removeChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );
}

void installOffscreenCanvasBindings(js::Engine* engine) {
    // ========================================================================
    // Native helper for offscreen canvas getContext('2d')
    // Called by the JS closure created in createElement('canvas')
    // ========================================================================
    engine->setGlobalProperty("__nativeGetContext2D",
        engine->newFunction("__nativeGetContext2D", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) {
                std::cerr << "[Canvas] __nativeGetContext2D requires contextType and canvasId" << std::endl;
                return g_engine->newNull();
            }

            std::string contextType = g_engine->toString(args[0]);
            int canvasId = static_cast<int>(g_engine->toNumber(args[1]));

            if (contextType != "2d") {
                std::cerr << "[Canvas] Unsupported context type for offscreen canvas: " << contextType << std::endl;
                return g_engine->newNull();
            }

            auto it = g_offscreenCanvases.find(canvasId);
            if (it == g_offscreenCanvases.end()) {
                std::cerr << "[Canvas] Canvas not found: " << canvasId << std::endl;
                return g_engine->newNull();
            }

            bridge::OffscreenCanvasState* canvas = it->second.get();

            // Return cached context if already created
            if (canvas->hasContext2d) {
                return canvas->context2d;
            }

            // Get current dimensions from the canvas element (in case they were changed)
            std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
            auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
            if (!g_engine->isNull(canvasElement) && !g_engine->isUndefined(canvasElement)) {
                auto widthProp = g_engine->getProperty(canvasElement, "width");
                auto heightProp = g_engine->getProperty(canvasElement, "height");
                if (!g_engine->isUndefined(widthProp)) {
                    canvas->width = static_cast<int>(g_engine->toNumber(widthProp));
                }
                if (!g_engine->isUndefined(heightProp)) {
                    canvas->height = static_cast<int>(g_engine->toNumber(heightProp));
                }
            }

            // Create Canvas 2D context with current dimensions
            if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D context (" << canvas->width << "x" << canvas->height << ")" << std::endl;
            canvas->context2d = canvas::createCanvas2DContext(g_engine, canvas->width, canvas->height);
            canvas->hasContext2d = true;
            g_engine->protect(canvas->context2d);
            return canvas->context2d;
        })
    );

    // ========================================================================
    // Global createOffscreenCanvas2D(width, height) helper
    // Creates an offscreen canvas with a 2D context at the specified size
    // This is easier to use than document.createElement('canvas').getContext('2d')
    // since it handles dimensions correctly
    // ========================================================================
    engine->setGlobalProperty("createOffscreenCanvas2D",
        engine->newFunction("createOffscreenCanvas2D", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            int width = 800;
            int height = 600;

            if (args.size() >= 1) {
                width = static_cast<int>(g_engine->toNumber(args[0]));
            }
            if (args.size() >= 2) {
                height = static_cast<int>(g_engine->toNumber(args[1]));
            }

            if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D canvas (" << width << "x" << height << ")" << std::endl;

            // Create a wrapper object that mimics a canvas with a 2D context
            auto canvasWrapper = g_engine->newObject();
            g_engine->setProperty(canvasWrapper, "width", g_engine->newNumber(width));
            g_engine->setProperty(canvasWrapper, "height", g_engine->newNumber(height));

            // Create the 2D context
            auto ctx2d = canvas::createCanvas2DContext(g_engine, width, height);
            g_engine->setProperty(canvasWrapper, "_context", ctx2d);

            // getContext('2d') returns the pre-created context
            g_engine->setProperty(canvasWrapper, "getContext",
                g_engine->newFunction("getContext", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // Get the stored context from the global (we need a way to access it)
                    // For now, return null and let callers use the _context directly
                    return g_engine->newNull();
                })
            );

            return canvasWrapper;
        })
    );
}

} // namespace webgpu
} // namespace mystral
