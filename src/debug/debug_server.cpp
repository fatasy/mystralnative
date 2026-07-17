/**
 * Debug Server Implementation
 *
 * Minimal WebSocket server using libuv for remote testing and debugging.
 */

#include "mystral/debug/debug_server.h"
#include "mystral/async/event_loop.h"
#include <uv.h>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <list>
#include <cstring>
#include <algorithm>
#include <array>
#include <cctype>

// ============================================================================
// Simple SHA1 implementation (for WebSocket handshake)
// Based on RFC 3174
// ============================================================================

namespace {

class SHA1 {
public:
    SHA1() { reset(); }

    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            buffer_[bufferIndex_++] = data[i];
            if (bufferIndex_ == 64) {
                processBlock();
                bitCount_ += 512;
                bufferIndex_ = 0;
            }
        }
    }

    void update(const std::string& str) {
        update(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    }

    std::array<uint8_t, 20> final() {
        uint64_t totalBits = bitCount_ + bufferIndex_ * 8;

        // Padding
        buffer_[bufferIndex_++] = 0x80;
        while (bufferIndex_ != 56) {
            if (bufferIndex_ == 64) {
                processBlock();
                bufferIndex_ = 0;
            }
            buffer_[bufferIndex_++] = 0;
        }

        // Append length in bits (big-endian)
        for (int i = 7; i >= 0; i--) {
            buffer_[bufferIndex_++] = static_cast<uint8_t>((totalBits >> (i * 8)) & 0xFF);
        }
        processBlock();

        // Output hash (big-endian)
        std::array<uint8_t, 20> hash;
        for (int i = 0; i < 5; i++) {
            hash[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFF);
            hash[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFF);
            hash[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFF);
            hash[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFF);
        }
        return hash;
    }

private:
    void reset() {
        h_[0] = 0x67452301;
        h_[1] = 0xEFCDAB89;
        h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476;
        h_[4] = 0xC3D2E1F0;
        bufferIndex_ = 0;
        bitCount_ = 0;
    }

    static uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    void processBlock() {
        uint32_t w[80];

        // Prepare message schedule
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(buffer_[i * 4]) << 24) |
                   (static_cast<uint32_t>(buffer_[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(buffer_[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(buffer_[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint8_t buffer_[64];
    size_t bufferIndex_;
    uint64_t bitCount_;
};

// Base64 encoding
std::string base64Encode(const uint8_t* data, size_t len) {
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

}  // anonymous namespace

// ============================================================================

namespace mystral {
namespace debug {

// WebSocket frame opcodes
enum class WSOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

/**
 * Connected client state
 */
struct DebugClient {
    uv_tcp_t* socket = nullptr;
    bool handshakeComplete = false;
    std::string receiveBuffer;
    std::string frameBuffer;  // Partial WebSocket frame
    int id = 0;

    // Pending requests waiting for response
    std::unordered_map<int, bool> pendingRequests;
};

/**
 * Write request with associated data
 */
struct WriteReq {
    uv_write_t req;
    uv_buf_t buf;
    char* data;
};

/**
 * Debug Server Implementation
 */
class DebugServerImpl {
public:
    DebugServerImpl(int port) : port_(port), running_(false), nextClientId_(1) {}

    ~DebugServerImpl() {
        stop();
    }

    bool start() {
        if (running_) return true;

        // Get the shared libuv event loop (same as the rest of the runtime)
        loop_ = mystral::async::EventLoop::instance().handle();
        if (!loop_) {
            std::cerr << "[DebugServer] Failed to get libuv loop (not initialized?)" << std::endl;
            return false;
        }

        // Initialize TCP handle
        server_ = new uv_tcp_t;
        uv_tcp_init(loop_, server_);
        server_->data = this;

        // Bind to port
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", port_, &addr);

        int r = uv_tcp_bind(server_, (const struct sockaddr*)&addr, 0);
        if (r != 0) {
            std::cerr << "[DebugServer] Bind failed: " << uv_strerror(r) << std::endl;
            delete server_;
            server_ = nullptr;
            return false;
        }

        // Start listening
        r = uv_listen((uv_stream_t*)server_, 128, onConnection);
        if (r != 0) {
            std::cerr << "[DebugServer] Listen failed: " << uv_strerror(r) << std::endl;
            delete server_;
            server_ = nullptr;
            return false;
        }

        running_ = true;
        std::cout << "[DebugServer] Listening on ws://127.0.0.1:" << port_ << std::endl;
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        // Close all clients
        for (auto& client : clients_) {
            if (client->socket) {
                uv_close((uv_handle_t*)client->socket, onClose);
            }
        }
        clients_.clear();

        // Close server
        if (server_) {
            uv_close((uv_handle_t*)server_, onClose);
            server_ = nullptr;
        }

        std::cout << "[DebugServer] Stopped" << std::endl;
    }

    bool isRunning() const { return running_; }

    void poll() {
        // libuv event processing is done by the main event loop
        // This is called from the main loop but events are processed via callbacks
    }

    void setCommandHandler(CommandHandler handler) {
        commandHandler_ = handler;
    }

    void broadcastEvent(const std::string& eventName, const std::string& params) {
        std::string json = "{\"event\":\"" + eventName + "\",\"params\":" + params + "}";
        for (auto& client : clients_) {
            if (client->handshakeComplete) {
                sendWebSocketFrame(client.get(), json);
            }
        }
    }

    void sendResponse(int clientId, int requestId, const std::string& result) {
        std::string json = "{\"id\":" + std::to_string(requestId) + ",\"result\":" + result + "}";
        for (auto& client : clients_) {
            if (client->id == clientId && client->pendingRequests.count(requestId)) {
                client->pendingRequests.erase(requestId);
                sendWebSocketFrame(client.get(), json);
                return;
            }
        }
    }

    void sendError(int clientId, int requestId, const std::string& errorMessage) {
        std::string json = "{\"id\":" + std::to_string(requestId) + ",\"error\":{\"message\":\"" + escapeJson(errorMessage) + "\"}}";
        for (auto& client : clients_) {
            if (client->id == clientId && client->pendingRequests.count(requestId)) {
                client->pendingRequests.erase(requestId);
                sendWebSocketFrame(client.get(), json);
                return;
            }
        }
    }

    int getClientCount() const {
        int count = 0;
        for (const auto& client : clients_) {
            if (client->handshakeComplete) count++;
        }
        return count;
    }

    int getPort() const { return port_; }

private:
    static void onConnection(uv_stream_t* server, int status) {
        auto* self = static_cast<DebugServerImpl*>(server->data);
        if (status < 0) {
            std::cerr << "[DebugServer] Connection error: " << uv_strerror(status) << std::endl;
            return;
        }

        auto client = std::make_unique<DebugClient>();
        client->socket = new uv_tcp_t;
        client->id = self->nextClientId_++;
        uv_tcp_init(self->loop_, client->socket);
        client->socket->data = self;

        if (uv_accept(server, (uv_stream_t*)client->socket) == 0) {
            std::cout << "[DebugServer] Client " << client->id << " connected" << std::endl;
            uv_read_start((uv_stream_t*)client->socket, onAlloc, onRead);
            self->clients_.push_back(std::move(client));
        } else {
            uv_close((uv_handle_t*)client->socket, onClose);
        }
    }

    static void onAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        buf->base = new char[suggested_size];
        buf->len = suggested_size;
    }

    static void onRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<DebugServerImpl*>(stream->data);

        if (nread < 0) {
            if (nread != UV_EOF) {
                std::cerr << "[DebugServer] Read error: " << uv_strerror(nread) << std::endl;
            }
            self->removeClient((uv_tcp_t*)stream);
            delete[] buf->base;
            return;
        }

        if (nread == 0) {
            delete[] buf->base;
            return;
        }

        // Find the client
        DebugClient* client = nullptr;
        for (auto& c : self->clients_) {
            if (c->socket == (uv_tcp_t*)stream) {
                client = c.get();
                break;
            }
        }

        if (!client) {
            delete[] buf->base;
            return;
        }

        // Add to receive buffer
        client->receiveBuffer.append(buf->base, nread);
        delete[] buf->base;

        // Process data
        if (!client->handshakeComplete) {
            self->processHandshake(client);
        } else {
            self->processWebSocketFrames(client);
        }
    }

    static void onClose(uv_handle_t* handle) {
        delete handle;
    }

    static void onWrite(uv_write_t* req, int status) {
        auto* writeReq = reinterpret_cast<WriteReq*>(req);
        delete[] writeReq->data;
        delete writeReq;
    }

    void removeClient(uv_tcp_t* socket) {
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if ((*it)->socket == socket) {
                std::cout << "[DebugServer] Client " << (*it)->id << " disconnected" << std::endl;
                uv_close((uv_handle_t*)socket, onClose);
                (*it)->socket = nullptr;
                clients_.erase(it);
                return;
            }
        }
    }

    void processHandshake(DebugClient* client) {
        // Look for end of HTTP headers
        size_t headerEnd = client->receiveBuffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            return;  // Incomplete headers
        }

        std::string headers = client->receiveBuffer.substr(0, headerEnd);
        client->receiveBuffer.erase(0, headerEnd + 4);

        // Parse Sec-WebSocket-Key
        std::string wsKey;
        std::string lowerHeaders = headers;
        std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        size_t keyPos = lowerHeaders.find("sec-websocket-key:");
        if (keyPos != std::string::npos) {
            size_t start = keyPos + 18;
            while (start < headers.size() && headers[start] == ' ') start++;
            size_t end = headers.find("\r\n", start);
            if (end == std::string::npos) end = headers.size();
            wsKey = headers.substr(start, end - start);
        }

        if (wsKey.empty()) {
            std::cerr << "[DebugServer] Invalid WebSocket handshake (no key)" << std::endl;
            removeClient(client->socket);
            return;
        }

        // Generate accept key
        std::string acceptKey = generateAcceptKey(wsKey);

        // Send handshake response
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
            "\r\n";

        sendRaw(client, response);
        client->handshakeComplete = true;
        std::cout << "[DebugServer] Client " << client->id << " WebSocket handshake complete" << std::endl;
    }

    void processWebSocketFrames(DebugClient* client) {
        while (client->receiveBuffer.size() >= 2) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(client->receiveBuffer.data());

            bool fin = (data[0] & 0x80) != 0;
            WSOpcode opcode = static_cast<WSOpcode>(data[0] & 0x0F);
            bool masked = (data[1] & 0x80) != 0;
            uint64_t payloadLen = data[1] & 0x7F;

            size_t headerLen = 2;
            if (payloadLen == 126) {
                if (client->receiveBuffer.size() < 4) return;
                payloadLen = (static_cast<uint16_t>(data[2]) << 8) | data[3];
                headerLen = 4;
            } else if (payloadLen == 127) {
                if (client->receiveBuffer.size() < 10) return;
                payloadLen = 0;
                for (int i = 0; i < 8; i++) {
                    payloadLen = (payloadLen << 8) | data[2 + i];
                }
                headerLen = 10;
            }

            if (masked) {
                headerLen += 4;
            }

            if (client->receiveBuffer.size() < headerLen + payloadLen) {
                return;  // Incomplete frame
            }

            // Extract payload
            std::string payload;
            payload.resize(payloadLen);
            const uint8_t* maskKey = masked ? data + (headerLen - 4) : nullptr;
            const uint8_t* payloadData = data + headerLen;

            for (uint64_t i = 0; i < payloadLen; i++) {
                if (masked) {
                    payload[i] = payloadData[i] ^ maskKey[i % 4];
                } else {
                    payload[i] = payloadData[i];
                }
            }

            // Remove frame from buffer
            client->receiveBuffer.erase(0, headerLen + payloadLen);

            // Handle frame
            switch (opcode) {
                case WSOpcode::Text:
                case WSOpcode::Binary:
                    if (fin) {
                        client->frameBuffer += payload;
                        handleMessage(client, client->frameBuffer);
                        client->frameBuffer.clear();
                    } else {
                        client->frameBuffer += payload;
                    }
                    break;

                case WSOpcode::Continuation:
                    client->frameBuffer += payload;
                    if (fin) {
                        handleMessage(client, client->frameBuffer);
                        client->frameBuffer.clear();
                    }
                    break;

                case WSOpcode::Close:
                    removeClient(client->socket);
                    return;

                case WSOpcode::Ping:
                    sendPong(client, payload);
                    break;

                case WSOpcode::Pong:
                    // Ignore pongs
                    break;
            }
        }
    }

    void handleMessage(DebugClient* client, const std::string& message) {
        // Parse JSON message
        // Simple parsing - extract id, method, params
        int requestId = 0;
        std::string method;
        std::string params = "{}";

        // Extract "id"
        size_t idPos = message.find("\"id\"");
        if (idPos != std::string::npos) {
            size_t colonPos = message.find(':', idPos);
            if (colonPos != std::string::npos) {
                size_t start = colonPos + 1;
                while (start < message.size() && (message[start] == ' ' || message[start] == '\t')) start++;
                size_t end = start;
                while (end < message.size() && (message[end] >= '0' && message[end] <= '9')) end++;
                if (end > start) {
                    requestId = std::stoi(message.substr(start, end - start));
                }
            }
        }

        // Extract "method"
        size_t methodPos = message.find("\"method\"");
        if (methodPos != std::string::npos) {
            size_t colonPos = message.find(':', methodPos);
            if (colonPos != std::string::npos) {
                size_t quoteStart = message.find('"', colonPos);
                if (quoteStart != std::string::npos) {
                    size_t quoteEnd = message.find('"', quoteStart + 1);
                    if (quoteEnd != std::string::npos) {
                        method = message.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                    }
                }
            }
        }

        // Extract "params"
        size_t paramsPos = message.find("\"params\"");
        if (paramsPos != std::string::npos) {
            size_t colonPos = message.find(':', paramsPos);
            if (colonPos != std::string::npos) {
                size_t start = colonPos + 1;
                while (start < message.size() && (message[start] == ' ' || message[start] == '\t')) start++;
                if (start < message.size()) {
                    // Find matching brace/bracket
                    char openChar = message[start];
                    char closeChar = (openChar == '{') ? '}' : (openChar == '[') ? ']' : '\0';
                    if (closeChar) {
                        int depth = 1;
                        bool inString = false;
                        bool escaped = false;
                        size_t end = start + 1;
                        while (end < message.size() && depth > 0) {
                            char current = message[end];
                            if (inString) {
                                if (escaped) {
                                    escaped = false;
                                } else if (current == '\\') {
                                    escaped = true;
                                } else if (current == '"') {
                                    inString = false;
                                }
                            } else if (current == '"') {
                                inString = true;
                            } else if (current == openChar) {
                                depth++;
                            } else if (current == closeChar) {
                                depth--;
                            }
                            end++;
                        }
                        params = message.substr(start, end - start);
                    }
                }
            }
        }

        if (method.empty()) {
            sendError(client->id, requestId, "Missing method");
            return;
        }

        // Track pending request
        client->pendingRequests[requestId] = true;

        // Call command handler
        if (commandHandler_) {
            std::string result = commandHandler_(client->id, requestId, method, params);
            if (!result.empty()) {
                // Immediate response
                client->pendingRequests.erase(requestId);
                std::string response = "{\"id\":" + std::to_string(requestId) + ",\"result\":" + result + "}";
                sendWebSocketFrame(client, response);
            }
            // If result is empty, handler will call sendResponse/sendError later
        } else {
            sendError(client->id, requestId, "No command handler");
        }
    }

    void sendRaw(DebugClient* client, const std::string& data) {
        auto* req = new WriteReq;
        req->data = new char[data.size()];
        memcpy(req->data, data.data(), data.size());
        req->buf = uv_buf_init(req->data, data.size());
        uv_write(&req->req, (uv_stream_t*)client->socket, &req->buf, 1, onWrite);
    }

    void sendWebSocketFrame(DebugClient* client, const std::string& payload) {
        std::vector<uint8_t> frame;

        // FIN + Text opcode
        frame.push_back(0x81);

        // Payload length (server doesn't mask)
        if (payload.size() < 126) {
            frame.push_back(static_cast<uint8_t>(payload.size()));
        } else if (payload.size() < 65536) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--) {
                frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
            }
        }

        // Payload
        frame.insert(frame.end(), payload.begin(), payload.end());

        auto* req = new WriteReq;
        req->data = new char[frame.size()];
        memcpy(req->data, frame.data(), frame.size());
        req->buf = uv_buf_init(req->data, frame.size());
        uv_write(&req->req, (uv_stream_t*)client->socket, &req->buf, 1, onWrite);
    }

    void sendPong(DebugClient* client, const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(0x8A);  // FIN + Pong
        frame.push_back(static_cast<uint8_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());

        auto* req = new WriteReq;
        req->data = new char[frame.size()];
        memcpy(req->data, frame.data(), frame.size());
        req->buf = uv_buf_init(req->data, frame.size());
        uv_write(&req->req, (uv_stream_t*)client->socket, &req->buf, 1, onWrite);
    }

    std::string generateAcceptKey(const std::string& key) {
        // Concatenate with magic GUID (WebSocket protocol requirement)
        std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // SHA1 hash
        SHA1 sha;
        sha.update(combined);
        auto hash = sha.final();

        // Base64 encode
        return base64Encode(hash.data(), hash.size());
    }

    std::string escapeJson(const std::string& str) {
        std::string result;
        for (char c : str) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    int port_;
    bool running_;
    uv_loop_t* loop_ = nullptr;
    uv_tcp_t* server_ = nullptr;
    std::list<std::unique_ptr<DebugClient>> clients_;
    CommandHandler commandHandler_;
    int nextClientId_;
};

// DebugServer implementation
DebugServer::DebugServer(int port) : impl_(std::make_unique<DebugServerImpl>(port)) {}
DebugServer::~DebugServer() = default;

bool DebugServer::start() { return impl_->start(); }
void DebugServer::stop() { impl_->stop(); }
bool DebugServer::isRunning() const { return impl_->isRunning(); }
void DebugServer::poll() { impl_->poll(); }
void DebugServer::setCommandHandler(CommandHandler handler) { impl_->setCommandHandler(handler); }
void DebugServer::broadcastEvent(const std::string& eventName, const std::string& params) { impl_->broadcastEvent(eventName, params); }
void DebugServer::sendResponse(int clientId, int requestId, const std::string& result) { impl_->sendResponse(clientId, requestId, result); }
void DebugServer::sendError(int clientId, int requestId, const std::string& errorMessage) { impl_->sendError(clientId, requestId, errorMessage); }
int DebugServer::getClientCount() const { return impl_->getClientCount(); }
int DebugServer::getPort() const { return impl_->getPort(); }

}  // namespace debug
}  // namespace mystral
