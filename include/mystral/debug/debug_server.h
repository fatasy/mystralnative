#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

namespace mystral {
namespace debug {

/**
 * Debug Server for Remote Testing
 *
 * Provides a WebSocket server that allows external tools (like Playwright-style
 * test frameworks) to control and inspect running MystralNative games.
 *
 * Protocol: JSON messages over WebSocket
 *
 * Commands (client -> server):
 *   { "id": 1, "method": "screenshot", "params": { "format": "png" } }
 *   { "id": 2, "method": "keyboard.press", "params": { "key": "Enter" } }
 *   { "id": 3, "method": "waitForFrame", "params": { "count": 60 } }
 *   { "id": 4, "method": "evaluate", "params": { "expression": "window.score" } }
 *   { "id": 5, "method": "getFrameCount" }
 *   { "id": 6, "method": "pause" }
 *   { "id": 7, "method": "stepFrames", "params": { "count": 1 } }
 *   { "id": 8, "method": "getLogs", "params": { "since": 0 } }
 *
 * Responses (server -> client):
 *   { "id": 1, "result": { "data": "base64..." } }
 *   { "id": 2, "result": {} }
 *   { "id": 3, "error": { "message": "Timeout" } }
 *
 * Events (server -> client):
 *   { "event": "frameRendered", "params": { "frame": 1234 } }
 *   { "event": "console", "params": { "type": "log", "message": "Hello" } }
 *   { "event": "exit", "params": { "code": 0 } }
 */

// Forward declarations
struct DebugClient;
class DebugServerImpl;

/**
 * Command handler callback
 * @param clientId The connected client identifier
 * @param requestId The client-provided request identifier
 * @param method The command method name
 * @param params JSON string of parameters
 * @return JSON string of result, or empty string for async handling
 */
using CommandHandler = std::function<std::string(int clientId, int requestId,
                                                 const std::string& method,
                                                 const std::string& params)>;

/**
 * Debug Server
 */
class DebugServer {
public:
    /**
     * Create a debug server
     * @param port Port to listen on
     */
    explicit DebugServer(int port);
    ~DebugServer();

    /**
     * Start the server (non-blocking)
     * @return true on success
     */
    bool start();

    /**
     * Stop the server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool isRunning() const;

    /**
     * Process pending events (call from main loop)
     * This is integrated with libuv event loop
     */
    void poll();

    /**
     * Set command handler
     * The handler will be called for each command received from clients
     */
    void setCommandHandler(CommandHandler handler);

    /**
     * Send an event to all connected clients
     * @param eventName Event name (e.g., "frameRendered", "console")
     * @param params JSON string of parameters
     */
    void broadcastEvent(const std::string& eventName, const std::string& params);

    /**
     * Send a response to a specific request
     * @param requestId The request ID
     * @param result JSON string of result
     */
    void sendResponse(int clientId, int requestId, const std::string& result);

    /**
     * Send an error response to a specific request
     * @param requestId The request ID
     * @param errorMessage Error message
     */
    void sendError(int clientId, int requestId, const std::string& errorMessage);

    /**
     * Get the number of connected clients
     */
    int getClientCount() const;

    /**
     * Get the port the server is listening on
     */
    int getPort() const;

private:
    std::unique_ptr<DebugServerImpl> impl_;
};

}  // namespace debug
}  // namespace mystral
