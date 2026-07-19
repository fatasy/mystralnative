/**
 * Mystral CLI
 *
 * Command-line interface for running Mystral applications.
 *
 * Usage:
 *   mystral run <script.js>                    Run a JavaScript file
 *   mystral run <script.js> --screenshot out.png  Run, screenshot, quit
 *   mystral --version                          Show version information
 *   mystral --help                             Show help
 */

#include "mystral/runtime.h"
#include "mystral/vfs/embedded_bundle.h"
#include "mystral/js/module_resolver.h"
#include "mystral/js/ts_transpiler.h"
#include "mystral/debug/debug_server.h"
#include "mystral/video/async_capture.h"
#include "mystral/video/video_recorder.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <unordered_set>
#include <regex>
#include <queue>
#include <array>
#include <deque>
#include <iomanip>
#include <limits>

// WebP animation encoding (for video recording)
#ifdef MYSTRAL_HAS_WEBP_MUX
#include <webp/encode.h>
#include <webp/mux.h>
#endif

// stb_image_write for PNG encoding (implementation is in stb_impl.cpp)
typedef void (*stbi_write_func)(void *context, void *data, int size);
extern "C" int stbi_write_png_to_func(stbi_write_func func, void *context, int w, int h, int comp, const void *data, int stride_in_bytes);

// SDL3 for input injection
#include <SDL3/SDL.h>

// Base64 encoding for screenshot data
static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];

        result += alphabet[(n >> 18) & 0x3F];
        result += alphabet[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? alphabet[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? alphabet[n & 0x3F] : '=';
    }
    return result;
}

// PNG write callback for stbi_write_png_to_func
static void pngWriteCallback(void* context, void* data, int size) {
    auto* buffer = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

// ============================================================================
// SDL Input Injection for Debug Server
// ============================================================================

/**
 * Map key name to SDL scancode
 * Supports both Playwright-style names (Enter, Space) and DOM KeyboardEvent.code values
 */
static SDL_Scancode keyNameToScancode(const std::string& key) {
    // Letters
    if (key == "KeyA" || key == "a" || key == "A") return SDL_SCANCODE_A;
    if (key == "KeyB" || key == "b" || key == "B") return SDL_SCANCODE_B;
    if (key == "KeyC" || key == "c" || key == "C") return SDL_SCANCODE_C;
    if (key == "KeyD" || key == "d" || key == "D") return SDL_SCANCODE_D;
    if (key == "KeyE" || key == "e" || key == "E") return SDL_SCANCODE_E;
    if (key == "KeyF" || key == "f" || key == "F") return SDL_SCANCODE_F;
    if (key == "KeyG" || key == "g" || key == "G") return SDL_SCANCODE_G;
    if (key == "KeyH" || key == "h" || key == "H") return SDL_SCANCODE_H;
    if (key == "KeyI" || key == "i" || key == "I") return SDL_SCANCODE_I;
    if (key == "KeyJ" || key == "j" || key == "J") return SDL_SCANCODE_J;
    if (key == "KeyK" || key == "k" || key == "K") return SDL_SCANCODE_K;
    if (key == "KeyL" || key == "l" || key == "L") return SDL_SCANCODE_L;
    if (key == "KeyM" || key == "m" || key == "M") return SDL_SCANCODE_M;
    if (key == "KeyN" || key == "n" || key == "N") return SDL_SCANCODE_N;
    if (key == "KeyO" || key == "o" || key == "O") return SDL_SCANCODE_O;
    if (key == "KeyP" || key == "p" || key == "P") return SDL_SCANCODE_P;
    if (key == "KeyQ" || key == "q" || key == "Q") return SDL_SCANCODE_Q;
    if (key == "KeyR" || key == "r" || key == "R") return SDL_SCANCODE_R;
    if (key == "KeyS" || key == "s" || key == "S") return SDL_SCANCODE_S;
    if (key == "KeyT" || key == "t" || key == "T") return SDL_SCANCODE_T;
    if (key == "KeyU" || key == "u" || key == "U") return SDL_SCANCODE_U;
    if (key == "KeyV" || key == "v" || key == "V") return SDL_SCANCODE_V;
    if (key == "KeyW" || key == "w" || key == "W") return SDL_SCANCODE_W;
    if (key == "KeyX" || key == "x" || key == "X") return SDL_SCANCODE_X;
    if (key == "KeyY" || key == "y" || key == "Y") return SDL_SCANCODE_Y;
    if (key == "KeyZ" || key == "z" || key == "Z") return SDL_SCANCODE_Z;

    // Numbers
    if (key == "Digit0" || key == "0") return SDL_SCANCODE_0;
    if (key == "Digit1" || key == "1") return SDL_SCANCODE_1;
    if (key == "Digit2" || key == "2") return SDL_SCANCODE_2;
    if (key == "Digit3" || key == "3") return SDL_SCANCODE_3;
    if (key == "Digit4" || key == "4") return SDL_SCANCODE_4;
    if (key == "Digit5" || key == "5") return SDL_SCANCODE_5;
    if (key == "Digit6" || key == "6") return SDL_SCANCODE_6;
    if (key == "Digit7" || key == "7") return SDL_SCANCODE_7;
    if (key == "Digit8" || key == "8") return SDL_SCANCODE_8;
    if (key == "Digit9" || key == "9") return SDL_SCANCODE_9;

    // Function keys
    if (key == "F1") return SDL_SCANCODE_F1;
    if (key == "F2") return SDL_SCANCODE_F2;
    if (key == "F3") return SDL_SCANCODE_F3;
    if (key == "F4") return SDL_SCANCODE_F4;
    if (key == "F5") return SDL_SCANCODE_F5;
    if (key == "F6") return SDL_SCANCODE_F6;
    if (key == "F7") return SDL_SCANCODE_F7;
    if (key == "F8") return SDL_SCANCODE_F8;
    if (key == "F9") return SDL_SCANCODE_F9;
    if (key == "F10") return SDL_SCANCODE_F10;
    if (key == "F11") return SDL_SCANCODE_F11;
    if (key == "F12") return SDL_SCANCODE_F12;

    // Navigation
    if (key == "ArrowUp" || key == "Up") return SDL_SCANCODE_UP;
    if (key == "ArrowDown" || key == "Down") return SDL_SCANCODE_DOWN;
    if (key == "ArrowLeft" || key == "Left") return SDL_SCANCODE_LEFT;
    if (key == "ArrowRight" || key == "Right") return SDL_SCANCODE_RIGHT;
    if (key == "Home") return SDL_SCANCODE_HOME;
    if (key == "End") return SDL_SCANCODE_END;
    if (key == "PageUp") return SDL_SCANCODE_PAGEUP;
    if (key == "PageDown") return SDL_SCANCODE_PAGEDOWN;

    // Editing
    if (key == "Backspace") return SDL_SCANCODE_BACKSPACE;
    if (key == "Delete") return SDL_SCANCODE_DELETE;
    if (key == "Insert") return SDL_SCANCODE_INSERT;
    if (key == "Enter" || key == "Return") return SDL_SCANCODE_RETURN;
    if (key == "Tab") return SDL_SCANCODE_TAB;
    if (key == "Escape" || key == "Esc") return SDL_SCANCODE_ESCAPE;
    if (key == "Space" || key == " ") return SDL_SCANCODE_SPACE;

    // Modifiers
    if (key == "ShiftLeft" || key == "Shift") return SDL_SCANCODE_LSHIFT;
    if (key == "ShiftRight") return SDL_SCANCODE_RSHIFT;
    if (key == "ControlLeft" || key == "Control" || key == "Ctrl") return SDL_SCANCODE_LCTRL;
    if (key == "ControlRight") return SDL_SCANCODE_RCTRL;
    if (key == "AltLeft" || key == "Alt") return SDL_SCANCODE_LALT;
    if (key == "AltRight") return SDL_SCANCODE_RALT;
    if (key == "MetaLeft" || key == "Meta" || key == "Command" || key == "Win") return SDL_SCANCODE_LGUI;
    if (key == "MetaRight") return SDL_SCANCODE_RGUI;
    if (key == "CapsLock") return SDL_SCANCODE_CAPSLOCK;

    // Punctuation
    if (key == "Minus" || key == "-") return SDL_SCANCODE_MINUS;
    if (key == "Equal" || key == "=" || key == "Plus") return SDL_SCANCODE_EQUALS;
    if (key == "BracketLeft" || key == "[") return SDL_SCANCODE_LEFTBRACKET;
    if (key == "BracketRight" || key == "]") return SDL_SCANCODE_RIGHTBRACKET;
    if (key == "Backslash" || key == "\\") return SDL_SCANCODE_BACKSLASH;
    if (key == "Semicolon" || key == ";") return SDL_SCANCODE_SEMICOLON;
    if (key == "Quote" || key == "'") return SDL_SCANCODE_APOSTROPHE;
    if (key == "Backquote" || key == "`") return SDL_SCANCODE_GRAVE;
    if (key == "Comma" || key == ",") return SDL_SCANCODE_COMMA;
    if (key == "Period" || key == ".") return SDL_SCANCODE_PERIOD;
    if (key == "Slash" || key == "/") return SDL_SCANCODE_SLASH;

    return SDL_SCANCODE_UNKNOWN;
}

/**
 * Inject a keyboard event into SDL's event queue
 */
static bool injectKeyboardEvent(SDL_Scancode scancode, bool down) {
    if (scancode == SDL_SCANCODE_UNKNOWN) return false;

    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down = down;
    event.key.repeat = false;

    return SDL_PushEvent(&event) > 0;
}

/**
 * Inject a mouse motion event
 */
static bool injectMouseMotion(float x, float y) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = 0;
    event.motion.yrel = 0;

    return SDL_PushEvent(&event) > 0;
}

/**
 * Inject a mouse button event
 */
static bool injectMouseButton(float x, float y, int button, bool down) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    event.button.clicks = 1;

    return SDL_PushEvent(&event) > 0;
}

/**
 * Parse JSON to extract a string value for a key
 */
static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos);
    if (quoteStart == std::string::npos) return "";

    std::string result;
    bool escaped = false;
    for (size_t i = quoteStart + 1; i < json.size(); i++) {
        char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: result.push_back(c); break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return result;
        } else {
            result.push_back(c);
        }
    }
    return "";
}

static std::string escapeJson(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    result += "\\u00";
                    result.push_back(hex[(c >> 4) & 0x0f]);
                    result.push_back(hex[c & 0x0f]);
                } else {
                    result.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return result;
}

/**
 * Parse JSON to extract a number value for a key
 */
static double extractJsonNumber(const std::string& json, const std::string& key, double defaultValue = 0) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultValue;

    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return defaultValue;

    size_t start = colonPos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;

    size_t end = start;
    while (end < json.size() && (json[end] == '-' || json[end] == '.' || (json[end] >= '0' && json[end] <= '9'))) end++;

    if (end > start) {
        try {
            return std::stod(json.substr(start, end - start));
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

// Platform-specific headers for process termination
#ifdef _WIN32
#include <process.h>  // Windows: _exit()
#include <windows.h>  // Windows: ExitProcess()
#else
#include <unistd.h>   // POSIX: _exit(), getpid()
#include <signal.h>   // POSIX: kill(), SIGKILL
#endif

void printVersion() {
    std::cout << "Mystral Native Runtime v" << mystral::getVersion() << std::endl;
    std::cout << "Native WebGPU JS runtime - " << mystral::getWebGPUBackend() << " + " << mystral::getJSEngine() << " build" << std::endl;
}

void printHelp() {
    std::cout << R"(
Mystral CLI - Native Runtime for Mystral Engine

USAGE:
    mystral run <script.js> [options]         Run a JavaScript file
    mystral compile <entry.js> [options]      Bundle JS + assets into a single binary
    mystral --compile <entry.js> [options]    Same as compile
    mystral bake <input.glb|input.js> [options]  Bake lightmaps for a scene
    mystral --version                         Show version information
    mystral --help                            Show this help message

RUN OPTIONS:
    --width <n>           Window width (default: 1280)
    --height <n>          Window height (default: 720)
    --title <str>         Window title (default: "Mystral")
    --headless            Run with hidden window (background mode)
    --no-sdl              Run without SDL (headless GPU, no window system required)
    --watch, -w           Watch mode: reload script on file changes
    --no-vsync            Prefer mailbox/immediate presentation when supported
    --present-mode <mode> auto|fifo|fifo-relaxed|immediate|mailbox
    --max-frame-latency <n> Presentation latency target (default: 2)
    --gpu-memory-budget-mb <n> Reject tracked GPU allocations above this budget
    --screenshot <file>   Take screenshot after N frames and quit
    --frames <n>          Number of frames before screenshot (default: 60)
    --quiet, -q           Suppress all output except errors

VIDEO RECORDING OPTIONS:
    --video, --record <file>  Record video to file (WebP format, or MP4 with --mp4)
    --start-frame <n>     First frame to capture (default: 0)
    --end-frame <n>       Last frame to capture (required for video recording)
    --video-fps <n>       Video framerate (default: 60)
    --video-quality <n>   WebP quality 0-100 (default: 80, higher = better)
    --mp4                 Convert to MP4 via FFmpeg (auto-detected if --video ends in .mp4)
    --native-capture      Use OS-level screen capture (default on macOS 12.3+/Windows 10 1803+)
                          Directly encodes to H.264 MP4 with low CPU overhead
    --gpu-capture         Force GPU readback capture (fallback mode, works everywhere)

DEBUG/TESTING OPTIONS:
    --debug               Enable verbose debug logging (WebGPU, shaders, etc.)
    --debug-port <port>   Enable debug server on specified port (e.g., 9222)
                          Allows remote testing via WebSocket protocol
    --benchmark <frames>  Profile exactly N frames, then exit
    --warmup <frames>     Frames excluded before profiling (default: 120)
    --benchmark-output <file>
                          Write the benchmark report as JSON

COMPILE OPTIONS:
    --include <dir>       Asset directory to bundle (repeatable)
    --assets <dir>        Alias for --include
    --output <file>       Output binary path (default: ./<entry-stem>)
    --out, -o <file>      Alias for --output
    --root <dir>          Root directory for bundle paths (default: cwd)
    --bundle-only         Create standalone .bundle file (no exe, for .app packaging)

BAKE OPTIONS (Lightmap Generation):
    --output <dir>        Output directory for lightmaps (default: ./lightmaps)
    --resolution <n>      Max lightmap atlas size (default: 2048)
    --samples <n>         Rays per texel (default: 64)
    --bounces <n>         Light bounces for GI (default: 2)

HEADLESS MODE:
    Run without displaying a window (useful for servers, CI, etc.):

    mystral run game.js --headless
    MYSTRAL_HEADLESS=1 mystral run game.js

    In headless mode:
    - Window is created but hidden
    - WebGPU rendering still works (GPU is used)
    - All JavaScript APIs work normally
    - Combine with --screenshot or --video for automated capture

SCREENSHOT MODE:
    Capture rendered output to a PNG file:

    mystral run scene.js --screenshot output.png              # 60 frames (default)
    mystral run scene.js --screenshot output.png --frames 120 # 120 frames

VIDEO RECORDING MODE:
    Record game output to an animated WebP or MP4 file:

    mystral run game.js --video demo.webp --end-frame 300     # 5 sec at 60fps
    mystral run game.js --video demo.mp4 --end-frame 600      # 10 sec, auto-convert
    mystral run game.js --video demo.webp --mp4 --end-frame 300  # Explicit MP4 convert

    Notes:
    - MP4 conversion requires FFmpeg installed on your system
    - If FFmpeg is not found, the WebP file is kept
    - WebP files play directly in browsers and most apps

EXAMPLES:
    mystral run game.js                                       # Run interactively
    mystral run app.js --width 1920 --height 1080             # Custom size
    mystral run test.js --headless --screenshot out.png       # Headless + screenshot
    mystral run game.js --headless --video out.mp4 --end-frame 300  # Record 5 sec video
    MYSTRAL_HEADLESS=1 mystral run render.js --screenshot render.png --frames 10
    mystral compile game.js --include assets --out my-game    # Bundle into a single binary
    mystral compile game.js --include assets --out game.bundle --bundle-only  # Standalone bundle file
    mystral bake scene.glb --output ./lightmaps               # Bake lightmaps for scene
    mystral bake game.js --resolution 1024 --samples 128      # Bake with custom settings

ENVIRONMENT:
    MYSTRAL_HEADLESS=1        Run in headless mode (hidden window)
    MYSTRAL_DEBUG=1           Enable verbose debug logging
    MYSTRAL_BUNDLE=<path>     Load external bundle file (overrides auto-detection)

)" << std::endl;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct CLIOptions {
    std::string command;
    std::string scriptPath;
    int width = 1280;
    int height = 720;
    std::string title = "Mystral";
    bool showHelp = false;
    bool showVersion = false;
    bool headless = false;
    bool watch = false;  // Watch mode for hot reloading
    bool vsync = true;
    std::string presentMode = "auto";
    uint32_t maxFrameLatency = 2;
    uint64_t gpuMemoryBudgetMiB = 0;

    // Screenshot mode
    std::string screenshotPath;
    int frames = 60;
    bool quiet = false;
    bool noSdl = false;  // Run without SDL (headless GPU, no window)

    // Video recording mode
    std::string videoPath;      // Output video path
    int startFrame = 0;         // First frame to capture
    int endFrame = -1;          // Last frame to capture (-1 = unlimited until quit)
    int videoFps = 60;          // Video framerate
    int videoQuality = 80;      // WebP quality (0-100, higher = better quality, larger file)
    bool convertToMp4 = false;  // Convert WebP to MP4 via FFmpeg
    bool useNativeCapture = true;  // Use OS-level capture when available (macOS/Windows)

    // Compile options
    std::vector<std::string> assetDirs;
    std::string outputPath;
    std::string rootDir;
    bool bundleOnly = false;  // Create standalone .bundle file (no exe copy)

    // Debug server
    int debugPort = 0;  // Port for debug server (0 = disabled)

    // Fixed-frame runtime benchmark
    bool benchmarkRequested = false;
    int benchmarkFrames = 0;
    int benchmarkWarmupFrames = 120;
    std::string benchmarkOutputPath;

    // Verbose logging
    bool debug = false;  // Enable verbose WebGPU/shader logging

    // Bake options
    int bakeResolution = 2048;   // Max lightmap atlas size
    int bakeSamples = 64;        // Rays per texel
    int bakeBounces = 2;         // Light bounces for GI
};

CLIOptions parseArgs(int argc, char* argv[]) {
    CLIOptions opts;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            opts.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            opts.showVersion = true;
        } else if (arg == "--width" && i + 1 < argc) {
            opts.width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            opts.height = std::stoi(argv[++i]);
        } else if (arg == "--title" && i + 1 < argc) {
            opts.title = argv[++i];
        } else if ((arg == "--include" || arg == "--assets") && i + 1 < argc) {
            opts.assetDirs.push_back(argv[++i]);
        } else if ((arg == "--output" || arg == "--out" || arg == "-o") && i + 1 < argc) {
            opts.outputPath = argv[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            opts.rootDir = argv[++i];
        } else if (arg == "--entry" && i + 1 < argc) {
            opts.scriptPath = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opts.screenshotPath = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            opts.frames = std::stoi(argv[++i]);
        } else if (arg == "--quiet" || arg == "-q") {
            opts.quiet = true;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--no-sdl") {
            opts.noSdl = true;
        } else if (arg == "--watch" || arg == "-w") {
            opts.watch = true;
        } else if (arg == "--no-vsync") {
            opts.vsync = false;
        } else if (arg == "--present-mode" && i + 1 < argc) {
            opts.presentMode = argv[++i];
        } else if (arg == "--max-frame-latency" && i + 1 < argc) {
            opts.maxFrameLatency = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--gpu-memory-budget-mb" && i + 1 < argc) {
            opts.gpuMemoryBudgetMiB = std::stoull(argv[++i]);
        } else if (arg == "--bundle-only") {
            opts.bundleOnly = true;
        } else if ((arg == "--video" || arg == "--record") && i + 1 < argc) {
            opts.videoPath = argv[++i];
            // Auto-detect --mp4 from extension
            if (opts.videoPath.size() > 4) {
                std::string ext = opts.videoPath.substr(opts.videoPath.size() - 4);
                if (ext == ".mp4" || ext == ".MP4") {
                    opts.convertToMp4 = true;
                }
            }
        } else if (arg == "--start-frame" && i + 1 < argc) {
            opts.startFrame = std::stoi(argv[++i]);
        } else if (arg == "--end-frame" && i + 1 < argc) {
            opts.endFrame = std::stoi(argv[++i]);
        } else if (arg == "--video-fps" && i + 1 < argc) {
            opts.videoFps = std::stoi(argv[++i]);
        } else if (arg == "--video-quality" && i + 1 < argc) {
            opts.videoQuality = std::stoi(argv[++i]);
        } else if (arg == "--mp4") {
            opts.convertToMp4 = true;
        } else if (arg == "--native-capture") {
            opts.useNativeCapture = true;
        } else if (arg == "--gpu-capture" || arg == "--no-native-capture") {
            opts.useNativeCapture = false;
        } else if (arg == "--debug-port" && i + 1 < argc) {
            opts.debugPort = std::stoi(argv[++i]);
        } else if (arg == "--benchmark" && i + 1 < argc) {
            opts.benchmarkRequested = true;
            opts.benchmarkFrames = std::stoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            opts.benchmarkWarmupFrames = std::stoi(argv[++i]);
        } else if (arg == "--benchmark-output" && i + 1 < argc) {
            opts.benchmarkOutputPath = argv[++i];
        } else if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--resolution" && i + 1 < argc) {
            opts.bakeResolution = std::stoi(argv[++i]);
        } else if (arg == "--samples" && i + 1 < argc) {
            opts.bakeSamples = std::stoi(argv[++i]);
        } else if (arg == "--bounces" && i + 1 < argc) {
            opts.bakeBounces = std::stoi(argv[++i]);
        } else if ((arg == "run") && opts.command.empty()) {
            opts.command = "run";
        } else if ((arg == "compile" || arg == "--compile") && opts.command.empty()) {
            opts.command = "compile";
        } else if ((arg == "bake") && opts.command.empty()) {
            opts.command = "bake";
        } else if (opts.command == "run" && opts.scriptPath.empty()) {
            opts.scriptPath = arg;
        } else if (opts.command == "compile" && opts.scriptPath.empty() && (arg.empty() || arg[0] != '-')) {
            opts.scriptPath = arg;
        } else if (opts.command == "bake" && opts.scriptPath.empty() && (arg.empty() || arg[0] != '-')) {
            opts.scriptPath = arg;
        } else if (arg[0] == '-') {
            // Unknown flag - warn the user
            std::cerr << "Warning: Unknown option '" << arg << "'" << std::endl;
        }
    }

    return opts;
}

struct BundleFile {
    std::filesystem::path sourcePath;
    std::string bundlePath;
    uint64_t size = 0;
    uint64_t offset = 0;
};

static bool isSafeRelative(const std::filesystem::path& relPath) {
    if (relPath.empty() || relPath.is_absolute()) {
        return false;
    }
    for (const auto& part : relPath) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

static bool makeBundlePath(const std::filesystem::path& filePath,
                           const std::filesystem::path& rootDir,
                           std::string* outPath) {
    std::error_code ec;
    std::filesystem::path absRoot = std::filesystem::absolute(rootDir, ec).lexically_normal();
    if (ec) {
        return false;
    }
    std::filesystem::path absFile = std::filesystem::absolute(filePath, ec).lexically_normal();
    if (ec) {
        return false;
    }

    std::filesystem::path rel = std::filesystem::relative(absFile, absRoot, ec);
    if (ec || !isSafeRelative(rel)) {
        return false;
    }

    std::string normalized = mystral::vfs::normalizeBundlePath(rel.generic_string());
    if (normalized.empty()) {
        return false;
    }
    *outPath = normalized;
    return true;
}

static void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

static void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
}

static bool copyStream(std::ifstream& in, std::ofstream& out) {
    std::vector<char> buffer(64 * 1024);
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize readCount = in.gcount();
        if (readCount > 0) {
            out.write(buffer.data(), readCount);
            if (!out.good()) {
                return false;
            }
        }
    }
    return in.eof() && out.good();
}

static bool writeFileToStream(const std::filesystem::path& path, std::ofstream& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    return copyStream(in, out);
}

// Extract import/require specifiers from source code
static std::vector<std::string> extractImportSpecifiers(const std::string& source) {
    std::vector<std::string> specifiers;

    // ES6 import patterns
    std::regex importDefault(R"(import\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importAll(R"(import\s+\*\s+as\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importNamed(R"(import\s+\{[^}]+\}\s+from\s+['"]([^'"]+)['"])");
    std::regex importMixed(R"(import\s+[A-Za-z_$][\w$]*\s*,\s*\{[^}]+\}\s+from\s+['"]([^'"]+)['"])");
    std::regex importMixedAll(R"(import\s+[A-Za-z_$][\w$]*\s*,\s*\*\s+as\s+[A-Za-z_$][\w$]*\s+from\s+['"]([^'"]+)['"])");
    std::regex importSideEffect(R"(import\s+['"]([^'"]+)['"])");

    // CommonJS require patterns
    std::regex requireCall(R"(require\s*\(\s*['"]([^'"]+)['"]\s*\))");

    // Re-export patterns
    std::regex exportFrom(R"(export\s+(?:\{[^}]*\}|\*)\s+from\s+['"]([^'"]+)['"])");

    auto extractMatches = [&](const std::regex& re) {
        std::sregex_iterator it(source.begin(), source.end(), re);
        std::sregex_iterator end;
        while (it != end) {
            std::smatch match = *it;
            if (match.size() > 1) {
                specifiers.push_back(match[1].str());
            }
            ++it;
        }
    };

    extractMatches(importMixed);
    extractMatches(importMixedAll);
    extractMatches(importDefault);
    extractMatches(importAll);
    extractMatches(importNamed);
    extractMatches(importSideEffect);
    extractMatches(requireCall);
    extractMatches(exportFrom);

    return specifiers;
}

// Check if a path is a TypeScript file
static bool isTypeScriptFile(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot);
    return ext == ".ts" || ext == ".tsx" || ext == ".mts" || ext == ".cts";
}

// Collect all dependencies starting from entry file
static bool collectDependencies(
    const std::filesystem::path& entryPath,
    const std::filesystem::path& rootDir,
    std::vector<std::filesystem::path>& outFiles,
    std::unordered_set<std::string>& seen,
    bool quiet) {

    namespace fs = std::filesystem;
    mystral::js::ModuleResolver resolver(rootDir.string());

    std::queue<std::string> toProcess;
    std::string entryAbs = fs::absolute(entryPath).lexically_normal().generic_string();
    toProcess.push(entryAbs);
    seen.insert(entryAbs);
    outFiles.push_back(entryPath);

    while (!toProcess.empty()) {
        std::string currentPath = toProcess.front();
        toProcess.pop();

        // Read the file
        std::ifstream file(currentPath);
        if (!file.is_open()) {
            std::cerr << "Warning: Could not read file for dependency scanning: " << currentPath << std::endl;
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // If it's TypeScript, transpile it first to get accurate import parsing
        if (isTypeScriptFile(currentPath) && mystral::js::isTypeScriptTranspilerAvailable()) {
            std::string outJs, outError;
            if (mystral::js::transpileTypeScript(source, currentPath, outJs, outError)) {
                source = outJs;
            }
        }

        // Extract import specifiers
        std::vector<std::string> specifiers = extractImportSpecifiers(source);

        for (const std::string& spec : specifiers) {
            // Skip bare specifiers (npm packages) - only resolve relative/absolute imports
            if (!spec.empty() && spec[0] != '.' && spec[0] != '/') {
                // Check if it's a Windows absolute path (e.g., "C:/...")
                bool isWindowsAbs = spec.size() > 2 &&
                    std::isalpha(static_cast<unsigned char>(spec[0])) && spec[1] == ':';
                if (!isWindowsAbs) {
                    continue;  // Skip npm packages for now
                }
            }

            mystral::js::ResolvedModule resolved;
            std::string error;
            if (!resolver.resolve(spec, currentPath, mystral::js::ResolveMode::Import, resolved, error)) {
                // Try require mode as fallback
                if (!resolver.resolve(spec, currentPath, mystral::js::ResolveMode::Require, resolved, error)) {
                    if (!quiet) {
                        std::cerr << "Warning: Could not resolve import '" << spec << "' from " << currentPath << std::endl;
                    }
                    continue;
                }
            }

            std::string resolvedPath = resolved.resolved.path;
            if (seen.count(resolvedPath) > 0) {
                continue;  // Already processed
            }

            // Check if the resolved file exists
            std::error_code ec;
            if (!fs::exists(resolvedPath, ec) || !fs::is_regular_file(resolvedPath, ec)) {
                if (!quiet) {
                    std::cerr << "Warning: Resolved path does not exist: " << resolvedPath << std::endl;
                }
                continue;
            }

            seen.insert(resolvedPath);
            outFiles.push_back(fs::path(resolvedPath));
            toProcess.push(resolvedPath);
        }
    }

    return true;
}

// ============================================================================
// Video Recording (Animated WebP)
// ============================================================================

#ifdef MYSTRAL_HAS_WEBP_MUX

/**
 * WebP Video Recorder
 *
 * Records frames to an animated WebP file using libwebp's WebPAnimEncoder.
 * Optionally converts to MP4 using FFmpeg if available.
 */
class WebPVideoRecorder {
public:
    WebPVideoRecorder(int width, int height, int fps, int quality)
        : width_(width), height_(height), fps_(fps), quality_(quality),
          encoder_(nullptr), frameCount_(0), timestampMs_(0) {

        // Initialize animation encoder options
        if (!WebPAnimEncoderOptionsInit(&encOptions_)) {
            std::cerr << "[Video] Failed to initialize WebP encoder options" << std::endl;
            return;
        }

        // Set encoding options
        encOptions_.anim_params.loop_count = 0;  // Infinite loop
        encOptions_.allow_mixed = 0;  // All frames same format
        encOptions_.minimize_size = 0;  // Prioritize speed over size
        // Force every frame to be a keyframe (prevents frame differencing artifacts)
        encOptions_.kmin = 1;
        encOptions_.kmax = 1;

        // Create encoder
        encoder_ = WebPAnimEncoderNew(width, height, &encOptions_);
        if (!encoder_) {
            std::cerr << "[Video] Failed to create WebP animation encoder" << std::endl;
            return;
        }

        // Calculate frame duration in milliseconds
        frameDurationMs_ = 1000 / fps;
    }

    ~WebPVideoRecorder() {
        if (encoder_) {
            WebPAnimEncoderDelete(encoder_);
        }
    }

    bool isValid() const {
        return encoder_ != nullptr;
    }

    /**
     * Add a frame from RGBA pixel data
     * @param rgbaData Pointer to RGBA pixel data (width * height * 4 bytes)
     * @return true on success
     */
    bool addFrame(const uint8_t* rgbaData) {
        if (!encoder_) return false;

        // Set up WebP picture
        WebPPicture pic;
        if (!WebPPictureInit(&pic)) {
            std::cerr << "[Video] Failed to init WebP picture" << std::endl;
            return false;
        }

        pic.width = width_;
        pic.height = height_;
        pic.use_argb = 1;  // Use ARGB format

        // Allocate picture buffer
        if (!WebPPictureAlloc(&pic)) {
            std::cerr << "[Video] Failed to allocate WebP picture" << std::endl;
            return false;
        }

        // Convert RGBA to ARGB (WebP's internal format)
        // RGBA: R G B A -> ARGB: A R G B (but stored as 32-bit words)
        // Actually WebPPictureImportRGBA handles this for us
        if (!WebPPictureImportRGBA(&pic, rgbaData, width_ * 4)) {
            std::cerr << "[Video] Failed to import RGBA data" << std::endl;
            WebPPictureFree(&pic);
            return false;
        }

        // Set up encoding config
        WebPConfig config;
        if (!WebPConfigInit(&config)) {
            std::cerr << "[Video] Failed to init WebP config" << std::endl;
            WebPPictureFree(&pic);
            return false;
        }

        // Set quality (0-100)
        config.quality = static_cast<float>(quality_);
        config.method = 4;  // Compression method (0=fast, 6=slow but better)

        // Add frame to animation
        if (!WebPAnimEncoderAdd(encoder_, &pic, timestampMs_, &config)) {
            std::cerr << "[Video] Failed to add frame: " << WebPAnimEncoderGetError(encoder_) << std::endl;
            WebPPictureFree(&pic);
            return false;
        }

        WebPPictureFree(&pic);

        frameCount_++;
        timestampMs_ += frameDurationMs_;

        return true;
    }

    /**
     * Finalize and save the video to a file
     * @param outputPath Path to save the WebP file
     * @return true on success
     */
    bool save(const std::string& outputPath) {
        if (!encoder_) return false;

        // Add final "null" frame to signal end of animation
        if (!WebPAnimEncoderAdd(encoder_, nullptr, timestampMs_, nullptr)) {
            std::cerr << "[Video] Failed to finalize animation" << std::endl;
            return false;
        }

        // Assemble the animation
        WebPData webpData;
        WebPDataInit(&webpData);

        if (!WebPAnimEncoderAssemble(encoder_, &webpData)) {
            std::cerr << "[Video] Failed to assemble animation: " << WebPAnimEncoderGetError(encoder_) << std::endl;
            return false;
        }

        // Write to file
        std::ofstream file(outputPath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[Video] Failed to open output file: " << outputPath << std::endl;
            WebPDataClear(&webpData);
            return false;
        }

        file.write(reinterpret_cast<const char*>(webpData.bytes), webpData.size);
        file.close();

        WebPDataClear(&webpData);

        return true;
    }

    int getFrameCount() const { return frameCount_; }

private:
    int width_;
    int height_;
    int fps_;
    int quality_;
    WebPAnimEncoder* encoder_;
    WebPAnimEncoderOptions encOptions_;
    int frameCount_;
    int timestampMs_;
    int frameDurationMs_;
};

#endif // MYSTRAL_HAS_WEBP_MUX

/**
 * Check if FFmpeg is available on the system
 * Note: Not static - used by video recorders
 */
bool isFFmpegAvailable() {
#ifdef _WIN32
    int result = system("where ffmpeg >nul 2>nul");
#else
    int result = system("which ffmpeg >/dev/null 2>&1");
#endif
    return result == 0;
}

/**
 * Convert WebP to MP4 using FFmpeg
 * Note: FFmpeg's native webp decoder doesn't support animated WebP.
 * We use the anim_dump approach: extract frames, then encode.
 * @param webpPath Input WebP file path
 * @param mp4Path Output MP4 file path
 * @param fps Video framerate
 * @param deleteWebp Whether to delete the WebP file after conversion
 * @return true on success
 * Note: Not static - used by video recorders
 */
bool convertWebPToMP4(const std::string& webpPath, const std::string& mp4Path, int fps, bool deleteWebp, bool quiet) {
    if (!isFFmpegAvailable()) {
        if (!quiet) {
            std::cerr << "[Video] FFmpeg not found. WebP file saved: " << webpPath << std::endl;
            std::cerr << "[Video] Note: Animated WebP plays in browsers and many apps" << std::endl;
            std::cerr << "[Video] To convert to MP4, install FFmpeg and use a tool that supports animated WebP" << std::endl;
        }
        return false;
    }

    // Create temp directory for frames
    std::error_code ec;
    std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec) / ("mystral-video-" + std::to_string(std::time(nullptr)));
    if (ec) {
        if (!quiet) std::cerr << "[Video] Failed to get temp directory" << std::endl;
        return false;
    }
    std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        if (!quiet) std::cerr << "[Video] Failed to create temp directory" << std::endl;
        return false;
    }

    // Check if webpmux is available (from libwebp package)
    bool hasWebpmux = false;
#ifdef _WIN32
    hasWebpmux = system("where webpmux >nul 2>nul") == 0;
#else
    hasWebpmux = system("which webpmux >/dev/null 2>&1") == 0;
#endif

    bool success = false;

    if (hasWebpmux) {
        // Use webpmux to extract frames, then ffmpeg to encode
        if (!quiet) std::cout << "[Video] Extracting frames with webpmux..." << std::endl;

        // Get frame count (we'll try up to 10000 frames)
        std::string extractCmd = "webpmux -get frame 1 \"" + webpPath + "\" -o \"" + tempDir.string() + "/frame_0001.webp\"";
#ifdef _WIN32
        extractCmd += " 2>nul";
#else
        extractCmd += " 2>/dev/null";
#endif

        // Extract first frame to test
        if (system(extractCmd.c_str()) != 0) {
            if (!quiet) std::cerr << "[Video] Failed to extract frames from animated WebP" << std::endl;
        } else {
            // Extract all frames
            int frameNum = 1;
            while (frameNum <= 10000) {
                char framePath[512];
                snprintf(framePath, sizeof(framePath), "%s/frame_%04d.webp", tempDir.string().c_str(), frameNum);

                std::string cmd = "webpmux -get frame " + std::to_string(frameNum) + " \"" + webpPath + "\" -o \"" + framePath + "\"";
#ifdef _WIN32
                cmd += " 2>nul";
#else
                cmd += " 2>/dev/null";
#endif
                if (system(cmd.c_str()) != 0) break;
                frameNum++;
            }

            if (frameNum > 1) {
                if (!quiet) std::cout << "[Video] Extracted " << (frameNum - 1) << " frames, encoding to MP4..." << std::endl;

                // Use FFmpeg to encode frames to MP4
                std::string ffmpegCmd = "ffmpeg -y -framerate " + std::to_string(fps) +
                    " -i \"" + tempDir.string() + "/frame_%04d.webp\" -c:v libx264 -pix_fmt yuv420p -crf 18 \"" + mp4Path + "\"";
                if (quiet) ffmpegCmd += " -loglevel quiet";
#ifdef _WIN32
                else ffmpegCmd += " 2>nul";
#endif

                if (system(ffmpegCmd.c_str()) == 0) {
                    success = true;
                }
            }
        }
    } else {
        // webpmux not available, provide instructions
        if (!quiet) {
            std::cerr << "[Video] MP4 conversion requires 'webpmux' (from libwebp) to extract animated WebP frames" << std::endl;
            std::cerr << "[Video] Install libwebp-tools: brew install webp (macOS) or apt install webp (Linux)" << std::endl;
            std::cerr << "[Video] Or use an online converter that supports animated WebP to MP4" << std::endl;
        }
    }

    // Cleanup temp directory
    std::filesystem::remove_all(tempDir, ec);

    if (success) {
        // Delete the WebP file if requested
        if (deleteWebp) {
            std::filesystem::remove(webpPath, ec);
        }
    } else {
        if (!quiet) {
            std::cerr << "[Video] MP4 conversion failed. WebP file preserved: " << webpPath << std::endl;
        }
    }

    return success;
}

static int compileBundle(const CLIOptions& opts) {
    namespace fs = std::filesystem;

    if (opts.scriptPath.empty()) {
        std::cerr << "Error: No entry file specified for compile." << std::endl;
        return 1;
    }

    fs::path entryPath = opts.scriptPath;
    if (!fs::exists(entryPath) || !fs::is_regular_file(entryPath)) {
        std::cerr << "Error: Entry file not found: " << entryPath << std::endl;
        return 1;
    }

    fs::path rootDir = opts.rootDir.empty() ? fs::current_path() : fs::path(opts.rootDir);
    if (!fs::exists(rootDir) || !fs::is_directory(rootDir)) {
        std::cerr << "Error: Root directory not found: " << rootDir << std::endl;
        return 1;
    }

    std::string entryBundlePath;
    if (!makeBundlePath(entryPath, rootDir, &entryBundlePath)) {
        std::cerr << "Error: Entry path is outside bundle root: " << entryPath << std::endl;
        return 1;
    }

    std::vector<BundleFile> files;
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> seenBundlePaths;

    auto addFile = [&](const fs::path& filePath) -> bool {
        std::string bundlePath;
        if (!makeBundlePath(filePath, rootDir, &bundlePath)) {
            std::cerr << "Error: Asset path is outside bundle root: " << filePath << std::endl;
            return false;
        }
        if (!seenBundlePaths.insert(bundlePath).second) {
            return true;
        }
        std::error_code ec;
        uint64_t size = static_cast<uint64_t>(fs::file_size(filePath, ec));
        if (ec) {
            std::cerr << "Error: Failed to read file size: " << filePath << std::endl;
            return false;
        }
        files.push_back({ filePath, bundlePath, size, 0 });
        return true;
    };

    // Collect all dependencies starting from entry file
    std::vector<fs::path> dependencyFiles;
    if (!collectDependencies(entryPath, rootDir, dependencyFiles, seen, opts.quiet)) {
        std::cerr << "Error: Failed to collect dependencies" << std::endl;
        return 1;
    }

    // Add all discovered dependencies
    for (const auto& depPath : dependencyFiles) {
        if (!addFile(depPath)) {
            return 1;
        }
    }

    // Also check for package.json in the entry directory (needed for module format detection)
    fs::path entryDir = entryPath.parent_path();
    fs::path packageJsonPath = entryDir / "package.json";
    if (fs::exists(packageJsonPath) && fs::is_regular_file(packageJsonPath)) {
        addFile(packageJsonPath);
    }

    for (const auto& assetDir : opts.assetDirs) {
        fs::path dirPath = assetDir;
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
            std::cerr << "Error: Asset directory not found: " << dirPath << std::endl;
            return 1;
        }
        for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (!addFile(entry.path())) {
                return 1;
            }
        }
    }

    fs::path outputPath = opts.outputPath.empty()
        ? fs::current_path() / fs::path(entryPath).stem()
        : fs::path(opts.outputPath);
    if (outputPath.is_relative()) {
        outputPath = fs::absolute(outputPath);
    }

    if (opts.bundleOnly) {
        // Bundle-only mode: add .bundle extension if no extension specified
        if (outputPath.extension().empty()) {
            outputPath += ".bundle";
        }
    } else {
#ifdef _WIN32
        if (outputPath.extension() != ".exe") {
            outputPath += ".exe";
        }
#endif
    }

    std::error_code ec;
    fs::path outputDir = outputPath.parent_path();
    if (!outputDir.empty() && !fs::exists(outputDir)) {
        fs::create_directories(outputDir, ec);
        if (ec) {
            std::cerr << "Error: Failed to create output directory: " << outputDir << std::endl;
            return 1;
        }
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Error: Failed to create output file: " << outputPath << std::endl;
        return 1;
    }

    if (!opts.bundleOnly) {
        // Copy the runtime executable as the base of the compiled binary
        std::string exePath = mystral::vfs::getExecutablePath();
        if (exePath.empty()) {
            std::cerr << "Error: Could not resolve current executable path." << std::endl;
            return 1;
        }

        if (fs::exists(outputPath, ec) && fs::equivalent(outputPath, exePath, ec)) {
            std::cerr << "Error: Output path must be different from the current executable." << std::endl;
            return 1;
        }

        std::ifstream in(exePath, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "Error: Failed to open runtime binary: " << exePath << std::endl;
            return 1;
        }

        if (!copyStream(in, out)) {
            std::cerr << "Error: Failed to copy runtime binary." << std::endl;
            return 1;
        }
    }

    uint64_t bundleStart = static_cast<uint64_t>(out.tellp());
    for (auto& file : files) {
        file.offset = static_cast<uint64_t>(out.tellp()) - bundleStart;
        if (!writeFileToStream(file.sourcePath, out)) {
            std::cerr << "Error: Failed to write file: " << file.sourcePath << std::endl;
            return 1;
        }
    }

    std::vector<uint8_t> index;
    appendU32(index, mystral::vfs::kBundleVersion);
    appendU32(index, static_cast<uint32_t>(files.size()));
    appendU32(index, static_cast<uint32_t>(entryBundlePath.size()));
    appendU32(index, 0);
    index.insert(index.end(), entryBundlePath.begin(), entryBundlePath.end());

    for (const auto& file : files) {
        appendU32(index, static_cast<uint32_t>(file.bundlePath.size()));
        appendU32(index, 0);
        appendU64(index, file.offset);
        appendU64(index, file.size);
        index.insert(index.end(), file.bundlePath.begin(), file.bundlePath.end());
    }

    out.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    if (!out.good()) {
        std::cerr << "Error: Failed to write bundle index." << std::endl;
        return 1;
    }

    std::vector<uint8_t> footer;
    footer.insert(footer.end(),
                  mystral::vfs::kBundleMagic,
                  mystral::vfs::kBundleMagic + mystral::vfs::kBundleMagicSize);
    appendU32(footer, mystral::vfs::kBundleVersion);
    appendU32(footer, 0);
    appendU64(footer, static_cast<uint64_t>(index.size()));

    out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
    out.flush();
    if (!out.good()) {
        std::cerr << "Error: Failed to finalize bundle." << std::endl;
        return 1;
    }

    if (!opts.bundleOnly) {
        // Copy executable permissions (only for compiled binaries, not standalone bundles)
        std::string exePath = mystral::vfs::getExecutablePath();
        auto perms = fs::status(exePath, ec).permissions();
        if (!ec) {
            fs::permissions(outputPath, perms, ec);
        }
#ifndef _WIN32
        if (!ec) {
            fs::permissions(outputPath,
                            perms | fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            ec);
        }
#endif
    }

    if (!opts.quiet) {
        std::cout << "Bundle complete!" << std::endl;
        std::cout << "Entry: " << entryBundlePath << std::endl;
        std::cout << "Files bundled: " << files.size() << std::endl;
        std::cout << "Output: " << outputPath << std::endl;
        if (opts.bundleOnly) {
            std::cout << "Mode: standalone bundle (place as game.bundle next to mystral binary)" << std::endl;
        }
    }

    return 0;
}

static void appendProfileStatisticsJson(
    std::ostringstream& out,
    const mystral::RuntimeProfileStatistics& stats) {
    out << "{\"minMs\":" << stats.minMs
        << ",\"meanMs\":" << stats.meanMs
        << ",\"p50Ms\":" << stats.p50Ms
        << ",\"p95Ms\":" << stats.p95Ms
        << ",\"p99Ms\":" << stats.p99Ms
        << ",\"maxMs\":" << stats.maxMs << "}";
}

static void appendMemorySnapshotJson(
    std::ostringstream& out,
    const mystral::RuntimeMemorySnapshot& memory) {
    out << "{\"heapUsedBytes\":" << memory.heapUsedBytes
        << ",\"heapTotalBytes\":" << memory.heapTotalBytes
        << ",\"heapLimitBytes\":" << memory.heapLimitBytes
        << ",\"nativeFunctions\":" << memory.nativeFunctions
        << ",\"frameHandles\":" << memory.frameHandles << "}";
}

static std::string makeBenchmarkJson(
    const mystral::RuntimeProfileReport& report,
    int requestedFrames,
    int warmupFrames,
    const std::string& scriptPath,
    const std::string& workloadJson) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    const double throughputFps = report.wallTimeMs > 0.0
        ? static_cast<double>(report.sampledFrames) * 1000.0 / report.wallTimeMs
        : 0.0;
    const int64_t heapDelta = static_cast<int64_t>(report.memoryEnd.heapUsedBytes)
        - static_cast<int64_t>(report.memoryStart.heapUsedBytes);

    out << "{\"schemaVersion\":1"
        << ",\"runtimeVersion\":\"" << escapeJson(mystral::getVersion()) << "\""
        << ",\"jsEngine\":\"" << escapeJson(mystral::getJSEngine()) << "\""
        << ",\"webgpuBackend\":\"" << escapeJson(mystral::getWebGPUBackend()) << "\""
        << ",\"script\":\"" << escapeJson(scriptPath) << "\""
        << ",\"workload\":" << (workloadJson.empty() ? "null" : workloadJson)
        << ",\"requestedFrames\":" << requestedFrames
        << ",\"warmupFrames\":" << warmupFrames
        << ",\"sampledFrames\":" << report.sampledFrames
        << ",\"wallTimeMs\":" << report.wallTimeMs
        << ",\"throughputFps\":" << throughputFps
        << ",\"phases\":{";
    out << "\"frame\":";
    appendProfileStatisticsJson(out, report.frame);
    out << ",\"events\":";
    appendProfileStatisticsJson(out, report.events);
    out << ",\"asyncWork\":";
    appendProfileStatisticsJson(out, report.asyncWork);
    out << ",\"callbacks\":";
    appendProfileStatisticsJson(out, report.callbacks);
    out << ",\"simulation\":";
    appendProfileStatisticsJson(out, report.simulation);
    out << ",\"animationFrame\":";
    appendProfileStatisticsJson(out, report.animationFrame);
    out << ",\"cleanup\":";
    appendProfileStatisticsJson(out, report.cleanup);
    out << "},\"frameBudgetMisses\":{"
        << "\"8.33ms\":" << report.framesOver8_33Ms
        << ",\"16.67ms\":" << report.framesOver16_67Ms
        << ",\"33.33ms\":" << report.framesOver33_33Ms
        << "},\"gameLoop\":{"
        << "\"ticksExecuted\":" << report.gameTicksExecuted
        << ",\"ticksDropped\":" << report.gameTicksDropped
        << ",\"catchUpFrames\":" << report.gameCatchUpFrames
        << ",\"clockClampFrames\":" << report.gameClockClampFrames
        << ",\"handlerFailures\":" << report.gameTickHandlerFailures
        << ",\"maxTicksPerFrame\":" << report.gameMaxTicksPerFrame
        << "},\"workers\":{"
        << "\"created\":" << report.workersCreated
        << ",\"nestedCreated\":" << report.nestedWorkersCreated
        << ",\"activeStart\":" << report.workersActiveStart
        << ",\"activeEnd\":" << report.workersActiveEnd
        << ",\"maxDepth\":" << report.workerMaxDepth
        << ",\"messagesProcessed\":" << report.workerMessagesProcessed
        << ",\"timerCallbacks\":" << report.workerTimerCallbacks
        << ",\"messagesRejected\":" << report.workerMessagesRejected
        << ",\"inputMessagesTooLarge\":" << report.workerInputMessagesTooLarge
        << ",\"inputQueueFull\":" << report.workerInputQueueFull
        << ",\"outputMessagesTooLarge\":" << report.workerOutputMessagesTooLarge
        << ",\"outputQueueFull\":" << report.workerOutputQueueFull
        << ",\"busyTimeMs\":" << report.workerBusyTimeMs
        << ",\"inputQueueEndBytes\":" << report.workerInputQueueEndBytes
        << ",\"outputQueueEndBytes\":" << report.workerOutputQueueEndBytes
        << ",\"inputQueuePeakBytes\":" << report.workerInputQueuePeakBytes
        << ",\"outputQueuePeakBytes\":" << report.workerOutputQueuePeakBytes
        << ",\"largestInputMessageBytes\":" << report.workerLargestInputMessageBytes
        << ",\"largestOutputMessageBytes\":" << report.workerLargestOutputMessageBytes
        << ",\"sharedMemoryStartBytes\":" << report.sharedMemoryStartBytes
        << ",\"sharedMemoryEndBytes\":" << report.sharedMemoryEndBytes
        << "},\"jobs\":{"
        << "\"workerCount\":" << report.jobWorkerCount
        << ",\"cpuBudgetThreads\":" << report.cpuBudgetThreads
        << ",\"cpuBudgetActiveEnd\":" << report.cpuBudgetActiveEnd
        << ",\"cpuBudgetPeakActive\":" << report.cpuBudgetPeakActive
        << ",\"submitted\":" << report.jobsSubmitted
        << ",\"completed\":" << report.jobsCompleted
        << ",\"cancelled\":" << report.jobsCancelled
        << ",\"failed\":" << report.jobsFailed
        << ",\"rejected\":" << report.jobsRejected
        << ",\"queueEnd\":" << report.jobQueueEnd
        << ",\"queuePeak\":" << report.jobQueuePeak
        << ",\"inFlightEnd\":" << report.jobInFlightEnd
        << ",\"inFlightPeak\":" << report.jobInFlightPeak
        << ",\"queueWait\":";
    appendProfileStatisticsJson(out, report.jobQueueWait);
    out << ",\"execution\":";
    appendProfileStatisticsJson(out, report.jobExecution);
    out
        << "},\"memory\":{\"start\":";
    appendMemorySnapshotJson(out, report.memoryStart);
    out << ",\"end\":";
    appendMemorySnapshotJson(out, report.memoryEnd);
    out << ",\"heapUsedDeltaBytes\":" << heapDelta << "}}";
    return out.str();
}

static void printBenchmarkSummary(
    const mystral::RuntimeProfileReport& report,
    int requestedFrames,
    int warmupFrames) {
    const double throughputFps = report.wallTimeMs > 0.0
        ? static_cast<double>(report.sampledFrames) * 1000.0 / report.wallTimeMs
        : 0.0;
    const double heapDeltaMiB = (
        static_cast<double>(report.memoryEnd.heapUsedBytes)
        - static_cast<double>(report.memoryStart.heapUsedBytes)) / (1024.0 * 1024.0);

    std::cout << "=== Runtime benchmark ===\n"
              << "Frames: " << report.sampledFrames << "/" << requestedFrames
              << " measured after " << warmupFrames << " warmup\n"
              << "Engine: " << mystral::getJSEngine()
              << " | WebGPU: " << mystral::getWebGPUBackend() << "\n\n"
              << std::left << std::setw(18) << "Phase"
              << std::right << std::setw(10) << "mean"
              << std::setw(10) << "p50"
              << std::setw(10) << "p95"
              << std::setw(10) << "p99"
              << std::setw(10) << "max" << "\n";

    auto printPhase = [](const char* name, const mystral::RuntimeProfileStatistics& stats) {
        std::cout << std::left << std::setw(18) << name
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(10) << stats.meanMs
                  << std::setw(10) << stats.p50Ms
                  << std::setw(10) << stats.p95Ms
                  << std::setw(10) << stats.p99Ms
                  << std::setw(10) << stats.maxMs << "\n";
    };

    printPhase("frame", report.frame);
    printPhase("events", report.events);
    printPhase("async", report.asyncWork);
    printPhase("callbacks", report.callbacks);
    printPhase("simulation", report.simulation);
    printPhase("animationFrame", report.animationFrame);
    printPhase("cleanup", report.cleanup);
    printPhase("jobQueueWait", report.jobQueueWait);
    printPhase("jobExecution", report.jobExecution);

    std::cout << "\nBudget misses: >8.33ms=" << report.framesOver8_33Ms
              << " >16.67ms=" << report.framesOver16_67Ms
              << " >33.33ms=" << report.framesOver33_33Ms << "\n"
              << "Throughput: " << std::fixed << std::setprecision(1) << throughputFps << " frames/s\n"
              << "Game loop: ticks=" << report.gameTicksExecuted
              << " dropped=" << report.gameTicksDropped
              << " catchUpFrames=" << report.gameCatchUpFrames
              << " clockClamps=" << report.gameClockClampFrames
              << " failures=" << report.gameTickHandlerFailures
              << " maxTicks/frame=" << report.gameMaxTicksPerFrame << "\n"
              << "Workers: created=" << report.workersCreated
              << " nested=" << report.nestedWorkersCreated
              << " depth=" << report.workerMaxDepth
              << " messages=" << report.workerMessagesProcessed
              << " timers=" << report.workerTimerCallbacks
              << " rejected=" << report.workerMessagesRejected
              << " (in-large=" << report.workerInputMessagesTooLarge
              << " in-full=" << report.workerInputQueueFull
              << " out-large=" << report.workerOutputMessagesTooLarge
              << " out-full=" << report.workerOutputQueueFull << ")"
              << " busy=" << std::setprecision(3) << report.workerBusyTimeMs << " ms"
              << " queuePeak=" << report.workerInputQueuePeakBytes
              << "/" << report.workerOutputQueuePeakBytes << " bytes"
              << " messageMax=" << report.workerLargestInputMessageBytes
              << "/" << report.workerLargestOutputMessageBytes << " bytes"
              << " shared=" << report.sharedMemoryEndBytes << " bytes\n"
              << "Jobs: workers=" << report.jobWorkerCount
              << " cpuBudget=" << report.cpuBudgetActiveEnd
              << "/" << report.cpuBudgetPeakActive
              << "/" << report.cpuBudgetThreads
              << " submitted=" << report.jobsSubmitted
              << " completed=" << report.jobsCompleted
              << " cancelled=" << report.jobsCancelled
              << " failed=" << report.jobsFailed
              << " rejected=" << report.jobsRejected
              << " queue=" << report.jobQueueEnd << "/" << report.jobQueuePeak
              << " inFlight=" << report.jobInFlightEnd << "/" << report.jobInFlightPeak << "\n"
              << "Heap delta: " << std::showpos << std::setprecision(3) << heapDeltaMiB
              << std::noshowpos << " MiB\n";
}

int runScript(const CLIOptions& opts) {
    const bool benchmarkMode = opts.benchmarkRequested;
    if ((benchmarkMode && opts.benchmarkFrames <= 0) || opts.benchmarkWarmupFrames < 0) {
        std::cerr << "Error: Benchmark frames must be positive and warmup frames non-negative." << std::endl;
        return 1;
    }
    if (!benchmarkMode && !opts.benchmarkOutputPath.empty()) {
        std::cerr << "Error: --benchmark-output requires --benchmark." << std::endl;
        return 1;
    }
    if (benchmarkMode && (!opts.screenshotPath.empty() || !opts.videoPath.empty())) {
        std::cerr << "Error: Benchmark mode cannot be combined with screenshot or video capture." << std::endl;
        return 1;
    }

    // Enable headless mode via environment variable (SDL3 uses this)
    if (opts.headless) {
        #ifdef _WIN32
        _putenv_s("MYSTRAL_HEADLESS", "1");
        #else
        setenv("MYSTRAL_HEADLESS", "1", 1);
        #endif
    }

    bool screenshotMode = !opts.screenshotPath.empty();
    bool videoMode = !opts.videoPath.empty();

    if (!opts.quiet) {
        std::cout << "=== Mystral Native Runtime ===" << std::endl;
        std::cout << "Version: " << mystral::getVersion() << std::endl;
        std::cout << "Script: " << opts.scriptPath << std::endl;
        std::cout << "Window: " << opts.width << "x" << opts.height << std::endl;
        if (screenshotMode) {
            std::cout << "Screenshot mode: " << opts.frames << " frames -> " << opts.screenshotPath << std::endl;
        }
        if (videoMode) {
            std::cout << "Video mode: frames " << opts.startFrame << "-";
            if (opts.endFrame >= 0) {
                std::cout << opts.endFrame;
            } else {
                std::cout << "end";
            }
            std::cout << " @ " << opts.videoFps << "fps -> " << opts.videoPath << std::endl;
        }
        if (opts.watch) {
            std::cout << "Watch mode: enabled (hot reload on file changes)" << std::endl;
        }
        if (opts.debugPort > 0) {
            std::cout << "Debug server: port " << opts.debugPort << std::endl;
        }
        if (benchmarkMode) {
            std::cout << "Benchmark mode: " << opts.benchmarkFrames
                      << " measured frames after " << opts.benchmarkWarmupFrames
                      << " warmup frames" << std::endl;
        }
        std::cout << std::endl;
    }

    // Check for debug mode via environment variable
    bool debugMode = opts.debug;
    const char* debugEnv = std::getenv("MYSTRAL_DEBUG");
    if (debugEnv && (std::string(debugEnv) == "1" || std::string(debugEnv) == "true")) {
        debugMode = true;
    }

    // Create runtime
    mystral::RuntimeConfig config;
    config.width = opts.width;
    config.height = opts.height;
    config.title = opts.title.c_str();
    config.noSdl = opts.noSdl;
    config.watch = opts.watch;
    config.debug = debugMode;
    config.vsync = opts.vsync;
    config.maxFrameLatency = std::max<uint32_t>(1, opts.maxFrameLatency);
    if (opts.gpuMemoryBudgetMiB > (std::numeric_limits<uint64_t>::max)() / (1024ull * 1024ull)) {
        std::cerr << "Error: --gpu-memory-budget-mb is too large." << std::endl;
        return 1;
    }
    config.maxTrackedGpuMemoryBytes = opts.gpuMemoryBudgetMiB * 1024ull * 1024ull;
    if (opts.presentMode == "auto") config.presentMode = mystral::PresentMode::Auto;
    else if (opts.presentMode == "fifo") config.presentMode = mystral::PresentMode::Fifo;
    else if (opts.presentMode == "fifo-relaxed") config.presentMode = mystral::PresentMode::FifoRelaxed;
    else if (opts.presentMode == "immediate") config.presentMode = mystral::PresentMode::Immediate;
    else if (opts.presentMode == "mailbox") config.presentMode = mystral::PresentMode::Mailbox;
    else {
        std::cerr << "Error: invalid --present-mode '" << opts.presentMode << "'." << std::endl;
        return 1;
    }

    struct DebugLogEntry {
        uint64_t sequence;
        std::string type;
        std::string message;
    };
    struct PendingFrameWait {
        int clientId;
        int requestId;
        uint64_t targetFrame;
        std::chrono::steady_clock::time_point deadline;
    };

    std::unique_ptr<mystral::debug::DebugServer> debugServer;
    std::deque<DebugLogEntry> debugLogs;
    std::vector<PendingFrameWait> pendingFrameWaits;
    uint64_t nextLogSequence = 1;
    int quitDelayPolls = -1;
    bool debugProfilerActive = false;
    uint64_t debugProfileExpectedFrames = 0;
    constexpr size_t maxDebugLogs = 1000;

    auto runtime = mystral::Runtime::create(config);
    if (!runtime) {
        std::cerr << "Error: Failed to create runtime!" << std::endl;
        return 1;
    }

    runtime->setConsoleCallback([&](const std::string& type, const std::string& message) {
        DebugLogEntry entry{nextLogSequence++, type, message};
        debugLogs.push_back(entry);
        if (debugLogs.size() > maxDebugLogs) {
            debugLogs.pop_front();
        }
        if (debugServer && debugServer->isRunning()) {
            debugServer->broadcastEvent(
                "console",
                "{\"sequence\":" + std::to_string(entry.sequence) +
                ",\"type\":\"" + escapeJson(entry.type) +
                "\",\"message\":\"" + escapeJson(entry.message) + "\"}");
        }
    });

    // Load and execute the script
    if (!runtime->loadScript(opts.scriptPath)) {
        std::cerr << "Error: Failed to evaluate script!" << std::endl;
        return 1;
    }

    if (benchmarkMode) {
        for (int frame = 0; frame < opts.benchmarkWarmupFrames; frame++) {
            if (!runtime->pollEvents()) {
                std::cerr << "Error: Runtime quit during benchmark warmup at frame "
                          << frame << "." << std::endl;
                return 1;
            }
        }

        runtime->startProfiler(static_cast<uint64_t>(opts.benchmarkFrames));
        bool quitEarly = false;
        for (int frame = 0; frame < opts.benchmarkFrames; frame++) {
            if (!runtime->pollEvents()) {
                quitEarly = frame + 1 < opts.benchmarkFrames;
                break;
            }
        }
        const mystral::RuntimeProfileReport report = runtime->stopProfiler();
        const mystral::EvaluationResult workload = runtime->evaluateExpression(
            "globalThis.__mystralRuntimeBenchmark ?? null");

        if (!opts.quiet) {
            printBenchmarkSummary(
                report,
                opts.benchmarkFrames,
                opts.benchmarkWarmupFrames);
        }

        const std::string json = makeBenchmarkJson(
            report,
            opts.benchmarkFrames,
            opts.benchmarkWarmupFrames,
            opts.scriptPath,
            workload.success ? workload.valueJson : "");
        if (!opts.benchmarkOutputPath.empty()) {
            std::ofstream output(opts.benchmarkOutputPath, std::ios::trunc);
            if (!output.is_open()) {
                std::cerr << "Error: Cannot write benchmark report: "
                          << opts.benchmarkOutputPath << std::endl;
                return 1;
            }
            output << json << "\n";
            if (!opts.quiet) {
                std::cout << "Report: " << opts.benchmarkOutputPath << std::endl;
            }
        } else if (opts.quiet) {
            std::cout << json << std::endl;
        }

        if (quitEarly || report.sampledFrames != static_cast<uint64_t>(opts.benchmarkFrames)) {
            std::cerr << "Error: Benchmark ended after " << report.sampledFrames
                      << " of " << opts.benchmarkFrames << " measured frames." << std::endl;
            return 1;
        }
        return runtime->getExitCode();
    }

    if (screenshotMode) {
        // Screenshot mode: run for N frames, take screenshot, quit
        auto startTime = std::chrono::high_resolution_clock::now();

        for (int frame = 0; frame < opts.frames; frame++) {
            if (!runtime->pollEvents()) {
                if (!opts.quiet) {
                    std::cerr << "Warning: Runtime quit early at frame " << frame << std::endl;
                }
                break;
            }

            // Small delay to let GPU work complete
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Take screenshot
        bool success = runtime->saveScreenshot(opts.screenshotPath);

        if (!opts.quiet) {
            if (success) {
                std::cout << "Screenshot saved: " << opts.screenshotPath << std::endl;
                std::cout << "Rendered " << opts.frames << " frames in " << duration.count() << "ms" << std::endl;
            } else {
                std::cerr << "Error: Failed to save screenshot!" << std::endl;
            }
        }

        // In screenshot mode, use _exit() to avoid cleanup crashes
        // that can trigger the macOS crash dialog. The screenshot is
        // already saved, so we don't need graceful shutdown.
        std::cout.flush();
        std::cerr.flush();
        _exit(success ? 0 : 1);
    } else if (videoMode) {
        // Video recording mode
        // Try native OS-level capture first (ScreenCaptureKit on macOS, Windows.Graphics.Capture on Windows)
        // Falls back to GPU readback + WebP encoding if native capture is not available

        // Validate options
        if (opts.endFrame < 0) {
            std::cerr << "Error: --end-frame is required for video recording" << std::endl;
            std::cerr << "Example: mystral run game.js --video output.mp4 --end-frame 300" << std::endl;
            return 1;
        }

        if (opts.endFrame <= opts.startFrame) {
            std::cerr << "Error: --end-frame must be greater than --start-frame" << std::endl;
            return 1;
        }

        // Check if native capture is available and requested
        bool useNative = opts.useNativeCapture && mystral::video::VideoRecorder::isNativeCaptureAvailable();

        // Determine output path
        std::string outputPath = opts.videoPath;

        // Native capture outputs MP4 directly; GPU capture uses WebP
        if (useNative) {
            // Ensure output path ends with .mp4 for native capture
            size_t dotPos = outputPath.rfind('.');
            if (dotPos != std::string::npos) {
                std::string ext = outputPath.substr(dotPos);
                if (ext != ".mp4" && ext != ".MP4") {
                    outputPath = outputPath.substr(0, dotPos) + ".mp4";
                }
            } else {
                outputPath += ".mp4";
            }
        }

        // Create video recorder (factory selects appropriate backend)
        std::unique_ptr<mystral::video::VideoRecorder> recorder;
        if (useNative) {
            recorder = mystral::video::VideoRecorder::create(nullptr, nullptr, nullptr);
        } else {
            // GPU readback mode requires WebGPU handles
            recorder = mystral::video::VideoRecorder::create(
                static_cast<WGPUDevice>(runtime->getWGPUDevice()),
                static_cast<WGPUQueue>(runtime->getWGPUQueue()),
                static_cast<WGPUInstance>(runtime->getWGPUInstance())
            );
        }

        if (!recorder) {
#ifdef MYSTRAL_HAS_WEBP_MUX
            // Fall back to legacy WebP recorder
            if (!opts.quiet) {
                std::cout << "[Video] Falling back to legacy WebP recorder..." << std::endl;
            }

            // Determine output paths for WebP fallback
            std::string webpPath = opts.videoPath;
            std::string mp4Path;
            bool needsConversion = opts.convertToMp4;

            // If output ends with .mp4, convert the path to .webp for intermediate file
            if (needsConversion) {
                mp4Path = opts.videoPath;
                size_t dotPos = mp4Path.rfind('.');
                if (dotPos != std::string::npos) {
                    mp4Path = mp4Path.substr(0, dotPos) + ".mp4";
                } else {
                    mp4Path = mp4Path + ".mp4";
                }

                dotPos = webpPath.rfind('.');
                if (dotPos != std::string::npos) {
                    std::string ext = webpPath.substr(dotPos);
                    if (ext != ".webp" && ext != ".WEBP") {
                        webpPath = webpPath.substr(0, dotPos) + ".webp";
                    }
                } else {
                    webpPath = webpPath + ".webp";
                }
            }

            // Create legacy WebP recorder
            WebPVideoRecorder legacyRecorder(opts.width, opts.height, opts.videoFps, opts.videoQuality);
            if (!legacyRecorder.isValid()) {
                std::cerr << "Error: Failed to create video recorder" << std::endl;
                return 1;
            }

            if (!opts.quiet) {
                std::cout << "[Video] Recording " << (opts.endFrame - opts.startFrame + 1) << " frames..." << std::endl;
            }

            // Frame queue for async encoding
            std::queue<std::vector<uint8_t>> frameQueue;
            std::mutex queueMutex;
            std::atomic<bool> encodingDone{false};
            std::atomic<int> encodedFrames{0};
            const int maxQueuedFrames = 30;

            // Start encoder thread
            std::thread encoderThread([&]() {
                while (!encodingDone || !frameQueue.empty()) {
                    std::vector<uint8_t> frameData;
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        if (frameQueue.empty()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            continue;
                        }
                        frameData = std::move(frameQueue.front());
                        frameQueue.pop();
                    }

                    if (!frameData.empty()) {
                        legacyRecorder.addFrame(frameData.data());
                        encodedFrames++;
                    }
                }
            });

            auto startTime = std::chrono::high_resolution_clock::now();
            int capturedFrames = 0;

            for (int frame = 0; frame <= opts.endFrame; frame++) {
                if (!runtime->pollEvents()) {
                    if (!opts.quiet) {
                        std::cerr << "[Video] Runtime quit early at frame " << frame << std::endl;
                    }
                    break;
                }

                if (frame >= opts.startFrame) {
                    std::vector<uint8_t> frameData;
                    uint32_t frameWidth, frameHeight;

                    if (runtime->captureFrame(frameData, frameWidth, frameHeight)) {
                        bool queued = false;
                        {
                            std::lock_guard<std::mutex> lock(queueMutex);
                            if (frameQueue.size() < static_cast<size_t>(maxQueuedFrames)) {
                                frameQueue.push(std::move(frameData));
                                queued = true;
                            }
                        }

                        if (queued) {
                            capturedFrames++;
                            if (!opts.quiet && capturedFrames % 60 == 0) {
                                std::cout << "[Video] Captured frame " << capturedFrames << "/" << (opts.endFrame - opts.startFrame + 1)
                                          << " (queue: " << frameQueue.size() << ", encoded: " << encodedFrames.load() << ")" << std::endl;
                            }
                        }
                    }
                }
            }

            encodingDone = true;
            encoderThread.join();

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            runtime.reset();
            SDL_PumpEvents();

            bool success = legacyRecorder.save(webpPath);

            if (success) {
                if (!opts.quiet) {
                    std::cout << "[Video] Saved WebP: " << webpPath << std::endl;
                    std::cout << "[Video] Recorded " << capturedFrames << " frames in " << duration.count() << "ms" << std::endl;
                }

                if (needsConversion) {
                    if (convertWebPToMP4(webpPath, mp4Path, opts.videoFps, true, opts.quiet)) {
                        if (!opts.quiet) {
                            std::cout << "[Video] Converted to MP4: " << mp4Path << std::endl;
                        }
                    }
                }
            }

            std::cout.flush();
            std::cerr.flush();
            _exit(success ? 0 : 1);
#else
            std::cerr << "Error: Video recording requires libwebpmux (build with MYSTRAL_HAS_WEBP_MUX)" << std::endl;
            return 1;
#endif
        }

        // Using new VideoRecorder API
        if (!opts.quiet) {
            std::cout << "[Video] Using " << recorder->getTypeName() << std::endl;
            std::cout << "[Video] Recording " << (opts.endFrame - opts.startFrame + 1) << " frames to " << outputPath << std::endl;
        }

        // Configure recording
        mystral::video::VideoRecorderConfig recConfig;
        recConfig.fps = opts.videoFps;
        recConfig.width = opts.width;
        recConfig.height = opts.height;
        recConfig.quality = opts.videoQuality;
        recConfig.convertToMp4 = opts.convertToMp4;

        // Start recording
        if (!recorder->startRecording(runtime->getSDLWindow(), outputPath, recConfig)) {
            std::cerr << "Error: Failed to start video recording" << std::endl;
            return 1;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Main recording loop
        for (int frame = 0; frame <= opts.endFrame; frame++) {
            if (!runtime->pollEvents()) {
                if (!opts.quiet) {
                    std::cerr << "[Video] Runtime quit early at frame " << frame << std::endl;
                }
                break;
            }

            // Capture frame (for GPU readback recorder; no-op for native capture)
            if (frame >= opts.startFrame) {
                void* texture = runtime->getCurrentTexture();
                if (texture) {
                    recorder->captureFrame(texture, opts.width, opts.height);
                }
            }

            // Process any pending capture operations
            recorder->processFrame();

            // Progress reporting
            if (!opts.quiet && frame >= opts.startFrame && (frame - opts.startFrame) % 60 == 0) {
                auto stats = recorder->getStats();
                std::cout << "[Video] Frame " << (frame - opts.startFrame) << "/" << (opts.endFrame - opts.startFrame + 1)
                          << " (captured: " << stats.capturedFrames << ", dropped: " << stats.droppedFrames << ")" << std::endl;
            }
        }

        // Stop recording
        bool success = recorder->stopRecording();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        if (success) {
            auto stats = recorder->getStats();
            if (!opts.quiet) {
                std::cout << "[Video] Recording complete: " << outputPath << std::endl;
                std::cout << "[Video] Captured " << stats.capturedFrames << " frames in " << duration.count() << "ms" << std::endl;
                if (stats.droppedFrames > 0) {
                    std::cout << "[Video] Dropped " << stats.droppedFrames << " frames" << std::endl;
                }
            }
        }

        // Cleanup
        recorder.reset();
        runtime.reset();
        SDL_PumpEvents();

        std::cout.flush();
        std::cerr.flush();
        _exit(success ? 0 : 1);
    } else {
        // Normal mode: run main loop until quit
        // If debug server is enabled, we need to use a manual loop

        if (opts.debugPort > 0) {
            debugServer = std::make_unique<mystral::debug::DebugServer>(opts.debugPort);
            if (!debugServer->start()) {
                std::cerr << "Warning: Failed to start debug server on port " << opts.debugPort << std::endl;
                debugServer.reset();
            } else {
                // Set up command handler
                debugServer->setCommandHandler([&](int clientId, int requestId, const std::string& method, const std::string& params) -> std::string {
                    auto fail = [&](const std::string& message) {
                        debugServer->sendError(clientId, requestId, message);
                        return std::string{};
                    };

                    if (method == "hello" || method == "getCapabilities") {
                        return "{\"protocolVersion\":1,\"runtimeVersion\":\"" +
                            std::string(mystral::getVersion()) +
                            "\",\"agentBridgeVersion\":1,\"methods\":[\"screenshot\",\"keyboard\",\"mouse\",\"gamepad\","
                            "\"getFrameCount\",\"evaluate\",\"getLogs\",\"waitForFrame\",\"pause\",\"resume\",\"stepFrames\",\"quit\","
                            "\"agent.list\",\"agent.inspect\",\"agent.act\",\"profile.start\",\"profile.stop\",\"metrics.snapshot\"]}";
                    }

                    // Handle getFrameCount
                    if (method == "getFrameCount") {
                        return "{\"frame\":" + std::to_string(runtime->getFrameCount()) +
                            ",\"paused\":" + (runtime->isPaused() ? "true" : "false") + "}";
                    }

                    if (method == "pause") {
                        runtime->setPaused(true);
                        return "{\"frame\":" + std::to_string(runtime->getFrameCount()) + ",\"paused\":true}";
                    }

                    if (method == "resume") {
                        runtime->setPaused(false);
                        return "{\"frame\":" + std::to_string(runtime->getFrameCount()) + ",\"paused\":false}";
                    }

                    if (method == "quit") {
                        quitDelayPolls = 2;
                        return "{\"quitting\":true}";
                    }

                    if (method == "stepFrames") {
                        int count = static_cast<int>(extractJsonNumber(params, "count", 1));
                        if (count < 1) {
                            return fail("count must be at least 1");
                        }
                        uint64_t targetFrame = runtime->getFrameCount() + static_cast<uint64_t>(count);
                        runtime->stepFrames(static_cast<uint32_t>(count));
                        return "{\"targetFrame\":" + std::to_string(targetFrame) + ",\"paused\":true}";
                    }

                    if (method == "profile.start") {
                        if (debugProfilerActive) {
                            return fail("A runtime profile is already active");
                        }
                        const double framesValue = extractJsonNumber(params, "frames", 120);
                        if (framesValue < 1 || framesValue > 3600 ||
                            framesValue != static_cast<uint64_t>(framesValue)) {
                            return fail("frames must be an integer between 1 and 3600");
                        }
                        debugProfileExpectedFrames = static_cast<uint64_t>(framesValue);
                        runtime->startProfiler(debugProfileExpectedFrames);
                        debugProfilerActive = true;
                        uint64_t startFrame = runtime->getFrameCount();
                        return "{\"startFrame\":" + std::to_string(startFrame) +
                            // profile.start is handled inside the current frame,
                            // which is intentionally excluded by Runtime. Wait
                            // one extra frame to collect exactly N full frames.
                            ",\"targetFrame\":" + std::to_string(
                                startFrame + debugProfileExpectedFrames + 1) + "}";
                    }

                    if (method == "profile.stop") {
                        if (!debugProfilerActive) {
                            return fail("No runtime profile is active");
                        }
                        const mystral::RuntimeProfileReport report = runtime->stopProfiler();
                        debugProfilerActive = false;
                        return makeBenchmarkJson(
                            report,
                            static_cast<int>(debugProfileExpectedFrames),
                            0,
                            opts.scriptPath,
                            "null");
                    }

                    if (method == "metrics.snapshot") {
                        mystral::EvaluationResult evaluation = runtime->evaluateExpression(
                            "({runtime:globalThis.__mystralRuntimeStats?.() ?? null,"
                            "webgpu:globalThis.__mystralWebGpuStats?.() ?? null})");
                        if (!evaluation.success) {
                            return fail(evaluation.error);
                        }
                        return evaluation.valueJson;
                    }

                    // Handle screenshot
                    if (method == "screenshot") {
                        std::vector<uint8_t> frameData;
                        uint32_t width, height;
                        if (runtime->captureFrame(frameData, width, height)) {
                            // Encode to PNG
                            std::vector<uint8_t> pngData;
                            if (stbi_write_png_to_func(pngWriteCallback, &pngData, width, height, 4, frameData.data(), width * 4)) {
                                // Base64 encode
                                std::string base64 = base64Encode(pngData.data(), pngData.size());
                                return "{\"data\":\"" + base64 + "\",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}";
                            }
                            return fail("Failed to encode PNG");
                        }
                        return fail("Failed to capture frame");
                    }

                    // Handle keyboard.press, keyboard.down, keyboard.up, keyboard.type
                    if (method.rfind("keyboard.", 0) == 0) {
                        std::string subMethod = method.substr(9); // After "keyboard."
                        std::string keyName = extractJsonString(params, "key");

                        if (subMethod == "press") {
                            SDL_Scancode scancode = keyNameToScancode(keyName);
                            if (scancode == SDL_SCANCODE_UNKNOWN) {
                                return fail("Unknown key: " + keyName);
                            }
                            injectKeyboardEvent(scancode, true);
                            injectKeyboardEvent(scancode, false);
                            return "{}";
                        }
                        if (subMethod == "down") {
                            SDL_Scancode scancode = keyNameToScancode(keyName);
                            if (scancode == SDL_SCANCODE_UNKNOWN) {
                                return fail("Unknown key: " + keyName);
                            }
                            injectKeyboardEvent(scancode, true);
                            return "{}";
                        }
                        if (subMethod == "up") {
                            SDL_Scancode scancode = keyNameToScancode(keyName);
                            if (scancode == SDL_SCANCODE_UNKNOWN) {
                                return fail("Unknown key: " + keyName);
                            }
                            injectKeyboardEvent(scancode, false);
                            return "{}";
                        }
                        if (subMethod == "type") {
                            std::string text = extractJsonString(params, "text");
                            for (char c : text) {
                                std::string keyStr(1, c);
                                SDL_Scancode scancode = keyNameToScancode(keyStr);
                                if (scancode != SDL_SCANCODE_UNKNOWN) {
                                    injectKeyboardEvent(scancode, true);
                                    injectKeyboardEvent(scancode, false);
                                }
                            }
                            return "{}";
                        }
                        return fail("Unknown keyboard method: " + subMethod);
                    }

                    // Handle mouse.move, mouse.click, mouse.down, mouse.up
                    if (method.rfind("mouse.", 0) == 0) {
                        std::string subMethod = method.substr(6); // After "mouse."
                        float x = static_cast<float>(extractJsonNumber(params, "x", 0));
                        float y = static_cast<float>(extractJsonNumber(params, "y", 0));
                        std::string buttonStr = extractJsonString(params, "button");
                        int button = SDL_BUTTON_LEFT;
                        if (buttonStr == "right") button = SDL_BUTTON_RIGHT;
                        else if (buttonStr == "middle") button = SDL_BUTTON_MIDDLE;

                        if (subMethod == "move") {
                            injectMouseMotion(x, y);
                            return "{}";
                        }
                        if (subMethod == "click") {
                            injectMouseButton(x, y, button, true);
                            injectMouseButton(x, y, button, false);
                            return "{}";
                        }
                        if (subMethod == "down") {
                            injectMouseButton(x, y, button, true);
                            return "{}";
                        }
                        if (subMethod == "up") {
                            injectMouseButton(x, y, button, false);
                            return "{}";
                        }
                        return fail("Unknown mouse method: " + subMethod);
                    }

                    // Handle gamepad.press, gamepad.axis
                    if (method.rfind("gamepad.", 0) == 0) {
                        std::string subMethod = method.substr(8); // After "gamepad."

                        if (subMethod == "press") {
                            std::string buttonStr = extractJsonString(params, "button");
                            // Map button names to SDL gamepad button enum
                            SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
                            if (buttonStr == "A" || buttonStr == "a") button = SDL_GAMEPAD_BUTTON_SOUTH;
                            else if (buttonStr == "B" || buttonStr == "b") button = SDL_GAMEPAD_BUTTON_EAST;
                            else if (buttonStr == "X" || buttonStr == "x") button = SDL_GAMEPAD_BUTTON_WEST;
                            else if (buttonStr == "Y" || buttonStr == "y") button = SDL_GAMEPAD_BUTTON_NORTH;
                            else if (buttonStr == "LB" || buttonStr == "L1") button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
                            else if (buttonStr == "RB" || buttonStr == "R1") button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
                            else if (buttonStr == "Back" || buttonStr == "Select") button = SDL_GAMEPAD_BUTTON_BACK;
                            else if (buttonStr == "Start") button = SDL_GAMEPAD_BUTTON_START;
                            else if (buttonStr == "Guide" || buttonStr == "Home") button = SDL_GAMEPAD_BUTTON_GUIDE;
                            else if (buttonStr == "LS" || buttonStr == "L3") button = SDL_GAMEPAD_BUTTON_LEFT_STICK;
                            else if (buttonStr == "RS" || buttonStr == "R3") button = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
                            else if (buttonStr == "DPadUp") button = SDL_GAMEPAD_BUTTON_DPAD_UP;
                            else if (buttonStr == "DPadDown") button = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
                            else if (buttonStr == "DPadLeft") button = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
                            else if (buttonStr == "DPadRight") button = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

                            if (button == SDL_GAMEPAD_BUTTON_INVALID) {
                                return fail("Unknown gamepad button: " + buttonStr);
                            }

                            // Inject button press and release
                            SDL_Event event;
                            SDL_zero(event);
                            event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                            event.gbutton.button = button;
                            event.gbutton.down = true;
                            SDL_PushEvent(&event);

                            event.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
                            event.gbutton.down = false;
                            SDL_PushEvent(&event);
                            return "{}";
                        }
                        if (subMethod == "axis") {
                            std::string axisStr = extractJsonString(params, "axis");
                            float x = static_cast<float>(extractJsonNumber(params, "x", 0));
                            float y = static_cast<float>(extractJsonNumber(params, "y", 0));

                            SDL_GamepadAxis axisX = SDL_GAMEPAD_AXIS_INVALID;
                            SDL_GamepadAxis axisY = SDL_GAMEPAD_AXIS_INVALID;
                            if (axisStr == "leftStick" || axisStr == "left") {
                                axisX = SDL_GAMEPAD_AXIS_LEFTX;
                                axisY = SDL_GAMEPAD_AXIS_LEFTY;
                            } else if (axisStr == "rightStick" || axisStr == "right") {
                                axisX = SDL_GAMEPAD_AXIS_RIGHTX;
                                axisY = SDL_GAMEPAD_AXIS_RIGHTY;
                            }

                            if (axisX == SDL_GAMEPAD_AXIS_INVALID) {
                                return fail("Unknown gamepad axis: " + axisStr);
                            }

                            // Inject axis events (values are -32768 to 32767)
                            SDL_Event event;
                            SDL_zero(event);
                            event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                            event.gaxis.axis = axisX;
                            event.gaxis.value = static_cast<int16_t>(x * 32767);
                            SDL_PushEvent(&event);

                            event.gaxis.axis = axisY;
                            event.gaxis.value = static_cast<int16_t>(y * 32767);
                            SDL_PushEvent(&event);
                            return "{}";
                        }
                        return fail("Unknown gamepad method: " + subMethod);
                    }

                    // Handle waitForFrame - returns empty to signal async handling
                    if (method == "waitForFrame") {
                        double absoluteFrame = extractJsonNumber(params, "frame", -1);
                        int count = static_cast<int>(extractJsonNumber(params, "count", 1));
                        int timeoutMs = static_cast<int>(extractJsonNumber(params, "timeoutMs", 5000));
                        uint64_t currentFrame = runtime->getFrameCount();
                        uint64_t targetFrame = absoluteFrame >= 0
                            ? static_cast<uint64_t>(absoluteFrame)
                            : currentFrame + static_cast<uint64_t>(count > 0 ? count : 0);
                        if (targetFrame <= currentFrame) {
                            return "{\"frame\":" + std::to_string(currentFrame) + "}";
                        }
                        pendingFrameWaits.push_back({
                            clientId,
                            requestId,
                            targetFrame,
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 1)
                        });
                        return "";
                    }

                    // Handle evaluate - execute JS in the runtime
                    if (method == "evaluate") {
                        std::string expression = extractJsonString(params, "expression");
                        if (params.find("\"expression\"") == std::string::npos) {
                            return fail("Missing expression");
                        }
                        mystral::EvaluationResult evaluation = runtime->evaluateExpression(expression);
                        if (!evaluation.success) {
                            return fail(evaluation.error);
                        }
                        return evaluation.valueJson;
                    }

                    if (method == "getLogs") {
                        double sinceValue = extractJsonNumber(params, "since", 0);
                        uint64_t since = static_cast<uint64_t>(sinceValue > 0 ? sinceValue : 0);
                        std::string result = "{\"entries\":[";
                        bool first = true;
                        for (const auto& entry : debugLogs) {
                            if (entry.sequence <= since) continue;
                            if (!first) result += ",";
                            first = false;
                            result += "{\"sequence\":" + std::to_string(entry.sequence) +
                                ",\"type\":\"" + escapeJson(entry.type) +
                                "\",\"message\":\"" + escapeJson(entry.message) + "\"}";
                        }
                        result += "]}";
                        return result;
                    }

                    if (method.rfind("agent.", 0) == 0) {
                        mystral::EvaluationResult result = runtime->dispatchAgentCommand(
                            method.substr(6),
                            params.empty() ? "{}" : params);
                        if (!result.success) {
                            return fail(result.error);
                        }
                        return result.valueJson;
                    }

                    return fail("Unknown method: " + method);
                });

                if (!opts.quiet) {
                    std::cout << "[DebugServer] Listening on ws://127.0.0.1:" << opts.debugPort << std::endl;
                }
            }
        }

        if (debugServer) {
            // Manual loop with debug server
            uint64_t lastBroadcastFrame = runtime->getFrameCount();
            while (runtime->pollEvents()) {
                uint64_t currentFrame = runtime->getFrameCount();

                // Broadcast frameRendered event to connected clients
                if (currentFrame != lastBroadcastFrame && debugServer->getClientCount() > 0) {
                    debugServer->broadcastEvent("frameRendered", "{\"frame\":" + std::to_string(currentFrame) + "}");
                }
                lastBroadcastFrame = currentFrame;

                auto now = std::chrono::steady_clock::now();
                for (auto it = pendingFrameWaits.begin(); it != pendingFrameWaits.end();) {
                    if (currentFrame >= it->targetFrame) {
                        debugServer->sendResponse(
                            it->clientId,
                            it->requestId,
                            "{\"frame\":" + std::to_string(currentFrame) + "}");
                        it = pendingFrameWaits.erase(it);
                    } else if (now >= it->deadline) {
                        debugServer->sendError(it->clientId, it->requestId, "waitForFrame timed out");
                        it = pendingFrameWaits.erase(it);
                    } else {
                        ++it;
                    }
                }

                if (quitDelayPolls >= 0 && --quitDelayPolls <= 0) {
                    runtime->quit();
                    break;
                }

                // Small delay to prevent CPU spin
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Broadcast exit event
            int exitCode = runtime->getExitCode();
            debugServer->broadcastEvent("exit", "{\"code\":" + std::to_string(exitCode) + "}");
            debugServer->stop();
        } else {
            // Standard run loop (no debug server)
            runtime->run();
        }

        // Get exit code from process.exit() if called
        int exitCode = runtime->getExitCode();

        if (!opts.quiet) {
            std::cout << "=== Script finished ===" << std::endl;
        }

        // Note: On macOS, SDL3's audio callback threads can prevent graceful shutdown.
        // The CoreAudio subsystem sometimes blocks even _exit(). SIGKILL is the only
        // reliable way to terminate. This is safe because all user-visible state
        // (files, screenshots) has already been written.
        // TODO: File SDL3 issue about CoreAudio callback blocking process exit.
#ifdef __APPLE__
        // Give the audio callback a brief moment to notice shutdown
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // SIGKILL is the only reliable termination on macOS with audio
        // Note: We lose the exit code here, but this is the only reliable way to exit
        kill(getpid(), SIGKILL);
        // Unreachable, but suppresses compiler warning
        return exitCode;
#elif !defined(_WIN32)
        _exit(exitCode);
#else
        ExitProcess(exitCode);
#endif
    }

    return 0;
}

/**
 * Bake lightmaps for a scene.
 * Generates a JavaScript wrapper that invokes the TypeScript lightmap baker.
 */
static int bakeLightmaps(const CLIOptions& opts) {
    namespace fs = std::filesystem;

    if (opts.scriptPath.empty()) {
        std::cerr << "Error: No input file specified for bake." << std::endl;
        std::cerr << "Usage: mystral bake <input.glb|input.js> --output <dir>" << std::endl;
        return 1;
    }

    fs::path inputPath = opts.scriptPath;
    if (!fs::exists(inputPath)) {
        std::cerr << "Error: Input file not found: " << inputPath << std::endl;
        return 1;
    }

    // Determine output directory
    std::string outputDir = opts.outputPath.empty() ? "./lightmaps" : opts.outputPath;

    if (!opts.quiet) {
        std::cout << "=== Mystral Lightmap Baker ===" << std::endl;
        std::cout << "Input: " << inputPath << std::endl;
        std::cout << "Output: " << outputDir << std::endl;
        std::cout << "Resolution: " << opts.bakeResolution << std::endl;
        std::cout << "Samples: " << opts.bakeSamples << std::endl;
        std::cout << "Bounces: " << opts.bakeBounces << std::endl;
        std::cout << std::endl;
    }

    // Generate a temporary JavaScript file that loads the scene and bakes
    std::string extension = inputPath.extension().string();
    bool isGLB = (extension == ".glb" || extension == ".GLB" ||
                  extension == ".gltf" || extension == ".GLTF");

    std::string bakerScript;
    if (isGLB) {
        // Generate script to load GLB and bake
        bakerScript = R"(
import { Engine } from 'mystral';
import { GLBLoader } from 'mystral/loaders/GLBLoader';
import { LightmapBaker } from 'mystral/tools/lightmap-baker';

async function main() {
    console.log('[Bake] Starting lightmap bake...');

    // Initialize engine in headless mode
    const engine = new Engine({ headless: true, width: 1, height: 1 });
    await engine.init();

    // Load GLB
    const loader = new GLBLoader(engine.device);
    const result = await loader.load(')" + inputPath.string() + R"(');

    console.log('[Bake] Scene loaded:', result.rootNode.name);

    // Create baker and bake
    const baker = new LightmapBaker(engine.device);
    const bakeResult = await baker.bake({
        scene: result.rootNode,
        resolution: )" + std::to_string(opts.bakeResolution) + R"(,
        samples: )" + std::to_string(opts.bakeSamples) + R"(,
        bounces: )" + std::to_string(opts.bakeBounces) + R"(,
        onProgress: (progress, message) => {
            console.log(`[Bake] ${Math.round(progress * 100)}% - ${message}`);
        },
    });

    // Save results
    await bakeResult.save(')" + outputDir + R"(');

    console.log('[Bake] Complete! Lightmaps saved to: )" + outputDir + R"(');
    console.log('[Bake] Manifest:', JSON.stringify(bakeResult.manifest, null, 2));

    process.exit(0);
}

main().catch(err => {
    console.error('[Bake] Error:', err);
    process.exit(1);
});
)";
    } else {
        // Input is a JS file - execute it directly but inject baker context
        std::ifstream inputFile(inputPath);
        if (!inputFile.is_open()) {
            std::cerr << "Error: Cannot read input file: " << inputPath << std::endl;
            return 1;
        }
        std::stringstream buffer;
        buffer << inputFile.rdbuf();
        std::string userScript = buffer.str();

        bakerScript = R"(
import { LightmapBaker } from 'mystral/tools/lightmap-baker';

// User's scene setup script
)" + userScript + R"(

// Bake function injected by CLI
async function __mystralBake(scene) {
    const baker = new LightmapBaker();
    const bakeResult = await baker.bake({
        scene: scene,
        resolution: )" + std::to_string(opts.bakeResolution) + R"(,
        samples: )" + std::to_string(opts.bakeSamples) + R"(,
        bounces: )" + std::to_string(opts.bakeBounces) + R"(,
        onProgress: (progress, message) => {
            console.log(`[Bake] ${Math.round(progress * 100)}% - ${message}`);
        },
    });

    await bakeResult.save(')" + outputDir + R"(');
    console.log('[Bake] Complete! Lightmaps saved to: )" + outputDir + R"(');
}

// Export for use by scene script
globalThis.__mystralBake = __mystralBake;
)";
    }

    // Write temporary script
    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec) {
        std::cerr << "Error: Cannot get temp directory" << std::endl;
        return 1;
    }

    fs::path tempScript = tempDir / ("mystral-bake-" + std::to_string(std::time(nullptr)) + ".js");
    std::ofstream tempFile(tempScript);
    if (!tempFile.is_open()) {
        std::cerr << "Error: Cannot create temp script file" << std::endl;
        return 1;
    }
    tempFile << bakerScript;
    tempFile.close();

    if (!opts.quiet) {
        std::cout << "[Bake] Executing baker script..." << std::endl;
    }

    // Create runtime and execute the bake script
    mystral::RuntimeConfig config;
    config.width = 1;
    config.height = 1;
    config.title = "Mystral Lightmap Baker";
    config.noSdl = true;  // No window needed for baking
    config.debug = opts.debug;

    auto runtime = mystral::Runtime::create(config);
    if (!runtime) {
        std::cerr << "Error: Failed to create runtime!" << std::endl;
        fs::remove(tempScript, ec);
        return 1;
    }

    // Load and execute the baker script
    if (!runtime->loadScript(tempScript.string())) {
        std::cerr << "Error: Failed to execute baker script!" << std::endl;
        fs::remove(tempScript, ec);
        return 1;
    }

    // Run until complete
    while (runtime->pollEvents()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int exitCode = runtime->getExitCode();

    // Cleanup temp file
    fs::remove(tempScript, ec);

    if (!opts.quiet && exitCode == 0) {
        std::cout << "=== Bake complete ===" << std::endl;
    }

    return exitCode;
}

int main(int argc, char* argv[]) {
    CLIOptions opts = parseArgs(argc, argv);
    std::string embeddedEntry = mystral::vfs::getEmbeddedEntryPath();

    // Handle --version
    if (opts.showVersion) {
        printVersion();
        return 0;
    }

    // Handle --help
    if (opts.showHelp) {
        printHelp();
        return 0;
    }

    // If we have an embedded entry and no explicit command, treat it as run
    if (opts.command.empty() && !embeddedEntry.empty()) {
        opts.command = "run";
        opts.scriptPath = embeddedEntry;
    }

    // Handle no args with no embedded entry
    if (opts.command.empty() && argc < 2) {
        printHelp();
        return 1;
    }

    // Handle 'compile' command
    if (opts.command == "compile") {
        return compileBundle(opts);
    }

    // Handle 'bake' command
    if (opts.command == "bake") {
        if (opts.scriptPath.empty()) {
            std::cerr << "Error: No input file specified for bake." << std::endl;
            std::cerr << "Usage: mystral bake <input.glb|input.js> --output <dir>" << std::endl;
            return 1;
        }
        return bakeLightmaps(opts);
    }

    // Handle 'run' command
    if (opts.command == "run") {
        if (opts.scriptPath.empty()) {
            if (!embeddedEntry.empty()) {
                opts.scriptPath = embeddedEntry;
            } else {
                std::cerr << "Error: No script file specified." << std::endl;
                std::cerr << "Usage: mystral run <script.js>" << std::endl;
                return 1;
            }
        }
        return runScript(opts);
    }

    // Unknown command
    std::cerr << "Error: Unknown command or missing arguments." << std::endl;
    printHelp();
    return 1;
}
