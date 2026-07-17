/**
 * Input System Implementation
 *
 * Translates SDL events to DOM-compatible event objects.
 */

#include "mystral/platform/input.h"
#include "mystral/platform/window.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace mystral {
namespace platform {

// Event callbacks
static KeyboardCallback g_keyboardCallback;
static TextInputCallback g_textInputCallback;
static CompositionCallback g_compositionCallback;
static MouseCallback g_mouseCallback;
static PointerCallback g_pointerCallback;
static WheelCallback g_wheelCallback;
static GamepadCallback g_gamepadCallback;
static ResizeCallback g_resizeCallback;

// Gamepad tracking
static std::unordered_map<SDL_JoystickID, SDL_Gamepad*> g_gamepads;
static std::vector<SDL_JoystickID> g_gamepadOrder;  // For index-based access

// Current modifier state
static bool g_ctrlKey = false;
static bool g_shiftKey = false;
static bool g_altKey = false;
static bool g_metaKey = false;
static bool g_compositionActive = false;
static bool g_compositionEndPending = false;

// Mouse button state
static int g_mouseButtons = 0;

/**
 * SDL key to DOM "key" property
 */
std::string sdlKeyToDOMKey(uint32_t sdlKey) {
    switch (sdlKey) {
        // Letters (lowercase by default)
        case SDLK_A: return "a";
        case SDLK_B: return "b";
        case SDLK_C: return "c";
        case SDLK_D: return "d";
        case SDLK_E: return "e";
        case SDLK_F: return "f";
        case SDLK_G: return "g";
        case SDLK_H: return "h";
        case SDLK_I: return "i";
        case SDLK_J: return "j";
        case SDLK_K: return "k";
        case SDLK_L: return "l";
        case SDLK_M: return "m";
        case SDLK_N: return "n";
        case SDLK_O: return "o";
        case SDLK_P: return "p";
        case SDLK_Q: return "q";
        case SDLK_R: return "r";
        case SDLK_S: return "s";
        case SDLK_T: return "t";
        case SDLK_U: return "u";
        case SDLK_V: return "v";
        case SDLK_W: return "w";
        case SDLK_X: return "x";
        case SDLK_Y: return "y";
        case SDLK_Z: return "z";

        // Numbers
        case SDLK_0: return "0";
        case SDLK_1: return "1";
        case SDLK_2: return "2";
        case SDLK_3: return "3";
        case SDLK_4: return "4";
        case SDLK_5: return "5";
        case SDLK_6: return "6";
        case SDLK_7: return "7";
        case SDLK_8: return "8";
        case SDLK_9: return "9";

        // Function keys
        case SDLK_F1: return "F1";
        case SDLK_F2: return "F2";
        case SDLK_F3: return "F3";
        case SDLK_F4: return "F4";
        case SDLK_F5: return "F5";
        case SDLK_F6: return "F6";
        case SDLK_F7: return "F7";
        case SDLK_F8: return "F8";
        case SDLK_F9: return "F9";
        case SDLK_F10: return "F10";
        case SDLK_F11: return "F11";
        case SDLK_F12: return "F12";

        // Navigation
        case SDLK_UP: return "ArrowUp";
        case SDLK_DOWN: return "ArrowDown";
        case SDLK_LEFT: return "ArrowLeft";
        case SDLK_RIGHT: return "ArrowRight";
        case SDLK_HOME: return "Home";
        case SDLK_END: return "End";
        case SDLK_PAGEUP: return "PageUp";
        case SDLK_PAGEDOWN: return "PageDown";

        // Editing
        case SDLK_BACKSPACE: return "Backspace";
        case SDLK_DELETE: return "Delete";
        case SDLK_INSERT: return "Insert";
        case SDLK_RETURN: return "Enter";
        case SDLK_TAB: return "Tab";
        case SDLK_ESCAPE: return "Escape";
        case SDLK_SPACE: return " ";

        // Modifiers
        case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
        case SDLK_LCTRL: case SDLK_RCTRL: return "Control";
        case SDLK_LALT: case SDLK_RALT: return "Alt";
        case SDLK_LGUI: case SDLK_RGUI: return "Meta";
        case SDLK_CAPSLOCK: return "CapsLock";

        // Punctuation
        case SDLK_MINUS: return "-";
        case SDLK_EQUALS: return "=";
        case SDLK_LEFTBRACKET: return "[";
        case SDLK_RIGHTBRACKET: return "]";
        case SDLK_BACKSLASH: return "\\";
        case SDLK_SEMICOLON: return ";";
        case SDLK_APOSTROPHE: return "'";
        case SDLK_GRAVE: return "`";
        case SDLK_COMMA: return ",";
        case SDLK_PERIOD: return ".";
        case SDLK_SLASH: return "/";

        default:
            // Return the SDL key name as fallback
            const char* name = SDL_GetKeyName(sdlKey);
            return name ? name : "Unidentified";
    }
}

/**
 * SDL scancode to DOM "code" property
 */
std::string sdlKeyToDOMCode(uint32_t sdlKey, uint32_t scancode) {
    switch (scancode) {
        // Letters
        case SDL_SCANCODE_A: return "KeyA";
        case SDL_SCANCODE_B: return "KeyB";
        case SDL_SCANCODE_C: return "KeyC";
        case SDL_SCANCODE_D: return "KeyD";
        case SDL_SCANCODE_E: return "KeyE";
        case SDL_SCANCODE_F: return "KeyF";
        case SDL_SCANCODE_G: return "KeyG";
        case SDL_SCANCODE_H: return "KeyH";
        case SDL_SCANCODE_I: return "KeyI";
        case SDL_SCANCODE_J: return "KeyJ";
        case SDL_SCANCODE_K: return "KeyK";
        case SDL_SCANCODE_L: return "KeyL";
        case SDL_SCANCODE_M: return "KeyM";
        case SDL_SCANCODE_N: return "KeyN";
        case SDL_SCANCODE_O: return "KeyO";
        case SDL_SCANCODE_P: return "KeyP";
        case SDL_SCANCODE_Q: return "KeyQ";
        case SDL_SCANCODE_R: return "KeyR";
        case SDL_SCANCODE_S: return "KeyS";
        case SDL_SCANCODE_T: return "KeyT";
        case SDL_SCANCODE_U: return "KeyU";
        case SDL_SCANCODE_V: return "KeyV";
        case SDL_SCANCODE_W: return "KeyW";
        case SDL_SCANCODE_X: return "KeyX";
        case SDL_SCANCODE_Y: return "KeyY";
        case SDL_SCANCODE_Z: return "KeyZ";

        // Numbers
        case SDL_SCANCODE_0: return "Digit0";
        case SDL_SCANCODE_1: return "Digit1";
        case SDL_SCANCODE_2: return "Digit2";
        case SDL_SCANCODE_3: return "Digit3";
        case SDL_SCANCODE_4: return "Digit4";
        case SDL_SCANCODE_5: return "Digit5";
        case SDL_SCANCODE_6: return "Digit6";
        case SDL_SCANCODE_7: return "Digit7";
        case SDL_SCANCODE_8: return "Digit8";
        case SDL_SCANCODE_9: return "Digit9";

        // Function keys
        case SDL_SCANCODE_F1: return "F1";
        case SDL_SCANCODE_F2: return "F2";
        case SDL_SCANCODE_F3: return "F3";
        case SDL_SCANCODE_F4: return "F4";
        case SDL_SCANCODE_F5: return "F5";
        case SDL_SCANCODE_F6: return "F6";
        case SDL_SCANCODE_F7: return "F7";
        case SDL_SCANCODE_F8: return "F8";
        case SDL_SCANCODE_F9: return "F9";
        case SDL_SCANCODE_F10: return "F10";
        case SDL_SCANCODE_F11: return "F11";
        case SDL_SCANCODE_F12: return "F12";

        // Navigation
        case SDL_SCANCODE_UP: return "ArrowUp";
        case SDL_SCANCODE_DOWN: return "ArrowDown";
        case SDL_SCANCODE_LEFT: return "ArrowLeft";
        case SDL_SCANCODE_RIGHT: return "ArrowRight";
        case SDL_SCANCODE_HOME: return "Home";
        case SDL_SCANCODE_END: return "End";
        case SDL_SCANCODE_PAGEUP: return "PageUp";
        case SDL_SCANCODE_PAGEDOWN: return "PageDown";

        // Editing
        case SDL_SCANCODE_BACKSPACE: return "Backspace";
        case SDL_SCANCODE_DELETE: return "Delete";
        case SDL_SCANCODE_INSERT: return "Insert";
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_TAB: return "Tab";
        case SDL_SCANCODE_ESCAPE: return "Escape";
        case SDL_SCANCODE_SPACE: return "Space";

        // Modifiers
        case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
        case SDL_SCANCODE_RSHIFT: return "ShiftRight";
        case SDL_SCANCODE_LCTRL: return "ControlLeft";
        case SDL_SCANCODE_RCTRL: return "ControlRight";
        case SDL_SCANCODE_LALT: return "AltLeft";
        case SDL_SCANCODE_RALT: return "AltRight";
        case SDL_SCANCODE_LGUI: return "MetaLeft";
        case SDL_SCANCODE_RGUI: return "MetaRight";
        case SDL_SCANCODE_CAPSLOCK: return "CapsLock";

        // Punctuation
        case SDL_SCANCODE_MINUS: return "Minus";
        case SDL_SCANCODE_EQUALS: return "Equal";
        case SDL_SCANCODE_LEFTBRACKET: return "BracketLeft";
        case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
        case SDL_SCANCODE_BACKSLASH: return "Backslash";
        case SDL_SCANCODE_SEMICOLON: return "Semicolon";
        case SDL_SCANCODE_APOSTROPHE: return "Quote";
        case SDL_SCANCODE_GRAVE: return "Backquote";
        case SDL_SCANCODE_COMMA: return "Comma";
        case SDL_SCANCODE_PERIOD: return "Period";
        case SDL_SCANCODE_SLASH: return "Slash";

        default:
            return "Unidentified";
    }
}

/**
 * Update modifier state from SDL event
 */
void updateModifiers(uint16_t sdlMod) {
    g_ctrlKey = (sdlMod & SDL_KMOD_CTRL) != 0;
    g_shiftKey = (sdlMod & SDL_KMOD_SHIFT) != 0;
    g_altKey = (sdlMod & SDL_KMOD_ALT) != 0;
    g_metaKey = (sdlMod & SDL_KMOD_GUI) != 0;
}

/**
 * Set callbacks
 */
void setKeyboardCallback(KeyboardCallback callback) {
    g_keyboardCallback = callback;
}

void setTextInputCallback(TextInputCallback callback) {
    g_textInputCallback = callback;
}

void setCompositionCallback(CompositionCallback callback) {
    g_compositionCallback = callback;
}

void setMouseCallback(MouseCallback callback) {
    g_mouseCallback = callback;
}

void setPointerCallback(PointerCallback callback) {
    g_pointerCallback = callback;
}

void setWheelCallback(WheelCallback callback) {
    g_wheelCallback = callback;
}

void setGamepadCallback(GamepadCallback callback) {
    g_gamepadCallback = callback;
}

void setResizeCallback(ResizeCallback callback) {
    g_resizeCallback = callback;
}

/**
 * Process keyboard event
 */
void processKeyboardEvent(const SDL_KeyboardEvent& event, bool isDown) {
    if (!g_keyboardCallback) return;

    updateModifiers(event.mod);

    KeyboardEventData data;
    data.type = isDown ? "keydown" : "keyup";
    data.key = sdlKeyToDOMKey(event.key);
    data.code = sdlKeyToDOMCode(event.key, event.scancode);
    data.keyCode = event.key;  // Legacy
    data.repeat = event.repeat;
    data.ctrlKey = g_ctrlKey;
    data.shiftKey = g_shiftKey;
    data.altKey = g_altKey;
    data.metaKey = g_metaKey;

    // If shift is held and it's a letter, uppercase it
    if (g_shiftKey && data.key.length() == 1 && data.key[0] >= 'a' && data.key[0] <= 'z') {
        data.key[0] = data.key[0] - 32;  // Convert to uppercase
    }

    g_keyboardCallback(data);
}

void processTextEditing(const SDL_TextEditingEvent& event) {
    if (!g_compositionCallback) return;

    const std::string text = event.text ? event.text : "";
    if (g_compositionEndPending && !text.empty()) {
        g_compositionCallback({"compositionend", "", 0, 0});
        g_compositionActive = false;
        g_compositionEndPending = false;
    }
    if (!g_compositionActive && !text.empty()) {
        g_compositionActive = true;
        g_compositionCallback({"compositionstart", "", event.start, event.length});
    }

    if (g_compositionActive) {
        if (text.empty()) {
            // SDL sends an empty edit immediately before committed text. Defer
            // compositionend until the rest of this SDL event batch is read.
            g_compositionEndPending = true;
        } else {
            g_compositionCallback({"compositionupdate", text, event.start, event.length});
        }
    }
}

void processTextInput(const SDL_TextInputEvent& event) {
    const std::string text = event.text ? event.text : "";
    const bool wasComposing = g_compositionActive;

    if (g_compositionActive && g_compositionCallback) {
        g_compositionCallback({"compositionend", text, 0, 0});
        g_compositionActive = false;
        g_compositionEndPending = false;
    }

    if (g_textInputCallback && !text.empty()) {
        g_textInputCallback({text, wasComposing});
    }
}

void flushTextInputEvents() {
    if (g_compositionActive && g_compositionEndPending && g_compositionCallback) {
        g_compositionCallback({"compositionend", "", 0, 0});
        g_compositionActive = false;
        g_compositionEndPending = false;
    }
}

bool startTextInput() {
    SDL_Window* window = getSDLWindow();
    return window && SDL_StartTextInput(window);
}

void stopTextInput() {
    SDL_Window* window = getSDLWindow();
    if (window) {
        SDL_StopTextInput(window);
    }
    if (g_compositionActive && g_compositionCallback) {
        g_compositionCallback({"compositionend", "", 0, 0});
    }
    g_compositionActive = false;
    g_compositionEndPending = false;
}

bool setTextInputArea(int x, int y, int width, int height, int cursor) {
    SDL_Window* window = getSDLWindow();
    if (!window) return false;
    const SDL_Rect rect{x, y, width, height};
    return SDL_SetTextInputArea(window, &rect, cursor);
}

std::string getClipboardText() {
    char* text = SDL_GetClipboardText();
    if (!text) return "";
    std::string result(text);
    SDL_free(text);
    return result;
}

bool setClipboardText(const std::string& text) {
    return SDL_SetClipboardText(text.c_str());
}

/**
 * Process mouse motion event
 */
void processMouseMotion(const SDL_MouseMotionEvent& event) {
    // Dispatch mouse event
    if (g_mouseCallback) {
        MouseEventData data;
        data.type = "mousemove";
        data.clientX = event.x;
        data.clientY = event.y;
        data.movementX = event.xrel;
        data.movementY = event.yrel;
        data.button = 0;
        data.buttons = g_mouseButtons;
        data.ctrlKey = g_ctrlKey;
        data.shiftKey = g_shiftKey;
        data.altKey = g_altKey;
        data.metaKey = g_metaKey;

        g_mouseCallback(data);
    }

    // Dispatch pointer event
    if (g_pointerCallback) {
        PointerEventData data;
        data.type = "pointermove";
        data.clientX = event.x;
        data.clientY = event.y;
        data.movementX = event.xrel;
        data.movementY = event.yrel;
        data.button = 0;
        data.buttons = g_mouseButtons;
        data.ctrlKey = g_ctrlKey;
        data.shiftKey = g_shiftKey;
        data.altKey = g_altKey;
        data.metaKey = g_metaKey;
        data.pointerId = 1;  // Mouse is always pointer ID 1
        data.pointerType = "mouse";
        data.isPrimary = true;
        data.width = 1;
        data.height = 1;
        data.pressure = g_mouseButtons ? 0.5 : 0;

        g_pointerCallback(data);
    }
}

/**
 * Process mouse button event
 */
void processMouseButton(const SDL_MouseButtonEvent& event, bool isDown) {
    // Map SDL button to DOM button
    int domButton = 0;
    int buttonBit = 1;
    switch (event.button) {
        case SDL_BUTTON_LEFT:   domButton = 0; buttonBit = 1; break;
        case SDL_BUTTON_MIDDLE: domButton = 1; buttonBit = 4; break;
        case SDL_BUTTON_RIGHT:  domButton = 2; buttonBit = 2; break;
        case SDL_BUTTON_X1:     domButton = 3; buttonBit = 8; break;
        case SDL_BUTTON_X2:     domButton = 4; buttonBit = 16; break;
    }

    // Update button state
    if (isDown) {
        g_mouseButtons |= buttonBit;
    } else {
        g_mouseButtons &= ~buttonBit;
    }

    // Dispatch mouse event
    if (g_mouseCallback) {
        MouseEventData data;
        data.type = isDown ? "mousedown" : "mouseup";
        data.clientX = event.x;
        data.clientY = event.y;
        data.movementX = 0;
        data.movementY = 0;
        data.button = domButton;
        data.buttons = g_mouseButtons;
        data.ctrlKey = g_ctrlKey;
        data.shiftKey = g_shiftKey;
        data.altKey = g_altKey;
        data.metaKey = g_metaKey;

        g_mouseCallback(data);

        // Also fire "click" on mouseup for left button
        if (!isDown && domButton == 0) {
            data.type = "click";
            g_mouseCallback(data);
        }
    }

    // Dispatch pointer event
    if (g_pointerCallback) {
        PointerEventData data;
        data.type = isDown ? "pointerdown" : "pointerup";
        data.clientX = event.x;
        data.clientY = event.y;
        data.movementX = 0;
        data.movementY = 0;
        data.button = domButton;
        data.buttons = g_mouseButtons;
        data.ctrlKey = g_ctrlKey;
        data.shiftKey = g_shiftKey;
        data.altKey = g_altKey;
        data.metaKey = g_metaKey;
        data.pointerId = 1;  // Mouse is always pointer ID 1
        data.pointerType = "mouse";
        data.isPrimary = true;
        data.width = 1;
        data.height = 1;
        data.pressure = isDown ? 0.5 : 0;

        g_pointerCallback(data);
    }
}

/**
 * Process mouse wheel event
 */
void processMouseWheel(const SDL_MouseWheelEvent& event) {
    if (!g_wheelCallback) return;

    // Get current mouse position
    float mx, my;
    SDL_GetMouseState(&mx, &my);

    WheelEventData data;
    data.type = "wheel";
    data.clientX = mx;
    data.clientY = my;
    // SDL3 uses float for wheel deltas, scale to typical browser values
    // Browser wheel events are typically in pixels (or lines)
    data.deltaX = event.x * -120.0;  // Negative because SDL and DOM have opposite conventions
    data.deltaY = event.y * -120.0;
    data.deltaZ = 0;
    data.deltaMode = 0;  // 0 = pixels
    data.ctrlKey = g_ctrlKey;
    data.shiftKey = g_shiftKey;
    data.altKey = g_altKey;
    data.metaKey = g_metaKey;

    g_wheelCallback(data);
}

/**
 * Process gamepad connected
 */
void processGamepadConnected(SDL_JoystickID id) {
    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (!gamepad) {
        std::cerr << "[Input] Failed to open gamepad: " << SDL_GetError() << std::endl;
        return;
    }

    g_gamepads[id] = gamepad;
    g_gamepadOrder.push_back(id);

    if (g_gamepadCallback) {
        GamepadEventData data;
        data.type = "gamepadconnected";

        // Find index
        int index = 0;
        for (size_t i = 0; i < g_gamepadOrder.size(); i++) {
            if (g_gamepadOrder[i] == id) {
                index = i;
                break;
            }
        }

        data.gamepad.index = index;
        data.gamepad.id = SDL_GetGamepadName(gamepad);
        data.gamepad.connected = true;
        data.gamepad.numAxes = 6;  // Standard gamepad has 6 axes
        data.gamepad.numButtons = 17;  // Standard gamepad has 17 buttons

        g_gamepadCallback(data);
    }

    std::cout << "[Input] Gamepad connected: " << SDL_GetGamepadName(gamepad) << std::endl;
}

/**
 * Process gamepad disconnected
 */
void processGamepadDisconnected(SDL_JoystickID id) {
    auto it = g_gamepads.find(id);
    if (it == g_gamepads.end()) return;

    // Find index before removing
    int index = 0;
    for (size_t i = 0; i < g_gamepadOrder.size(); i++) {
        if (g_gamepadOrder[i] == id) {
            index = i;
            break;
        }
    }

    if (g_gamepadCallback) {
        GamepadEventData data;
        data.type = "gamepaddisconnected";
        data.gamepad.index = index;
        data.gamepad.id = SDL_GetGamepadName(it->second);
        data.gamepad.connected = false;

        g_gamepadCallback(data);
    }

    std::cout << "[Input] Gamepad disconnected" << std::endl;

    SDL_CloseGamepad(it->second);
    g_gamepads.erase(it);

    // Remove from order list
    for (auto oit = g_gamepadOrder.begin(); oit != g_gamepadOrder.end(); ++oit) {
        if (*oit == id) {
            g_gamepadOrder.erase(oit);
            break;
        }
    }
}

/**
 * Process resize event
 */
void processResize(int width, int height) {
    if (!g_resizeCallback) return;

    ResizeEventData data;
    data.width = width;
    data.height = height;

    g_resizeCallback(data);
}

/**
 * Get gamepad state
 */
bool getGamepadState(int index, GamepadState* state) {
    if (index < 0 || index >= (int)g_gamepadOrder.size()) {
        return false;
    }

    SDL_JoystickID id = g_gamepadOrder[index];
    auto it = g_gamepads.find(id);
    if (it == g_gamepads.end()) {
        return false;
    }

    SDL_Gamepad* gamepad = it->second;

    state->index = index;
    state->id = SDL_GetGamepadName(gamepad);
    state->connected = true;
    state->numAxes = 6;
    state->numButtons = 17;

    // Standard gamepad mapping (matches W3C Gamepad API)
    // Axes: leftStickX, leftStickY, rightStickX, rightStickY, leftTrigger, rightTrigger
    state->axes[0] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
    state->axes[1] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0;
    state->axes[2] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0;
    state->axes[3] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0;
    state->axes[4] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0;
    state->axes[5] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0;

    // Buttons (W3C standard mapping)
    // 0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=LT, 7=RT, 8=Back, 9=Start,
    // 10=LS, 11=RS, 12=DUp, 13=DDown, 14=DLeft, 15=DRight, 16=Home
    SDL_GamepadButton buttonMap[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,       // 0 - A/Cross
        SDL_GAMEPAD_BUTTON_EAST,        // 1 - B/Circle
        SDL_GAMEPAD_BUTTON_WEST,        // 2 - X/Square
        SDL_GAMEPAD_BUTTON_NORTH,       // 3 - Y/Triangle
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  // 4 - LB
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, // 5 - RB
        SDL_GAMEPAD_BUTTON_INVALID,     // 6 - LT (handled via axis)
        SDL_GAMEPAD_BUTTON_INVALID,     // 7 - RT (handled via axis)
        SDL_GAMEPAD_BUTTON_BACK,        // 8 - Back/Select
        SDL_GAMEPAD_BUTTON_START,       // 9 - Start
        SDL_GAMEPAD_BUTTON_LEFT_STICK,  // 10 - LS Click
        SDL_GAMEPAD_BUTTON_RIGHT_STICK, // 11 - RS Click
        SDL_GAMEPAD_BUTTON_DPAD_UP,     // 12
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,   // 13
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,   // 14
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,  // 15
        SDL_GAMEPAD_BUTTON_GUIDE,       // 16 - Home/Guide
    };

    for (int i = 0; i < 17; i++) {
        if (i == 6) {
            // Left trigger as button
            state->buttons[i] = state->axes[4] > 0.5;
            state->buttonValues[i] = state->axes[4];
        } else if (i == 7) {
            // Right trigger as button
            state->buttons[i] = state->axes[5] > 0.5;
            state->buttonValues[i] = state->axes[5];
        } else if (buttonMap[i] != SDL_GAMEPAD_BUTTON_INVALID) {
            state->buttons[i] = SDL_GetGamepadButton(gamepad, buttonMap[i]);
            state->buttonValues[i] = state->buttons[i] ? 1.0 : 0.0;
        } else {
            state->buttons[i] = false;
            state->buttonValues[i] = 0.0;
        }
    }

    return true;
}

/**
 * Get gamepad count
 */
int getGamepadCount() {
    return (int)g_gamepads.size();
}

}  // namespace platform
}  // namespace mystral
