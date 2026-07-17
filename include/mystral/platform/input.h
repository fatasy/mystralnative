/**
 * Input System
 *
 * Provides DOM-like input event handling for native runtime.
 * Translates SDL events to browser-compatible event objects.
 */

#pragma once

#include <functional>
#include <string>
#include <cstdint>

namespace mystral {
namespace platform {

/**
 * Keyboard event data (matches DOM KeyboardEvent)
 */
struct KeyboardEventData {
    std::string type;       // "keydown", "keyup"
    std::string key;        // "a", "Enter", "ArrowUp", etc.
    std::string code;       // "KeyA", "Enter", "ArrowUp", etc.
    uint32_t keyCode;       // Legacy keyCode
    bool repeat;            // Is this a repeat event?
    bool ctrlKey;
    bool shiftKey;
    bool altKey;
    bool metaKey;
};

/**
 * Committed UTF-8 text from the platform text input service.
 */
struct TextInputEventData {
    std::string text;
    bool fromComposition;
};

/**
 * IME composition event data.
 */
struct CompositionEventData {
    std::string type;       // "compositionstart", "compositionupdate", "compositionend"
    std::string data;
    int start;
    int length;
};

/**
 * Mouse event data (matches DOM MouseEvent)
 */
struct MouseEventData {
    std::string type;       // "mousedown", "mouseup", "mousemove", "click"
    double clientX;
    double clientY;
    double movementX;       // For mousemove
    double movementY;
    int button;             // 0=left, 1=middle, 2=right
    int buttons;            // Bitmask of currently pressed buttons
    bool ctrlKey;
    bool shiftKey;
    bool altKey;
    bool metaKey;
};

/**
 * Pointer event data (matches DOM PointerEvent, extends MouseEvent)
 */
struct PointerEventData {
    std::string type;       // "pointerdown", "pointerup", "pointermove", "pointercancel"
    double clientX;
    double clientY;
    double movementX;
    double movementY;
    int button;
    int buttons;
    bool ctrlKey;
    bool shiftKey;
    bool altKey;
    bool metaKey;
    // PointerEvent specific
    int pointerId;
    std::string pointerType;  // "mouse", "pen", "touch"
    bool isPrimary;
    double width;
    double height;
    double pressure;
};

/**
 * Wheel event data (matches DOM WheelEvent)
 */
struct WheelEventData {
    std::string type;       // "wheel"
    double clientX;
    double clientY;
    double deltaX;
    double deltaY;
    double deltaZ;
    int deltaMode;          // 0=pixels, 1=lines, 2=pages
    bool ctrlKey;
    bool shiftKey;
    bool altKey;
    bool metaKey;
};

/**
 * Gamepad button/axis state
 */
struct GamepadState {
    int index;
    std::string id;
    bool connected;
    double axes[16];        // Up to 16 axes
    bool buttons[32];       // Up to 32 buttons (pressed state)
    double buttonValues[32]; // Button analog values
    int numAxes;
    int numButtons;
};

/**
 * Gamepad event data
 */
struct GamepadEventData {
    std::string type;       // "gamepadconnected", "gamepaddisconnected"
    GamepadState gamepad;
};

/**
 * Resize event data
 */
struct ResizeEventData {
    int width;
    int height;
};

/**
 * Input event callback types
 */
using KeyboardCallback = std::function<void(const KeyboardEventData&)>;
using TextInputCallback = std::function<void(const TextInputEventData&)>;
using CompositionCallback = std::function<void(const CompositionEventData&)>;
using MouseCallback = std::function<void(const MouseEventData&)>;
using PointerCallback = std::function<void(const PointerEventData&)>;
using WheelCallback = std::function<void(const WheelEventData&)>;
using GamepadCallback = std::function<void(const GamepadEventData&)>;
using ResizeCallback = std::function<void(const ResizeEventData&)>;

/**
 * Set event callbacks
 */
void setKeyboardCallback(KeyboardCallback callback);
void setTextInputCallback(TextInputCallback callback);
void setCompositionCallback(CompositionCallback callback);
void setMouseCallback(MouseCallback callback);
void setPointerCallback(PointerCallback callback);
void setWheelCallback(WheelCallback callback);
void setGamepadCallback(GamepadCallback callback);
void setResizeCallback(ResizeCallback callback);

/**
 * Control the platform text input service and IME candidate window.
 */
bool startTextInput();
void stopTextInput();
bool setTextInputArea(int x, int y, int width, int height, int cursor);

/**
 * UTF-8 system clipboard access.
 */
std::string getClipboardText();
bool setClipboardText(const std::string& text);

/**
 * Get current gamepad state
 */
bool getGamepadState(int index, GamepadState* state);

/**
 * Get number of connected gamepads
 */
int getGamepadCount();

/**
 * Convert SDL keycode to DOM key string
 */
std::string sdlKeyToDOMKey(uint32_t sdlKey);

/**
 * Convert SDL keycode to DOM code string
 */
std::string sdlKeyToDOMCode(uint32_t sdlKey, uint32_t scancode);

}  // namespace platform
}  // namespace mystral
