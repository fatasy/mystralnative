#include "js/runtime_io_bindings.h"

#include "js/runtime_sources.h"
#include "mystral/async/job_system.h"
#include "mystral/fs/async_file.h"
#include "mystral/http/async_http_client.h"
#include "mystral/http/http_client.h"
#include "mystral/vfs/embedded_bundle.h"

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

#include <fstream>
#include <iostream>
#include <utility>

namespace mystral::js {

void RuntimeIOBindings::install(Engine* engine, const uint64_t* generation) {
    engine_ = engine;
    generation_ = generation;
    if (!engine_ || !generation_) return;

    // Native file reading function - uses SDL on Android for asset access
    engine_->setGlobalProperty("__readFileSync",
        engine_->newFunction("__readFileSync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return engine_->newNull();
            }

            std::string path = engine_->toString(args[0]);

            // Handle file:// prefix
            if (path.substr(0, 7) == "file://") {
                path = path.substr(7);
            }

            // Check embedded bundle first (if present)
            std::vector<uint8_t> embeddedData;
            if (vfs::readEmbeddedFile(path, embeddedData)) {
                std::cout << "[Fetch] Read " << embeddedData.size() << " bytes from bundle: " << path << std::endl;
                return engine_->newArrayBuffer(embeddedData.data(), embeddedData.size());
            }

#if defined(__ANDROID__)
            // On Android, use SDL_IOFromFile which can read from assets
            SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
            if (!io) {
                std::cerr << "[Fetch] Failed to open file (SDL): " << path << " - " << SDL_GetError() << std::endl;
                return engine_->newNull();
            }

            Sint64 size = SDL_GetIOSize(io);
            if (size < 0) {
                std::cerr << "[Fetch] Failed to get file size: " << path << std::endl;
                SDL_CloseIO(io);
                return engine_->newNull();
            }

            std::vector<uint8_t> buffer(static_cast<size_t>(size));
            size_t bytesRead = SDL_ReadIO(io, buffer.data(), static_cast<size_t>(size));
            SDL_CloseIO(io);

            if (bytesRead != static_cast<size_t>(size)) {
                std::cerr << "[Fetch] Failed to read file: " << path << std::endl;
                return engine_->newNull();
            }

            std::cout << "[Fetch] Read " << size << " bytes from (SDL): " << path << std::endl;
            return engine_->newArrayBuffer(buffer.data(), buffer.size());
#else
            // On other platforms, use std::ifstream
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "[Fetch] Failed to open file: " << path << std::endl;
                return engine_->newNull();
            }

            size_t size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> buffer(size);
            if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
                std::cerr << "[Fetch] Failed to read file: " << path << std::endl;
                return engine_->newNull();
            }

            std::cout << "[Fetch] Read " << size << " bytes from: " << path << std::endl;
            return engine_->newArrayBuffer(buffer.data(), buffer.size());
#endif
        })
    );

    // Async asset file reading through the native priority job system.
    // Takes (path, callback) where callback receives (data, error)
    engine_->setGlobalProperty("__readFileAsync",
        engine_->newFunction("__readFileAsync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) {
                std::cerr << "[Fetch Async] Missing arguments (need path, callback)" << std::endl;
                return engine_->newUndefined();
            }

            std::string path = engine_->toString(args[0]);

            // Handle file:// prefix
            if (path.substr(0, 7) == "file://") {
                path = path.substr(7);
            }

            // Get and protect the callback so it survives until we call it
            auto callback = args[1];
            engine_->protect(callback);

            const uint64_t generation = *generation_;

            // Check embedded bundle first (synchronously - it's fast)
            std::vector<uint8_t> embeddedData;
            if (vfs::readEmbeddedFile(path, embeddedData)) {
                std::cout << "[Fetch] Read " << embeddedData.size() << " bytes from bundle: " << path << std::endl;
                // Queue callback for next tick instead of calling immediately
                // This prevents stack overflow and matches browser async behavior
                pendingFileCallbacks_.push({
                    callback,
                    std::move(embeddedData),
                    "" // no error
                });
                return engine_->newUndefined();
            }

            // The callback is completed on the main thread when native jobs drain.
            fs::getAsyncFileReader().readFile(path, [this, callback, generation](std::vector<uint8_t> data, std::string error) {
                if (generation != *generation_) return;
                // Queue the callback with data for processing in the main loop
                pendingFileCallbacks_.push({
                    callback,
                    std::move(data),
                    std::move(error)
                });
            }, generation, async::JobPriority::Streaming);

            return engine_->newUndefined();
        })
    );

    // Native HTTP request function
    engine_->setGlobalProperty("__httpRequest",
        engine_->newFunction("__httpRequest", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return engine_->newNull();
            }

            std::string url = engine_->toString(args[0]);
            std::string method = "GET";
            std::vector<uint8_t> body;
            http::HttpOptions options;

            // Parse options object if provided
            if (args.size() > 1 && !engine_->isUndefined(args[1])) {
                auto optObj = args[1];

                auto methodVal = engine_->getProperty(optObj, "method");
                if (!engine_->isUndefined(methodVal)) {
                    method = engine_->toString(methodVal);
                }

                auto headersVal = engine_->getProperty(optObj, "headers");
                if (!engine_->isUndefined(headersVal)) {
                    // Get header keys - this is simplified, real impl would iterate
                    // For now, just handle common headers
                }

                auto bodyVal = engine_->getProperty(optObj, "body");
                if (!engine_->isUndefined(bodyVal)) {
                    if (engine_->isString(bodyVal)) {
                        std::string bodyStr = engine_->toString(bodyVal);
                        body.assign(bodyStr.begin(), bodyStr.end());
                    } else {
                        // Try to get as ArrayBuffer
                        size_t size = 0;
                        void* data = engine_->getArrayBufferData(bodyVal, &size);
                        if (data && size > 0) {
                            body.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                        }
                    }
                }
            }

            std::cout << "[HTTP] " << method << " " << url << std::endl;

            // Perform HTTP request
            auto& client = http::getHttpClient();
            http::HttpResponse response = client.request(method, url, body, options);

            // Create result object
            auto result = engine_->newObject();
            engine_->setProperty(result, "ok", engine_->newBoolean(response.ok));
            engine_->setProperty(result, "status", engine_->newNumber(response.status));
            engine_->setProperty(result, "url", engine_->newString(response.url.c_str()));

            if (!response.error.empty()) {
                engine_->setProperty(result, "error", engine_->newString(response.error.c_str()));
            }

            // Set response data as ArrayBuffer
            if (!response.data.empty()) {
                auto arrayBuffer = engine_->newArrayBuffer(response.data.data(), response.data.size());
                engine_->setProperty(result, "data", arrayBuffer);
            } else {
                engine_->setProperty(result, "data", engine_->newNull());
            }

            std::cout << "[HTTP] Response: " << response.status << " (" << response.data.size() << " bytes)" << std::endl;

            return result;
        })
    );

    // Async HTTP request function - uses libuv for non-blocking I/O
    // Takes (url, options, callback) where callback receives the result object
    engine_->setGlobalProperty("__httpRequestAsync",
        engine_->newFunction("__httpRequestAsync", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 3) {
                std::cerr << "[HTTP Async] Missing arguments (need url, options, callback)" << std::endl;
                return engine_->newUndefined();
            }

            std::string url = engine_->toString(args[0]);
            std::string method = "GET";
            std::vector<uint8_t> body;
            http::HttpOptions options;

            // Parse options object
            if (!engine_->isUndefined(args[1]) && !engine_->isNull(args[1])) {
                auto optObj = args[1];

                auto methodVal = engine_->getProperty(optObj, "method");
                if (!engine_->isUndefined(methodVal)) {
                    method = engine_->toString(methodVal);
                }

                auto bodyVal = engine_->getProperty(optObj, "body");
                if (!engine_->isUndefined(bodyVal)) {
                    if (engine_->isString(bodyVal)) {
                        std::string bodyStr = engine_->toString(bodyVal);
                        body.assign(bodyStr.begin(), bodyStr.end());
                    } else {
                        size_t size = 0;
                        void* data = engine_->getArrayBufferData(bodyVal, &size);
                        if (data && size > 0) {
                            body.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                        }
                    }
                }
            }

            // Get and protect the callback
            auto callback = args[2];
            engine_->protect(callback);

            const uint64_t generation = *generation_;

            // Start async request
            http::getAsyncHttpClient().request(method, url, body,
                [this, callback, generation](http::HttpResponse response) {
                    if (generation != *generation_) return;
                    auto* engine = engine_;
                    if (!engine) return;
                    // This runs on the main thread when the response arrives
                    // Create result object
                    auto result = engine->newObject();
                    engine->setProperty(result, "ok", engine->newBoolean(response.ok));
                    engine->setProperty(result, "status", engine->newNumber(response.status));
                    engine->setProperty(result, "url", engine->newString(response.url.c_str()));

                    if (!response.error.empty()) {
                        engine->setProperty(result, "error", engine->newString(response.error.c_str()));
                    }

                    if (!response.data.empty()) {
                        auto arrayBuffer = engine->newArrayBuffer(response.data.data(), response.data.size());
                        engine->setProperty(result, "data", arrayBuffer);
                    } else {
                        engine->setProperty(result, "data", engine->newNull());
                    }

                    // Call the JS callback with the result
                    std::vector<js::JSValueHandle> callbackArgs = { result };
                    engine->call(callback, engine->newUndefined(), callbackArgs);

                    // Unprotect the callback now that we're done
                    engine->unprotect(callback);
                },
                options
            );

            return engine_->newUndefined();
        })
    );

    // JavaScript fetch polyfill
    const char* fetchPolyfill = js::runtime_sources::fetchPolyfill();

    engine_->eval(fetchPolyfill, "fetch-polyfill.js");
    std::cout << "[Mystral] Fetch API initialized (file://, http://, https://)" << std::endl;

    // --- WHATWG Streams -------------------------------------------------
    // Real (spec-shaped) ReadableStream / WritableStream / TransformStream
    // plus TextEncoderStream / TextDecoderStream. These back the
    // WebTransport API (so its readable/writable support pipeTo/pipeThrough,
    // tee and async iteration) and are also available to user code. Each is
    // guarded by typeof-undefined so a native engine implementation wins.
    const char* streamsPolyfill = js::runtime_sources::streamsPolyfill();
    engine_->eval(streamsPolyfill, "streams-polyfill.js");
    std::cout << "[Mystral] Web Streams API initialized (ReadableStream/WritableStream/TransformStream)" << std::endl;
}



size_t RuntimeIOBindings::processFileCallbacks(size_t maxCount) {
    // Process pending file callbacks - these come from async file reads
    // We process them on the main thread to ensure JS context safety

    size_t processed = 0;
    while (!pendingFileCallbacks_.empty() && processed < maxCount) {
        auto pending = std::move(pendingFileCallbacks_.front());
        pendingFileCallbacks_.pop();

        if (pending.error.empty()) {
            // Success - create ArrayBuffer and call callback with (data, null)
            auto dataVal = engine_->newArrayBuffer(pending.data.data(), pending.data.size());
            auto errorVal = engine_->newNull();
            std::vector<js::JSValueHandle> callbackArgs = { dataVal, errorVal };
            engine_->call(pending.callback, engine_->newUndefined(), callbackArgs);
        } else {
            // Error - call callback with (null, error)
            auto nullVal = engine_->newNull();
            auto errorVal = engine_->newString(pending.error.c_str());
            std::vector<js::JSValueHandle> callbackArgs = { nullVal, errorVal };
            engine_->call(pending.callback, engine_->newUndefined(), callbackArgs);
        }

        // Unprotect the callback now that we're done with it
        engine_->unprotect(pending.callback);
        ++processed;
    }
    return processed;
}

void RuntimeIOBindings::clearFileCallbacks() {
    while (!pendingFileCallbacks_.empty()) {
        if (engine_) engine_->unprotect(pendingFileCallbacks_.front().callback);
        pendingFileCallbacks_.pop();
    }
}

} // namespace mystral::js
