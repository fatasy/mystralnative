#include "webgpu/image_decoder.h"

#include "stb_image.h"

#ifdef MYSTRAL_HAS_WEBP
#include <webp/decode.h>
#endif

#include <iostream>
#include <limits>

namespace mystral::webgpu::bridge {

void decodeImageData(const async::JobContext& job, DecodedImageData& image) {
    if (job.isCancelled()) return;
    if (image.encoded.empty() ||
        image.encoded.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        image.error = "Image data is empty or too large to decode";
        return;
    }

    const auto* input = image.encoded.data();
    const size_t inputSize = image.encoded.size();
    const bool isWebP = inputSize >= 12 &&
        input[0] == 'R' && input[1] == 'I' && input[2] == 'F' && input[3] == 'F' &&
        input[8] == 'W' && input[9] == 'E' && input[10] == 'B' && input[11] == 'P';

    unsigned char* decoded = nullptr;
    if (isWebP) {
#ifdef MYSTRAL_HAS_WEBP
        decoded = WebPDecodeRGBA(input, inputSize, &image.width, &image.height);
        if (!decoded) image.error = "Failed to decode WebP image";
#else
        image.error = "WebP image detected but libwebp support is not compiled in";
#endif
    } else {
        int channels = 0;
        decoded = stbi_load_from_memory(
            input,
            static_cast<int>(inputSize),
            &image.width,
            &image.height,
            &channels,
            4);
        if (!decoded) {
            const char* reason = stbi_failure_reason();
            image.error = std::string("Failed to decode image") +
                (reason ? std::string(": ") + reason : std::string());
        }
    }

    if (!decoded) return;
    if (image.width <= 0 || image.height <= 0 ||
        static_cast<size_t>(image.width) >
            (std::numeric_limits<size_t>::max)() / 4 / static_cast<size_t>(image.height)) {
        image.error = "Decoded image dimensions are invalid";
    } else if (!job.isCancelled()) {
        const size_t byteCount =
            static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4;
        image.rgba.assign(decoded, decoded + byteCount);
    }

    if (isWebP) {
#ifdef MYSTRAL_HAS_WEBP
        WebPFree(decoded);
#endif
    } else {
        stbi_image_free(decoded);
    }
}

void ImageDecoderBindings::install(
    js::Engine* engine,
    AsyncBridge* asyncBridge,
    bool verbose) {
    engine_ = engine;
    asyncBridge_ = asyncBridge;
    verbose_ = verbose;

    engine_->setGlobalProperty("__decodeImageDataAsync",
        engine_->newFunction("__decodeImageDataAsync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return asyncBridge_->rejectedPromise("Image decode requires an ArrayBuffer argument");
            }

            size_t inputSize = 0;
            void* inputData = engine_->getArrayBufferData(args[0], &inputSize);
            if (!inputData || inputSize == 0) {
                return asyncBridge_->rejectedPromise("Image decode received an invalid ArrayBuffer");
            }

            auto image = std::make_shared<DecodedImageData>();
            const auto* bytes = static_cast<const uint8_t*>(inputData);
            image->encoded.assign(bytes, bytes + inputSize);

            js::JSValueHandle promise;
            auto* pending = asyncBridge_->createPromise(promise);
            if (!pending) return engine_->newUndefined();

            auto handle = async::getJobSystem().submit(
                async::JobPriority::Streaming,
                generation_,
                [image](const async::JobContext& job) {
                    decodeImageData(job, *image);
                },
                [this, image, pending](async::JobStatus status) {
                    if (!pending->active || pending->session != asyncBridge_->session() || pending->engine != engine_) {
                        asyncBridge_->settle(pending, false, {});
                        return;
                    }
                    if (status == async::JobStatus::Cancelled) {
                        asyncBridge_->settle(pending, false, {}, "Image decode was cancelled");
                        return;
                    }
                    if (status == async::JobStatus::Failed && image->error.empty()) {
                        image->error = "Image decode job failed";
                    }
                    if (!image->error.empty() || image->rgba.empty()) {
                        asyncBridge_->settle(
                            pending,
                            false,
                            {},
                            image->error.empty() ? "Image decode produced no pixels" : image->error);
                        return;
                    }

                    auto result = engine_->newObject();
                    auto pixels = engine_->newArrayBuffer(image->rgba.data(), image->rgba.size());
                    engine_->setProperty(result, "width", engine_->newNumber(image->width));
                    engine_->setProperty(result, "height", engine_->newNumber(image->height));
                    engine_->setProperty(result, "_data", pixels);
                    engine_->setProperty(result, "_closed", engine_->newBoolean(false));
                    if (verbose_) {
                        std::cout << "[createImageBitmap] Decoded "
                                  << image->width << "x" << image->height
                                  << " image asynchronously" << std::endl;
                    }
                    asyncBridge_->settle(pending, true, result);
                });

            if (!handle) {
                asyncBridge_->settle(
                    pending,
                    false,
                    {},
                    "Image decode queue is full or shutting down");
            }
            return promise;
        })
    );

    // JavaScript polyfill for createImageBitmap
    const char* imageBitmapPolyfill = R"(
// ImageBitmap class (web-compatible)
class ImageBitmap {
    constructor(width, height, data) {
        this.width = width;
        this.height = height;
        this._data = data;  // Internal RGBA pixel data
        this._closed = false;
    }

    close() {
        this._closed = true;
        this._data = null;
    }
}

// createImageBitmap - Standard Web API
// Supports: Blob, ArrayBuffer, Response, or object with arrayBuffer() method
async function createImageBitmap(source, options) {
    let arrayBuffer;

    if (source instanceof ArrayBuffer) {
        arrayBuffer = source;
    } else if (ArrayBuffer.isView(source)) {
        arrayBuffer = source.buffer.slice(source.byteOffset, source.byteOffset + source.byteLength);
    } else if (source && typeof source.arrayBuffer === 'function') {
        // Blob or Response
        arrayBuffer = await source.arrayBuffer();
    } else if (source && source._data) {
        // Already an ImageBitmap-like object
        return source;
    } else {
        throw new Error('createImageBitmap: unsupported source type');
    }

    // Decode using native function
    const decoded = await __decodeImageDataAsync(arrayBuffer);

    if (!decoded) {
        throw new Error('createImageBitmap: failed to decode image');
    }

    // Create ImageBitmap
    const bitmap = new ImageBitmap(decoded.width, decoded.height, decoded._data);
    return bitmap;
}

globalThis.createImageBitmap = createImageBitmap;
globalThis.ImageBitmap = ImageBitmap;

// CanvasRenderingContext2D - Placeholder class for instanceof checks
// The actual implementation is in Canvas2D bindings, this is just for type checking
class CanvasRenderingContext2D {
    constructor() {
        // This constructor is never called directly - contexts are created via getContext('2d')
    }
}
globalThis.CanvasRenderingContext2D = CanvasRenderingContext2D;

// HTMLCanvasElement - Placeholder class for instanceof checks
class HTMLCanvasElement {
    constructor() {}
}
globalThis.HTMLCanvasElement = HTMLCanvasElement;

// OffscreenCanvas - For type checking
class OffscreenCanvas {
    constructor(width, height) {
        this.width = width || 300;
        this.height = height || 150;
        this._contextType = null;
        this._context = null;
    }

    getContext(type, options) {
        if (type === '2d') {
            // For basic 2D context needs
            if (!this._context) {
                this._context = { canvas: this };
            }
            return this._context;
        }
        return null;
    }
}
globalThis.OffscreenCanvas = OffscreenCanvas;
)";
    engine_->eval(imageBitmapPolyfill, "imageBitmap-polyfill.js");
}

void ImageDecoderBindings::cancelPending() {
    const uint64_t generation = generation_++;
    if (!async::getJobSystem().isRunning()) return;
    async::getJobSystem().cancelGeneration(generation);
    async::getJobSystem().waitIdle();
    async::getJobSystem().processCompletions();
}

} // namespace mystral::webgpu::bridge
