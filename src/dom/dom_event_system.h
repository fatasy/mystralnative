#pragma once

#include "mystral/js/engine.h"
#include "mystral/platform/input.h"

#include <map>
#include <string>
#include <vector>

namespace mystral::dom {

class DomEventSystem {
public:
    void install(js::Engine* engine, int width, int height, bool noSdl);
    void reset();
    void setSize(int width, int height);

private:
    struct EventListener {
        js::JSValueHandle callback;
        bool useCapture = false;
    };

    void clearDOMEventListeners();
    bool listenerUsesCapture(const std::vector<js::JSValueHandle>& args);
    void setupDOMEvents();
    void installEventMethods(js::JSValueHandle event, bool bubbles, bool cancelable);
    bool eventFlag(js::JSValueHandle event, const char* name);
    js::JSValueHandle eventTargetObject(const std::string& target);
    void dispatchToListeners(const std::string& target, const std::string& eventType,
                             js::JSValueHandle event, bool capture, int eventPhase);
    void dispatchToTextElement(js::JSValueHandle target, js::JSValueHandle event, bool capture);
    void finishEventDispatch(js::JSValueHandle event);
    void dispatchEventPath(const std::string& eventType, js::JSValueHandle event,
                           const std::string& target,
                           js::JSValueHandle explicitTarget = {});
    void dispatchKeyboardEvent(const platform::KeyboardEventData& event);
    void dispatchTextInputEvent(const platform::TextInputEventData& event);
    void dispatchCompositionEvent(const platform::CompositionEventData& event);
    void dispatchMouseEvent(const platform::MouseEventData& event);
    void dispatchPointerEvent(const platform::PointerEventData& event);
    void dispatchWheelEvent(const platform::WheelEventData& event);
    void dispatchGamepadEvent(const platform::GamepadEventData& event);
    void dispatchResizeEvent(const platform::ResizeEventData& event);
    void sendMockPointerEvent();

    js::Engine* engine_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool noSdl_ = false;
    std::map<std::string, std::map<std::string, std::vector<EventListener>>> eventListeners_;
    js::JSValueHandle canvasElement_;
    js::JSValueHandle activeTextElement_;
};

}  // namespace mystral::dom
