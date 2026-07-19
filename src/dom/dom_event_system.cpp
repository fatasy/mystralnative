#include "dom/dom_event_system.h"

#include "js/runtime_sources.h"
#include "mystral/platform/window.h"

#include <iostream>

namespace mystral::dom {

void DomEventSystem::install(js::Engine* engine, int width, int height, bool noSdl) {
    reset();
    engine_ = engine;
    width_ = width;
    height_ = height;
    noSdl_ = noSdl;
    setupDOMEvents();
}

void DomEventSystem::reset() {
    clearDOMEventListeners();
    canvasElement_ = {};
    activeTextElement_ = {};
    engine_ = nullptr;
}

void DomEventSystem::setSize(int width, int height) {
    width_ = width;
    height_ = height;
}

void DomEventSystem::clearDOMEventListeners() {
        if (activeTextElement_.ptr) {
            if (engine_) {
                engine_->unprotect(activeTextElement_);
            }
            activeTextElement_ = {};
            if (!noSdl_) {
                platform::stopTextInput();
            }
        }
        if (!engine_) {
            eventListeners_.clear();
            return;
        }
        size_t count = 0;
        for (auto& [target, byType] : eventListeners_) {
            for (auto& [type, listeners] : byType) {
                for (auto& listener : listeners) {
                    if (listener.callback.ptr) {
                        engine_->unprotect(listener.callback);
                        count++;
                    }
                }
            }
        }
        eventListeners_.clear();
        if (count > 0) {
            std::cout << "[HotReload] Released " << count << " DOM listeners" << std::endl;
        }
    }

bool DomEventSystem::listenerUsesCapture(const std::vector<js::JSValueHandle>& args) {
        if (args.size() < 3 || engine_->isUndefined(args[2]) || engine_->isNull(args[2])) {
            return false;
        }
        if (engine_->isBoolean(args[2])) {
            return engine_->toBoolean(args[2]);
        }
        if (engine_->isObject(args[2])) {
            return engine_->toBoolean(engine_->getProperty(args[2], "capture"));
        }
        return false;
    }

void DomEventSystem::setupDOMEvents() {
        if (!engine_) return;

        // ========================================================================
        // Create canvas element FIRST (before document) so getElementById can return it
        // ========================================================================
        auto canvas = engine_->newObject();

        // Canvas properties
        engine_->setProperty(canvas, "id", engine_->newString("canvas"));
        engine_->setProperty(canvas, "tagName", engine_->newString("CANVAS"));
        engine_->setProperty(canvas, "width", engine_->newNumber(width_));
        engine_->setProperty(canvas, "height", engine_->newNumber(height_));
        engine_->setProperty(canvas, "clientWidth", engine_->newNumber(width_));
        engine_->setProperty(canvas, "clientHeight", engine_->newNumber(height_));

        // canvas.addEventListener - SAME PATTERN AS document and window
        engine_->setProperty(canvas, "addEventListener",
            engine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return engine_->newUndefined();

                std::string eventType = engine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = listenerUsesCapture(args);

                engine_->protect(callback);
                eventListeners_["canvas"][eventType].push_back({callback, useCapture});

                // std::cout << "[DOM] canvas.addEventListener('" << eventType << "')" << std::endl;

                return engine_->newUndefined();
            })
        );

        // canvas.removeEventListener
        engine_->setProperty(canvas, "removeEventListener",
            engine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return engine_->newUndefined();
            })
        );

        // canvas.getBoundingClientRect
        engine_->setProperty(canvas, "getBoundingClientRect",
            engine_->newFunction("getBoundingClientRect", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                auto rect = engine_->newObject();
                engine_->setProperty(rect, "x", engine_->newNumber(0));
                engine_->setProperty(rect, "y", engine_->newNumber(0));
                engine_->setProperty(rect, "width", engine_->newNumber(width_));
                engine_->setProperty(rect, "height", engine_->newNumber(height_));
                engine_->setProperty(rect, "top", engine_->newNumber(0));
                engine_->setProperty(rect, "left", engine_->newNumber(0));
                engine_->setProperty(rect, "right", engine_->newNumber(width_));
                engine_->setProperty(rect, "bottom", engine_->newNumber(height_));
                return rect;
            })
        );

        // canvas.style
        auto style = engine_->newObject();
        engine_->setProperty(style, "touchAction", engine_->newString(""));
        engine_->setProperty(style, "cursor", engine_->newString(""));
        engine_->setProperty(style, "width", engine_->newString(""));
        engine_->setProperty(style, "height", engine_->newString(""));
        engine_->protect(style);
        engine_->setProperty(canvas, "style", style);

        // canvas.setPointerCapture (stub)
        engine_->setProperty(canvas, "setPointerCapture",
            engine_->newFunction("setPointerCapture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        // canvas.releasePointerCapture (stub)
        engine_->setProperty(canvas, "releasePointerCapture",
            engine_->newFunction("releasePointerCapture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return js::JSValueHandle{nullptr, nullptr};
            })
        );

        // canvas.getContext (stub - WebGPU context is set up separately)
        engine_->setProperty(canvas, "getContext",
            engine_->newFunction("getContext", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return engine_->newNull();
            })
        );

        // canvas.toDataURL - Returns a data URL for the specified image type.
        // This is used by @loaders.gl to detect WebP support. We return proper
        // data URLs for formats we support (including WebP via libwebp).
        engine_->setProperty(canvas, "toDataURL",
            engine_->newFunction("toDataURL", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                std::string mimeType = "image/png";  // Default
                if (!args.empty()) {
                    mimeType = engine_->toString(args[0]);
                }

                // Return a minimal valid data URL for supported formats
                // This tells @loaders.gl that we support these formats
                if (mimeType == "image/png" || mimeType == "image/jpeg" || mimeType == "image/gif") {
                    // These are always supported via stb_image
                    return engine_->newString(("data:" + mimeType + ";base64,").c_str());
                }
#ifdef MYSTRAL_HAS_WEBP
                if (mimeType == "image/webp") {
                    // WebP is supported when libwebp is compiled in
                    return engine_->newString("data:image/webp;base64,");
                }
#endif
                // Unsupported format - return empty data URL
                return engine_->newString("data:,");
            })
        );

        // Cache and protect the canvas element
        canvasElement_ = canvas;
        engine_->protect(canvasElement_);

        std::cout << "[DOM] Canvas element created with addEventListener, style, etc." << std::endl;

        // ========================================================================
        // Create document object
        // ========================================================================
        auto document = engine_->newObject();

        // document.addEventListener
        engine_->setProperty(document, "addEventListener",
            engine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return engine_->newUndefined();

                std::string eventType = engine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = listenerUsesCapture(args);

                engine_->protect(callback);
                eventListeners_["document"][eventType].push_back({callback, useCapture});

                return engine_->newUndefined();
            })
        );

        // document.removeEventListener
        engine_->setProperty(document, "removeEventListener",
            engine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return engine_->newUndefined();

                std::string eventType = engine_->toString(args[0]);
                js::JSValueHandle callback = args[1];

                auto& listeners = eventListeners_["document"][eventType];
                for (auto it = listeners.begin(); it != listeners.end(); ++it) {
                    // Note: Comparing function handles is tricky. For now, we don't properly compare.
                    // A full implementation would need to track callback identity.
                }

                return engine_->newUndefined();
            })
        );

        // document.getElementById - returns our pre-created canvas element
        engine_->setProperty(document, "getElementById",
            engine_->newFunction("getElementById", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                // std::cout << "[DOM] getElementById called with " << args.size() << " args" << std::endl;

                if (args.empty()) return engine_->newNull();

                std::string id = engine_->toString(args[0]);
                // std::cout << "[DOM] getElementById('" << id << "')" << std::endl;

                // Return canvas element for "canvas" or any canvas-like id
                // Also handle '#canvas' prefix (common in jQuery-style code)
                if (id == "canvas" || id == "#canvas" || id == "engine-canvas" || id == "game-canvas") {
                    return canvasElement_;
                }

                return engine_->newNull();
            })
        );

        // Global helper for canvas.toDataURL.
        engine_->setGlobalProperty("__nativeCanvasToDataURL",
            engine_->newFunction("__nativeCanvasToDataURL", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                std::string mimeType = "image/png";
                if (!args.empty()) {
                    mimeType = engine_->toString(args[0]);
                }

                // Return proper data URLs for supported formats
                if (mimeType == "image/png" || mimeType == "image/jpeg" || mimeType == "image/gif") {
                    return engine_->newString(("data:" + mimeType + ";base64,").c_str());
                }
#ifdef MYSTRAL_HAS_WEBP
                if (mimeType == "image/webp") {
                    return engine_->newString("data:image/webp;base64,");
                }
#endif
                return engine_->newString("data:,");
            })
        );

        // document.body (for compatibility)
        auto body = engine_->newObject();
        engine_->setProperty(body, "tagName", engine_->newString("BODY"));
        engine_->setProperty(body, "appendChild", engine_->newFunction("appendChild", [this](void*, const std::vector<js::JSValueHandle>&) {
            return engine_->newUndefined();
        }));
        engine_->setProperty(body, "style", engine_->newObject());
        engine_->setProperty(document, "body", body);

        // document.head (for script loading)
        auto head = engine_->newObject();
        engine_->setProperty(head, "tagName", engine_->newString("HEAD"));
        engine_->setProperty(head, "appendChild", engine_->newFunction("appendChild", [this](void*, const std::vector<js::JSValueHandle>& args) {
            // For script loading, call onload callback asynchronously
            if (!args.empty()) {
                auto el = args[0];
                auto onload = engine_->getProperty(el, "onload");
                if (!engine_->isUndefined(onload) && !engine_->isNull(onload)) {
                    // Call onload via setTimeout to simulate async loading
                    engine_->eval("setTimeout(() => { arguments[0] && arguments[0](); }, 0);", "onload-trigger");
                }
            }
            return engine_->newUndefined();
        }));
        engine_->setProperty(document, "head", head);

        // document.location (for URL information)
        auto location = engine_->newObject();
        engine_->setProperty(location, "href", engine_->newString("file:///game.html"));
        engine_->setProperty(location, "protocol", engine_->newString("file:"));
        engine_->setProperty(location, "host", engine_->newString(""));
        engine_->setProperty(location, "hostname", engine_->newString(""));
        engine_->setProperty(location, "pathname", engine_->newString("/game.html"));
        engine_->setProperty(location, "origin", engine_->newString("file://"));
        engine_->setProperty(document, "location", location);

        engine_->setGlobalProperty("document", document);

        engine_->setGlobalProperty("__nativeFocusTextInput",
            engine_->newMethod("__nativeFocusTextInput", [this](void*, js::JSValueHandle receiver,
                                                                  const std::vector<js::JSValueHandle>&) {
                if (activeTextElement_.ptr) {
                    engine_->unprotect(activeTextElement_);
                }
                activeTextElement_ = receiver;
                engine_->protect(activeTextElement_);

                auto document = engine_->getGlobalProperty("document");
                engine_->setProperty(document, "activeElement", receiver);
                const bool started = noSdl_ ? false : platform::startTextInput();
                return engine_->newBoolean(started);
            }));

        engine_->setGlobalProperty("__nativeBlurTextInput",
            engine_->newMethod("__nativeBlurTextInput", [this](void*, js::JSValueHandle,
                                                                 const std::vector<js::JSValueHandle>&) {
                if (!noSdl_) {
                    platform::stopTextInput();
                }
                if (activeTextElement_.ptr) {
                    engine_->unprotect(activeTextElement_);
                    activeTextElement_ = {};
                }
                auto document = engine_->getGlobalProperty("document");
                engine_->setProperty(document, "activeElement", engine_->getProperty(document, "body"));
                return engine_->newUndefined();
            }));

        engine_->setGlobalProperty("__nativeSetTextInputArea",
            engine_->newFunction("__nativeSetTextInputArea", [this](void*, const std::vector<js::JSValueHandle>& args) {
                if (noSdl_ || args.size() < 4) return engine_->newBoolean(false);
                const int cursor = args.size() > 4 ? static_cast<int>(engine_->toNumber(args[4])) : 0;
                return engine_->newBoolean(platform::setTextInputArea(
                    static_cast<int>(engine_->toNumber(args[0])),
                    static_cast<int>(engine_->toNumber(args[1])),
                    static_cast<int>(engine_->toNumber(args[2])),
                    static_cast<int>(engine_->toNumber(args[3])), cursor));
            }));

        engine_->setGlobalProperty("__nativeClipboardReadText",
            engine_->newFunction("__nativeClipboardReadText", [this](void*, const std::vector<js::JSValueHandle>&) {
                if (noSdl_) return engine_->newString("");
                const std::string text = platform::getClipboardText();
                return engine_->newString(text.c_str());
            }));

        engine_->setGlobalProperty("__nativeClipboardWriteText",
            engine_->newFunction("__nativeClipboardWriteText", [this](void*, const std::vector<js::JSValueHandle>& args) {
                if (noSdl_ || args.empty()) return engine_->newBoolean(false);
                return engine_->newBoolean(platform::setClipboardText(engine_->toString(args[0])));
            }));

        // Set up document.createElement entirely in JavaScript for proper value handling
        // This must run AFTER document is set as a global
        const char* createElementSetup = js::runtime_sources::domCreateElement();
        engine_->eval(createElementSetup, "createElement-setup");

        // Create window object with event listeners
        // Note: We use the global object as window, and also set 'window' as a global property
        auto window = engine_->getGlobal();
        engine_->setGlobalProperty("window", window);

        // Set 'self' to point to global object (required by Three.js and other libs)
        // In browsers, 'self' refers to the global object (same as 'this' at global scope)
        engine_->setGlobalProperty("self", window);

        // Also set document as window.document (browsers have both)
        engine_->setProperty(window, "document", document);

        // window.addEventListener
        engine_->setProperty(window, "addEventListener",
            engine_->newFunction("addEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return engine_->newUndefined();

                std::string eventType = engine_->toString(args[0]);
                js::JSValueHandle callback = args[1];
                bool useCapture = listenerUsesCapture(args);

                engine_->protect(callback);
                eventListeners_["window"][eventType].push_back({callback, useCapture});

                return engine_->newUndefined();
            })
        );

        // window.removeEventListener
        engine_->setProperty(window, "removeEventListener",
            engine_->newFunction("removeEventListener", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return engine_->newUndefined();
            })
        );

        // window.innerWidth / window.innerHeight
        engine_->setProperty(window, "innerWidth", engine_->newNumber(width_));
        engine_->setProperty(window, "innerHeight", engine_->newNumber(height_));

        // window.devicePixelRatio
        engine_->setProperty(window, "devicePixelRatio", engine_->newNumber(1.0));

        // Set up input event callbacks
        platform::setKeyboardCallback([this](const platform::KeyboardEventData& e) {
            dispatchKeyboardEvent(e);
        });

        platform::setTextInputCallback([this](const platform::TextInputEventData& e) {
            dispatchTextInputEvent(e);
        });

        platform::setCompositionCallback([this](const platform::CompositionEventData& e) {
            dispatchCompositionEvent(e);
        });

        platform::setMouseCallback([this](const platform::MouseEventData& e) {
            dispatchMouseEvent(e);
        });

        platform::setPointerCallback([this](const platform::PointerEventData& e) {
            dispatchPointerEvent(e);
        });

        platform::setWheelCallback([this](const platform::WheelEventData& e) {
            dispatchWheelEvent(e);
        });

        platform::setGamepadCallback([this](const platform::GamepadEventData& e) {
            dispatchGamepadEvent(e);
        });

        platform::setResizeCallback([this](const platform::ResizeEventData& e) {
            dispatchResizeEvent(e);
        });

        // Set up navigator.getGamepads()
        auto navigator = engine_->getGlobalProperty("navigator");
        if (engine_->isUndefined(navigator)) {
            navigator = engine_->newObject();
            engine_->setGlobalProperty("navigator", navigator);
        }

        const char* clipboardSetup = js::runtime_sources::clipboard();
        engine_->eval(clipboardSetup, "clipboard-setup");

        engine_->setProperty(navigator, "getGamepads",
            engine_->newFunction("getGamepads", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                int count = platform::getGamepadCount();
                auto gamepads = engine_->newArray(4);  // Standard says 4 slots

                for (int i = 0; i < 4; i++) {
                    platform::GamepadState state;
                    if (platform::getGamepadState(i, &state)) {
                        auto gamepad = engine_->newObject();
                        engine_->setProperty(gamepad, "index", engine_->newNumber(state.index));
                        engine_->setProperty(gamepad, "id", engine_->newString(state.id.c_str()));
                        engine_->setProperty(gamepad, "connected", engine_->newBoolean(state.connected));

                        // Axes
                        auto axes = engine_->newArray(state.numAxes);
                        for (int a = 0; a < state.numAxes; a++) {
                            engine_->setPropertyIndex(axes, a, engine_->newNumber(state.axes[a]));
                        }
                        engine_->setProperty(gamepad, "axes", axes);

                        // Buttons
                        auto buttons = engine_->newArray(state.numButtons);
                        for (int b = 0; b < state.numButtons; b++) {
                            auto btn = engine_->newObject();
                            engine_->setProperty(btn, "pressed", engine_->newBoolean(state.buttons[b]));
                            engine_->setProperty(btn, "value", engine_->newNumber(state.buttonValues[b]));
                            engine_->setPropertyIndex(buttons, b, btn);
                        }
                        engine_->setProperty(gamepad, "buttons", buttons);

                        engine_->setPropertyIndex(gamepads, i, gamepad);
                    } else {
                        engine_->setPropertyIndex(gamepads, i, engine_->newNull());
                    }
                }

                return gamepads;
            })
        );

        // Pre-cache image format support for @loaders.gl
        // This must run before any user script that uses the GLTF loader
        const char* imageSupportInit = js::runtime_sources::imageSupport();
        engine_->eval(imageSupportInit, "image-support-init");

        std::cout << "[Mystral] DOM event system initialized" << std::endl;
    }

void DomEventSystem::installEventMethods(js::JSValueHandle event, bool bubbles, bool cancelable) {
        engine_->setProperty(event, "bubbles", engine_->newBoolean(bubbles));
        engine_->setProperty(event, "cancelable", engine_->newBoolean(cancelable));
        engine_->setProperty(event, "defaultPrevented", engine_->newBoolean(false));
        engine_->setProperty(event, "cancelBubble", engine_->newBoolean(false));
        engine_->setProperty(event, "eventPhase", engine_->newNumber(0));
        engine_->setProperty(event, "target", engine_->newNull());
        engine_->setProperty(event, "currentTarget", engine_->newNull());
        engine_->setProperty(event, "__immediatePropagationStopped", engine_->newBoolean(false));

        engine_->setProperty(event, "preventDefault",
            engine_->newMethod("preventDefault", [this](void*, js::JSValueHandle receiver,
                                                          const std::vector<js::JSValueHandle>&) {
                if (engine_->toBoolean(engine_->getProperty(receiver, "cancelable"))) {
                    engine_->setProperty(receiver, "defaultPrevented", engine_->newBoolean(true));
                }
                return engine_->newUndefined();
            }));
        engine_->setProperty(event, "stopPropagation",
            engine_->newMethod("stopPropagation", [this](void*, js::JSValueHandle receiver,
                                                            const std::vector<js::JSValueHandle>&) {
                engine_->setProperty(receiver, "cancelBubble", engine_->newBoolean(true));
                return engine_->newUndefined();
            }));
        engine_->setProperty(event, "stopImmediatePropagation",
            engine_->newMethod("stopImmediatePropagation", [this](void*, js::JSValueHandle receiver,
                                                                     const std::vector<js::JSValueHandle>&) {
                engine_->setProperty(receiver, "cancelBubble", engine_->newBoolean(true));
                engine_->setProperty(receiver, "__immediatePropagationStopped", engine_->newBoolean(true));
                return engine_->newUndefined();
            }));
    }

bool DomEventSystem::eventFlag(js::JSValueHandle event, const char* name) {
        return engine_->toBoolean(engine_->getProperty(event, name));
    }

js::JSValueHandle DomEventSystem::eventTargetObject(const std::string& target) {
        if (target == "window") return engine_->getGlobal();
        if (target == "document") return engine_->getGlobalProperty("document");
        return canvasElement_;
    }

void DomEventSystem::dispatchToListeners(const std::string& target, const std::string& eventType,
                             js::JSValueHandle event, bool capture, int eventPhase) {
        auto targetIt = eventListeners_.find(target);
        if (targetIt == eventListeners_.end()) return;

        auto typeIt = targetIt->second.find(eventType);
        if (typeIt == targetIt->second.end()) return;

        const auto currentTarget = eventTargetObject(target);
        engine_->setProperty(event, "currentTarget", currentTarget);
        engine_->setProperty(event, "eventPhase", engine_->newNumber(eventPhase));

        // Copy listeners in case a callback changes the registration list.
        const auto listeners = typeIt->second;
        for (const auto& listener : listeners) {
            if (listener.useCapture != capture) continue;
            engine_->call(listener.callback, currentTarget, {event});
            if (eventFlag(event, "__immediatePropagationStopped")) break;
        }
    }

void DomEventSystem::dispatchToTextElement(js::JSValueHandle target, js::JSValueHandle event, bool capture) {
        engine_->setProperty(event, "currentTarget", target);
        engine_->setProperty(event, "eventPhase", engine_->newNumber(2));
        auto dispatch = engine_->getProperty(target, "__dispatchListeners");
        if (engine_->isFunction(dispatch)) {
            engine_->call(dispatch, target, {event, engine_->newBoolean(capture)});
        }
    }

void DomEventSystem::finishEventDispatch(js::JSValueHandle event) {
        engine_->setProperty(event, "currentTarget", engine_->newNull());
        engine_->setProperty(event, "eventPhase", engine_->newNumber(0));
    }

void DomEventSystem::dispatchEventPath(const std::string& eventType, js::JSValueHandle event,
                           const std::string& targetName,
                           js::JSValueHandle textTarget) {
        const auto target = textTarget.ptr ? textTarget : eventTargetObject(targetName);
        engine_->setProperty(event, "target", target);

        if (targetName != "window") {
            dispatchToListeners("window", eventType, event, true, 1);
            if (eventFlag(event, "cancelBubble")) {
                finishEventDispatch(event);
                return;
            }
        }
        if (targetName != "window" && targetName != "document") {
            dispatchToListeners("document", eventType, event, true, 1);
            if (eventFlag(event, "cancelBubble")) {
                finishEventDispatch(event);
                return;
            }
        }

        if (textTarget.ptr) {
            dispatchToTextElement(textTarget, event, true);
            if (!eventFlag(event, "__immediatePropagationStopped")) {
                dispatchToTextElement(textTarget, event, false);
            }
        } else {
            dispatchToListeners(targetName, eventType, event, true, 2);
            if (!eventFlag(event, "__immediatePropagationStopped")) {
                dispatchToListeners(targetName, eventType, event, false, 2);
            }
        }

        if (!eventFlag(event, "bubbles") || eventFlag(event, "cancelBubble")) {
            finishEventDispatch(event);
            return;
        }

        if (targetName != "window" && targetName != "document") {
            dispatchToListeners("document", eventType, event, false, 3);
            if (eventFlag(event, "cancelBubble")) {
                finishEventDispatch(event);
                return;
            }
        }
        if (targetName != "window") {
            dispatchToListeners("window", eventType, event, false, 3);
        }
        finishEventDispatch(event);
    }

void DomEventSystem::dispatchKeyboardEvent(const platform::KeyboardEventData& e) {
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));
        engine_->setProperty(event, "key", engine_->newString(e.key.c_str()));
        engine_->setProperty(event, "code", engine_->newString(e.code.c_str()));
        engine_->setProperty(event, "keyCode", engine_->newNumber(e.keyCode));
        engine_->setProperty(event, "repeat", engine_->newBoolean(e.repeat));
        engine_->setProperty(event, "ctrlKey", engine_->newBoolean(e.ctrlKey));
        engine_->setProperty(event, "shiftKey", engine_->newBoolean(e.shiftKey));
        engine_->setProperty(event, "altKey", engine_->newBoolean(e.altKey));
        engine_->setProperty(event, "metaKey", engine_->newBoolean(e.metaKey));

        installEventMethods(event, true, true);
        if (activeTextElement_.ptr) {
            dispatchEventPath(e.type, event, "text", activeTextElement_);
        } else {
            dispatchEventPath(e.type, event, "canvas");
        }
    }

void DomEventSystem::dispatchTextInputEvent(const platform::TextInputEventData& e) {
        if (!activeTextElement_.ptr) return;
        const auto target = activeTextElement_;
        const char* inputType = e.fromComposition ? "insertCompositionText" : "insertText";

        auto beforeInput = engine_->newObject();
        engine_->setProperty(beforeInput, "type", engine_->newString("beforeinput"));
        engine_->setProperty(beforeInput, "data", engine_->newString(e.text.c_str()));
        engine_->setProperty(beforeInput, "inputType", engine_->newString(inputType));
        engine_->setProperty(beforeInput, "isComposing", engine_->newBoolean(false));
        installEventMethods(beforeInput, true, true);
        dispatchEventPath("beforeinput", beforeInput, "text", target);
        if (eventFlag(beforeInput, "defaultPrevented")) return;

        auto applyText = engine_->getProperty(target, "__applyTextInput");
        if (engine_->isFunction(applyText)) {
            engine_->call(applyText, target, {engine_->newString(e.text.c_str())});
        }

        auto input = engine_->newObject();
        engine_->setProperty(input, "type", engine_->newString("input"));
        engine_->setProperty(input, "data", engine_->newString(e.text.c_str()));
        engine_->setProperty(input, "inputType", engine_->newString(inputType));
        engine_->setProperty(input, "isComposing", engine_->newBoolean(false));
        installEventMethods(input, true, false);
        dispatchEventPath("input", input, "text", target);
    }

void DomEventSystem::dispatchCompositionEvent(const platform::CompositionEventData& e) {
        if (!activeTextElement_.ptr) return;
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));
        engine_->setProperty(event, "data", engine_->newString(e.data.c_str()));
        engine_->setProperty(event, "start", engine_->newNumber(e.start));
        engine_->setProperty(event, "length", engine_->newNumber(e.length));
        engine_->setProperty(event, "isComposing", engine_->newBoolean(e.type != "compositionend"));
        installEventMethods(event, true, false);
        dispatchEventPath(e.type, event, "text", activeTextElement_);
    }

void DomEventSystem::dispatchMouseEvent(const platform::MouseEventData& e) {
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));
        engine_->setProperty(event, "clientX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "clientY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "pageX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "pageY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "offsetX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "offsetY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "movementX", engine_->newNumber(e.movementX));
        engine_->setProperty(event, "movementY", engine_->newNumber(e.movementY));
        engine_->setProperty(event, "button", engine_->newNumber(e.button));
        engine_->setProperty(event, "buttons", engine_->newNumber(e.buttons));
        engine_->setProperty(event, "ctrlKey", engine_->newBoolean(e.ctrlKey));
        engine_->setProperty(event, "shiftKey", engine_->newBoolean(e.shiftKey));
        engine_->setProperty(event, "altKey", engine_->newBoolean(e.altKey));
        engine_->setProperty(event, "metaKey", engine_->newBoolean(e.metaKey));

        installEventMethods(event, true, true);
        dispatchEventPath(e.type, event, "canvas");
    }

void DomEventSystem::dispatchPointerEvent(const platform::PointerEventData& e) {
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));
        engine_->setProperty(event, "clientX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "clientY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "pageX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "pageY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "offsetX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "offsetY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "movementX", engine_->newNumber(e.movementX));
        engine_->setProperty(event, "movementY", engine_->newNumber(e.movementY));
        engine_->setProperty(event, "button", engine_->newNumber(e.button));
        engine_->setProperty(event, "buttons", engine_->newNumber(e.buttons));
        engine_->setProperty(event, "ctrlKey", engine_->newBoolean(e.ctrlKey));
        engine_->setProperty(event, "shiftKey", engine_->newBoolean(e.shiftKey));
        engine_->setProperty(event, "altKey", engine_->newBoolean(e.altKey));
        engine_->setProperty(event, "metaKey", engine_->newBoolean(e.metaKey));
        // PointerEvent specific properties
        engine_->setProperty(event, "pointerId", engine_->newNumber(e.pointerId));
        engine_->setProperty(event, "pointerType", engine_->newString(e.pointerType.c_str()));
        engine_->setProperty(event, "isPrimary", engine_->newBoolean(e.isPrimary));
        engine_->setProperty(event, "width", engine_->newNumber(e.width));
        engine_->setProperty(event, "height", engine_->newNumber(e.height));
        engine_->setProperty(event, "pressure", engine_->newNumber(e.pressure));

        installEventMethods(event, true, true);
        dispatchEventPath(e.type, event, "canvas");
    }

void DomEventSystem::dispatchWheelEvent(const platform::WheelEventData& e) {
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));
        engine_->setProperty(event, "clientX", engine_->newNumber(e.clientX));
        engine_->setProperty(event, "clientY", engine_->newNumber(e.clientY));
        engine_->setProperty(event, "deltaX", engine_->newNumber(e.deltaX));
        engine_->setProperty(event, "deltaY", engine_->newNumber(e.deltaY));
        engine_->setProperty(event, "deltaZ", engine_->newNumber(e.deltaZ));
        engine_->setProperty(event, "deltaMode", engine_->newNumber(e.deltaMode));
        engine_->setProperty(event, "ctrlKey", engine_->newBoolean(e.ctrlKey));
        engine_->setProperty(event, "shiftKey", engine_->newBoolean(e.shiftKey));
        engine_->setProperty(event, "altKey", engine_->newBoolean(e.altKey));
        engine_->setProperty(event, "metaKey", engine_->newBoolean(e.metaKey));

        installEventMethods(event, true, true);
        dispatchEventPath(e.type, event, "canvas");
    }

void DomEventSystem::dispatchGamepadEvent(const platform::GamepadEventData& e) {
        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString(e.type.c_str()));

        // Create gamepad object
        auto gamepad = engine_->newObject();
        engine_->setProperty(gamepad, "index", engine_->newNumber(e.gamepad.index));
        engine_->setProperty(gamepad, "id", engine_->newString(e.gamepad.id.c_str()));
        engine_->setProperty(gamepad, "connected", engine_->newBoolean(e.gamepad.connected));

        engine_->setProperty(event, "gamepad", gamepad);

        installEventMethods(event, false, false);
        dispatchEventPath(e.type, event, "window");
    }

void DomEventSystem::dispatchResizeEvent(const platform::ResizeEventData& e) {
        // Update internal dimensions
        width_ = e.width;
        height_ = e.height;

        // Update window.innerWidth/innerHeight
        auto window = engine_->getGlobal();
        engine_->setProperty(window, "innerWidth", engine_->newNumber(e.width));
        engine_->setProperty(window, "innerHeight", engine_->newNumber(e.height));

        auto event = engine_->newObject();
        engine_->setProperty(event, "type", engine_->newString("resize"));

        installEventMethods(event, false, false);
        dispatchEventPath("resize", event, "window");
    }

    // Test function to send a mock pointer event - call this after script evaluation
void DomEventSystem::sendMockPointerEvent() {
        // Debug output disabled to reduce log spam
        // std::cout << "[Input] Sending mock pointerdown event for testing..." << std::endl;
        // std::cout << "[Input] Registered event listeners:" << std::endl;
        // for (const auto& targetPair : eventListeners_) {
        //     std::cout << "  Target: " << targetPair.first << std::endl;
        //     for (const auto& typePair : targetPair.second) {
        //         std::cout << "    Event: " << typePair.first << " (" << typePair.second.size() << " listeners)" << std::endl;
        //     }
        // }

        platform::PointerEventData e;
        e.type = "pointerdown";
        e.clientX = 640;
        e.clientY = 360;
        e.movementX = 0;
        e.movementY = 0;
        e.button = 0;
        e.buttons = 1;
        e.ctrlKey = false;
        e.shiftKey = false;
        e.altKey = false;
        e.metaKey = false;
        e.pointerId = 1;
        e.pointerType = "mouse";
        e.isPrimary = true;
        e.width = 1;
        e.height = 1;
        e.pressure = 0.5;

        dispatchPointerEvent(e);
    }

}  // namespace mystral::dom
