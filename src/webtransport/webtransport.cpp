/**
 * WebTransport API implementation (client) over quiche QUIC + HTTP/3.
 *
 * See include/mystral/webtransport/webtransport.h for the high-level design.
 */

#include "mystral/webtransport/webtransport.h"

#include <iostream>

// Full implementation requires quiche. The QUIC UDP socket and timers are driven
// directly on the runtime's per-frame poll loop using raw non-blocking sockets
// (no libuv), so WebTransport works on every platform — desktop and mobile.
// Otherwise we fall back to a stub that makes WebTransport construction reject.
#if defined(MYSTRAL_HAS_QUICHE)

#include "mystral/js/engine.h"

#include <quiche.h>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // prevent windows.h from defining min()/max() macros
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mystral {
namespace webtransport {

namespace {

// ---------------------------------------------------------------------------
// Cross-platform raw UDP socket helpers (no libuv)
// ---------------------------------------------------------------------------

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(socket_t s) { ::closesocket(s); }
inline bool setSocketNonBlocking(socket_t s) {
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void closeSocket(socket_t s) { ::close(s); }
inline bool setSocketNonBlocking(socket_t s) {
    int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

// ---------------------------------------------------------------------------
// WebTransport / HTTP/3 wire constants
// ---------------------------------------------------------------------------

// Signal frame type prepended to a client-initiated bidirectional WT stream.
constexpr uint64_t WT_STREAM_BIDI_SIGNAL = 0x41;
// Stream type prepended to a unidirectional WT stream.
constexpr uint64_t WT_STREAM_UNI_SIGNAL = 0x54;

// HTTP/3 unidirectional stream types that belong to the h3 layer (must not be
// treated as WebTransport streams).
constexpr uint64_t H3_CONTROL_STREAM_TYPE = 0x00;
constexpr uint64_t H3_PUSH_STREAM_TYPE = 0x01;
constexpr uint64_t H3_QPACK_ENCODER_STREAM_TYPE = 0x02;
constexpr uint64_t H3_QPACK_DECODER_STREAM_TYPE = 0x03;

// WebTransport HTTP/3 SETTINGS (advertised to the server so it accepts the
// extended CONNECT). Multiple draft identifiers are sent for compatibility.
constexpr uint64_t SETTINGS_WEBTRANSPORT_MAX_SESSIONS_DRAFT = 0x2b603742;  // draft-02 ENABLE_WEBTRANSPORT
constexpr uint64_t SETTINGS_WT_MAX_SESSIONS = 0xc671706a;                  // draft-07+ WT_MAX_SESSIONS

constexpr size_t MAX_DATAGRAM_SIZE = 1350;
constexpr size_t STREAM_READ_CHUNK = 64 * 1024;

// ---------------------------------------------------------------------------
// Varint helpers (QUIC variable-length integer encoding, RFC 9000 §16)
// ---------------------------------------------------------------------------

void varintEncode(std::vector<uint8_t>& out, uint64_t v) {
    if (v <= 63) {
        out.push_back(static_cast<uint8_t>(v));
    } else if (v <= 16383) {
        out.push_back(static_cast<uint8_t>(0x40 | (v >> 8)));
        out.push_back(static_cast<uint8_t>(v & 0xff));
    } else if (v <= 1073741823ull) {
        out.push_back(static_cast<uint8_t>(0x80 | (v >> 24)));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(v & 0xff));
    } else {
        out.push_back(static_cast<uint8_t>(0xc0 | (v >> 56)));
        out.push_back(static_cast<uint8_t>((v >> 48) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 40) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 32) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(v & 0xff));
    }
}

// Decodes a varint from buf at offset. Returns the number of bytes consumed, or
// 0 if there are not enough bytes available yet.
size_t varintDecode(const uint8_t* buf, size_t len, uint64_t* out) {
    if (len == 0) return 0;
    uint8_t first = buf[0];
    size_t length = 1u << (first >> 6);  // 1, 2, 4 or 8
    if (len < length) return 0;
    uint64_t v = first & 0x3f;
    for (size_t i = 1; i < length; i++) {
        v = (v << 8) | buf[i];
    }
    *out = v;
    return length;
}

// ---------------------------------------------------------------------------
// Event queue: native -> JS, drained on the main thread in processEvents().
// ---------------------------------------------------------------------------

enum class EventType {
    Ready,
    Closed,       // graceful / remote close (message = reason)
    Error,        // failure before/around ready (message = reason)
    Datagram,     // data
    IncomingUni,  // streamId
    IncomingBidi, // streamId
    StreamData,   // streamId, data, fin
    StreamReset,  // streamId, code
};

struct Event {
    uint32_t sessionId = 0;
    EventType type;
    int64_t streamId = -1;
    uint64_t code = 0;
    bool fin = false;
    std::vector<uint8_t> data;
    std::string message;
};

std::queue<Event> g_events;

// ---------------------------------------------------------------------------
// Per-stream parse state
// ---------------------------------------------------------------------------

struct StreamState {
    bool serverInitiated = false;
    bool isUni = false;
    // For server-initiated streams we must consume the WT signal frame + session
    // id before delivering payload. h3-owned streams are drained and ignored.
    bool headerConsumed = false;
    bool isH3Owned = false;   // server control/qpack stream — drain & ignore
    bool announced = false;   // IncomingUni/IncomingBidi already emitted
    bool finDelivered = false;
    std::vector<uint8_t> pending;  // inbound bytes awaiting header parse

    // Outbound buffering. quiche's stream_send can return Done when the
    // congestion-control window is momentarily exhausted, so writes (and the
    // initial WT signal frame) are buffered here and flushed by pumpStreamSends()
    // whenever capacity becomes available.
    bool isOutgoing = false;
    std::vector<uint8_t> outBuf;
    bool outFin = false;   // a FIN has been requested by the writer
    bool finSent = false;  // the FIN has been flushed to quiche
    bool sendError = false;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

struct Session {
    uint32_t id = 0;

    // Raw non-blocking UDP socket; polled each frame in pumpSocket(). The QUIC
    // loss/idle timers are tracked with steady_clock (no libuv timer handle).
    socket_t sock = kInvalidSocket;
    std::chrono::steady_clock::time_point timeoutAt{};
    bool hasTimeout = false;

    quiche_conn* conn = nullptr;
    quiche_h3_conn* h3 = nullptr;
    quiche_config* config = nullptr;
    quiche_h3_config* h3config = nullptr;

    struct sockaddr_storage peer{};
    socklen_t peerLen = 0;
    struct sockaddr_storage local{};
    socklen_t localLen = 0;

    std::string host;
    int port = 0;
    std::string path;

    bool established = false;  // QUIC handshake complete
    bool h3Created = false;
    bool wtReady = false;      // CONNECT 200 received
    bool reportedReady = false;
    bool reportedClosed = false;
    bool failed = false;
    bool wantClose = false;    // teardown requested

    int64_t connectStreamId = -1;

    // Next client-initiated stream ids for WT data streams. h3 uses client bidi
    // 0 (the CONNECT request) and client uni 2/6/10 (control + qpack), so WT
    // streams start beyond those.
    uint64_t nextClientBidi = 4;
    uint64_t nextClientUni = 14;

    std::map<uint64_t, StreamState> streams;

    ~Session() {
        if (sock != kInvalidSocket) closeSocket(sock);
        if (h3) quiche_h3_conn_free(h3);
        if (conn) quiche_conn_free(conn);
        if (h3config) quiche_h3_config_free(h3config);
        if (config) quiche_config_free(config);
    }
};

std::map<uint32_t, std::unique_ptr<Session>> g_sessions;
uint32_t g_nextSessionId = 1;

Session* findSession(uint32_t id) {
    auto it = g_sessions.find(id);
    return it == g_sessions.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// QUIC packet I/O
// ---------------------------------------------------------------------------

// Forward declarations (definitions appear later in this file).
void readDatagrams(Session* s);
void readStreams(Session* s);

// Re-arm the QUIC timeout deadline based on quiche's schedule. Driven on the
// per-frame poll loop in pumpSocket(); no libuv timer involved.
void armTimeout(Session* s) {
    if (!s->conn) { s->hasTimeout = false; return; }
    uint64_t ms = quiche_conn_timeout_as_millis(s->conn);
    if (ms == UINT64_MAX) {  // quiche: no timeout currently scheduled
        s->hasTimeout = false;
        return;
    }
    s->timeoutAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    s->hasTimeout = true;
}

void flushEgress(Session* s) {
    if (!s->conn || s->sock == kInvalidSocket) return;
    static uint8_t out[MAX_DATAGRAM_SIZE];
    while (true) {
        quiche_send_info sendInfo;
        ssize_t written = quiche_conn_send(s->conn, out, sizeof(out), &sendInfo);
        if (written == QUICHE_ERR_DONE) break;
        if (written < 0) {
            std::cerr << "[WebTransport] quiche_conn_send failed: " << written << std::endl;
            break;
        }
        // Non-blocking sendto; QUIC handles loss/retransmission. EWOULDBLOCK just
        // means the OS buffer is momentarily full — quiche will resend later.
        int sent = static_cast<int>(::sendto(
            s->sock, reinterpret_cast<const char*>(out), static_cast<int>(written), 0,
            reinterpret_cast<const struct sockaddr*>(&sendInfo.to),
            static_cast<socklen_t>(sendInfo.to_len)));
        if (sent < 0) {
            // Drop on transient error; retransmission is the QUIC layer's job.
            break;
        }
    }
    armTimeout(s);
}

// Drain all datagrams currently readable on the socket, feed them to quiche, and
// (once the session is up) read out datagrams/streams immediately. Reading right
// after quiche_conn_recv matters: quiche garbage-collects a server-initiated
// stream that arrives complete (data + FIN) if it is not read promptly. Also
// fires the QUIC timeout when its deadline has passed. Runs on the main thread.
void pumpSocket(Session* s) {
    if (!s->conn || s->sock == kInvalidSocket) return;

    static thread_local std::vector<uint8_t> rbuf(65536);
    while (true) {
        struct sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        auto n = ::recvfrom(s->sock, reinterpret_cast<char*>(rbuf.data()),
                            static_cast<int>(rbuf.size()), 0,
                            reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (n < 0) break;  // EWOULDBLOCK / no more data this tick (or transient error)

        quiche_recv_info recvInfo;
        recvInfo.from = reinterpret_cast<struct sockaddr*>(&from);
        recvInfo.from_len = fromLen;
        recvInfo.to = reinterpret_cast<struct sockaddr*>(&s->local);
        recvInfo.to_len = s->localLen;

        ssize_t done = quiche_conn_recv(s->conn, rbuf.data(),
                                        static_cast<size_t>(n), &recvInfo);
        if (done < 0 && done != QUICHE_ERR_DONE) {
            std::cerr << "[WebTransport] quiche_conn_recv failed: " << done << std::endl;
            continue;
        }
        if (s->wtReady) {
            readDatagrams(s);
            readStreams(s);
        }
    }

    // Drive QUIC timers off a monotonic clock instead of a libuv timer.
    if (s->hasTimeout && std::chrono::steady_clock::now() >= s->timeoutAt) {
        quiche_conn_on_timeout(s->conn);
        flushEgress(s);
    }
}

// ---------------------------------------------------------------------------
// Address resolution
// ---------------------------------------------------------------------------

bool resolvePeer(const std::string& host, int port, struct sockaddr_storage* out,
                 socklen_t* outLen) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }
    std::memcpy(out, res->ai_addr, res->ai_addrlen);
    *outLen = static_cast<socklen_t>(res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

// ---------------------------------------------------------------------------
// Handshake: HTTP/3 extended CONNECT
// ---------------------------------------------------------------------------

void sendConnectRequest(Session* s) {
    std::string authority = s->host + ":" + std::to_string(s->port);
    std::string path = s->path.empty() ? "/" : s->path;
    std::string origin = "https://" + authority;

    quiche_h3_header headers[] = {
        {(const uint8_t*)":method", 7, (const uint8_t*)"CONNECT", 7},
        {(const uint8_t*)":protocol", 9, (const uint8_t*)"webtransport", 12},
        {(const uint8_t*)":scheme", 7, (const uint8_t*)"https", 5},
        {(const uint8_t*)":authority", 10, (const uint8_t*)authority.c_str(), authority.size()},
        {(const uint8_t*)":path", 5, (const uint8_t*)path.c_str(), path.size()},
        {(const uint8_t*)"origin", 6, (const uint8_t*)origin.c_str(), origin.size()},
    };

    int64_t streamId = quiche_h3_send_request(s->h3, s->conn, headers, 6, /*fin=*/false);
    if (streamId < 0) {
        std::cerr << "[WebTransport] CONNECT send failed: " << streamId << std::endl;
        s->failed = true;
        g_events.push({s->id, EventType::Error, -1, 0, false, {}, "Failed to send CONNECT request"});
        return;
    }
    s->connectStreamId = streamId;
}

int headerCollect(uint8_t* name, size_t nameLen, uint8_t* value, size_t valueLen, void* argp) {
    auto* status = static_cast<std::string*>(argp);
    if (nameLen == 7 && std::memcmp(name, ":status", 7) == 0) {
        status->assign(reinterpret_cast<char*>(value), valueLen);
    }
    return 0;
}

void pollHandshake(Session* s) {
    if (!s->h3 || s->wtReady || s->failed) return;
    while (true) {
        quiche_h3_event* ev = nullptr;
        int64_t streamId = quiche_h3_conn_poll(s->h3, s->conn, &ev);
        if (streamId < 0) break;  // QUICHE_H3_ERR_DONE or error

        switch (quiche_h3_event_type(ev)) {
            case QUICHE_H3_EVENT_HEADERS: {
                std::string status;
                quiche_h3_event_for_each_header(ev, headerCollect, &status);
                if (streamId == s->connectStreamId) {
                    if (!status.empty() && status[0] == '2') {
                        s->wtReady = true;
                    } else {
                        s->failed = true;
                        g_events.push({s->id, EventType::Error, -1, 0, false, {},
                                       "WebTransport CONNECT rejected with status " + status});
                    }
                }
                break;
            }
            case QUICHE_H3_EVENT_FINISHED:
            case QUICHE_H3_EVENT_RESET:
                if (streamId == s->connectStreamId && !s->wtReady) {
                    s->failed = true;
                    g_events.push({s->id, EventType::Error, -1, 0, false, {},
                                   "WebTransport CONNECT stream closed before establishment"});
                }
                break;
            default:
                break;
        }
        quiche_h3_event_free(ev);
        if (s->wtReady || s->failed) break;
    }
}

// ---------------------------------------------------------------------------
// Datagram I/O (HTTP/3 datagram = quarter-stream-id varint + payload)
// ---------------------------------------------------------------------------

void readDatagrams(Session* s) {
    if (!s->conn || s->connectStreamId < 0) return;
    uint64_t expectedFlow = static_cast<uint64_t>(s->connectStreamId) / 4;
    static uint8_t buf[MAX_DATAGRAM_SIZE];
    while (true) {
        ssize_t len = quiche_conn_dgram_recv(s->conn, buf, sizeof(buf));
        if (len <= 0) break;
        uint64_t flowId = 0;
        size_t consumed = varintDecode(buf, static_cast<size_t>(len), &flowId);
        if (consumed == 0 || flowId != expectedFlow) continue;  // not our session
        Event e;
        e.sessionId = s->id;
        e.type = EventType::Datagram;
        e.data.assign(buf + consumed, buf + len);
        g_events.push(std::move(e));
    }
}

// ---------------------------------------------------------------------------
// Stream I/O
// ---------------------------------------------------------------------------

// Tries to consume the leading WT signal (frame type + session id) from a
// server-initiated stream's pending buffer. Returns true once consumed.
bool consumeStreamHeader(Session* s, uint64_t streamId, StreamState& st) {
    if (st.headerConsumed) return true;
    const uint8_t* p = st.pending.data();
    size_t avail = st.pending.size();

    uint64_t signal = 0;
    size_t n1 = varintDecode(p, avail, &signal);
    if (n1 == 0) return false;  // need more bytes

    if (st.isUni) {
        if (signal == H3_CONTROL_STREAM_TYPE || signal == H3_PUSH_STREAM_TYPE ||
            signal == H3_QPACK_ENCODER_STREAM_TYPE || signal == H3_QPACK_DECODER_STREAM_TYPE) {
            // An HTTP/3 control/qpack stream — drain and ignore from now on.
            st.isH3Owned = true;
            st.headerConsumed = true;
            st.pending.clear();
            return true;
        }
        if (signal != WT_STREAM_UNI_SIGNAL) {
            // Unknown unidirectional stream type — ignore.
            st.isH3Owned = true;
            st.headerConsumed = true;
            st.pending.clear();
            return true;
        }
    } else {
        if (signal != WT_STREAM_BIDI_SIGNAL) {
            st.isH3Owned = true;
            st.headerConsumed = true;
            st.pending.clear();
            return true;
        }
    }

    // Consume the session id varint that follows the signal.
    uint64_t sessionId = 0;
    size_t n2 = varintDecode(p + n1, avail - n1, &sessionId);
    if (n2 == 0) return false;  // need more bytes

    // Strip signal + session id; remainder is payload.
    st.pending.erase(st.pending.begin(), st.pending.begin() + n1 + n2);
    st.headerConsumed = true;
    return true;
}

void readStream(Session* s, uint64_t streamId) {
    if (streamId == static_cast<uint64_t>(s->connectStreamId)) {
        // The CONNECT request stream carries no WT payload; drain it. A FIN here
        // means the server ended the session.
        static uint8_t scratch[STREAM_READ_CHUNK];
        bool fin = false;
        uint64_t errCode = 0;
        while (true) {
            ssize_t r = quiche_conn_stream_recv(s->conn, streamId, scratch, sizeof(scratch),
                                                &fin, &errCode);
            if (r <= 0) break;
        }
        return;
    }

    auto& st = s->streams[streamId];
    if (!st.announced && s->streams[streamId].pending.empty() && !st.headerConsumed) {
        // First time we see this stream id: classify it.
        st.serverInitiated = (streamId & 0x1) != 0;
        st.isUni = (streamId & 0x2) != 0;
        // Client-initiated streams (we created them and wrote the signal) carry
        // pure payload on the read side; no header to strip.
        if (!st.serverInitiated) st.headerConsumed = true;
    }

    static uint8_t scratch[STREAM_READ_CHUNK];
    bool fin = false;
    uint64_t errCode = 0;
    bool sawFin = false;
    while (true) {
        fin = false;
        ssize_t r = quiche_conn_stream_recv(s->conn, streamId, scratch, sizeof(scratch),
                                            &fin, &errCode);
        if (r == QUICHE_ERR_DONE) break;
        if (r == QUICHE_ERR_INVALID_STREAM_STATE) {
            // The stream was collected by quiche. If we already buffered its data
            // (e.g. it arrived complete with FIN), fall through to deliver it;
            // otherwise it is a stale readable entry for an h3-internal stream.
            if (st.pending.empty() && !sawFin) {
                s->streams.erase(streamId);
                return;
            }
            break;
        }
        if (r < 0) {
            // Stream reset/stopped by the peer.
            Event e;
            e.sessionId = s->id;
            e.type = EventType::StreamReset;
            e.streamId = static_cast<int64_t>(streamId);
            e.code = errCode;
            g_events.push(std::move(e));
            s->streams.erase(streamId);
            return;
        }
        if (r > 0) st.pending.insert(st.pending.end(), scratch, scratch + r);
        if (fin) sawFin = true;
        if (r == 0) break;
    }

    // For server-initiated streams, parse the WT signal before emitting anything.
    if (st.serverInitiated && !st.headerConsumed) {
        if (!consumeStreamHeader(s, streamId, st)) {
            return;  // need more bytes for the header
        }
        if (st.isH3Owned) {
            // Drain & ignore h3 control/qpack data.
            return;
        }
    }

    // Announce a newly-arrived server stream to JS once.
    if (st.serverInitiated && !st.isH3Owned && !st.announced) {
        Event e;
        e.sessionId = s->id;
        e.type = st.isUni ? EventType::IncomingUni : EventType::IncomingBidi;
        e.streamId = static_cast<int64_t>(streamId);
        g_events.push(std::move(e));
        st.announced = true;
    }

    // Emit payload (and/or fin) to JS.
    if (!st.pending.empty() || (sawFin && !st.finDelivered)) {
        Event e;
        e.sessionId = s->id;
        e.type = EventType::StreamData;
        e.streamId = static_cast<int64_t>(streamId);
        e.data.swap(st.pending);
        e.fin = sawFin;
        g_events.push(std::move(e));
        if (sawFin) st.finDelivered = true;
    }

    if (sawFin) {
        // Keep client-initiated bidi streams around until both directions done;
        // for read-completed streams we can drop tracking.
        if (st.isUni || st.serverInitiated) {
            s->streams.erase(streamId);
        }
    }
}

void readStreams(Session* s) {
    if (!s->conn) return;
    quiche_stream_iter* it = quiche_conn_readable(s->conn);
    if (!it) return;
    uint64_t streamId = 0;
    std::vector<uint64_t> ids;
    while (quiche_stream_iter_next(it, &streamId)) {
        ids.push_back(streamId);
    }
    quiche_stream_iter_free(it);
    for (uint64_t id : ids) {
        readStream(s, id);
    }
}

// ---------------------------------------------------------------------------
// JS dispatch
// ---------------------------------------------------------------------------

js::Engine* g_engine = nullptr;
js::JSValueHandle g_dispatch{};  // protected handle to globalThis.__wtDispatch
bool g_hasDispatch = false;

void dispatchEvent(const Event& e) {
    if (!g_engine || !g_hasDispatch) return;

    const char* typeStr = "";
    switch (e.type) {
        case EventType::Ready: typeStr = "ready"; break;
        case EventType::Closed: typeStr = "closed"; break;
        case EventType::Error: typeStr = "error"; break;
        case EventType::Datagram: typeStr = "datagram"; break;
        case EventType::IncomingUni: typeStr = "incomingUni"; break;
        case EventType::IncomingBidi: typeStr = "incomingBidi"; break;
        case EventType::StreamData: typeStr = "streamData"; break;
        case EventType::StreamReset: typeStr = "streamReset"; break;
    }

    std::vector<js::JSValueHandle> args;
    args.push_back(g_engine->newNumber(e.sessionId));
    args.push_back(g_engine->newString(typeStr));

    switch (e.type) {
        case EventType::Datagram:
            args.push_back(g_engine->createUint8Array(e.data.data(), e.data.size()));
            break;
        case EventType::StreamData:
            args.push_back(g_engine->newNumber(static_cast<double>(e.streamId)));
            args.push_back(g_engine->createUint8Array(e.data.data(), e.data.size()));
            args.push_back(g_engine->newBoolean(e.fin));
            break;
        case EventType::IncomingUni:
        case EventType::IncomingBidi:
            args.push_back(g_engine->newNumber(static_cast<double>(e.streamId)));
            break;
        case EventType::StreamReset:
            args.push_back(g_engine->newNumber(static_cast<double>(e.streamId)));
            args.push_back(g_engine->newNumber(static_cast<double>(e.code)));
            break;
        case EventType::Error:
        case EventType::Closed:
            args.push_back(g_engine->newString(e.message.c_str()));
            break;
        default:
            break;
    }

    g_engine->call(g_dispatch, g_engine->newUndefined(), args);
    if (g_engine->hasException()) {
        std::cerr << "[WebTransport] dispatch threw: " << g_engine->getException() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Native bridge functions (called from the JS polyfill)
// ---------------------------------------------------------------------------

// Parses an https URL into host, port, path. Returns false on malformed input.
bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    size_t start = scheme.size();
    size_t pathStart = url.find('/', start);
    std::string authority = (pathStart == std::string::npos)
                                ? url.substr(start)
                                : url.substr(start, pathStart - start);
    path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) return false;  // WebTransport requires explicit port
    host = authority.substr(0, colon);
    try {
        port = std::stoi(authority.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return host.size() > 0 && port > 0 && port < 65536;
}

// Creates a non-blocking UDP socket bound to an ephemeral local port matching the
// peer's address family, and records the local address. Returns kInvalidSocket on
// failure.
socket_t createUdpSocket(Session* s) {
    socket_t fd = ::socket(s->peer.ss_family, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) return kInvalidSocket;

    struct sockaddr_storage bindAddr{};
    socklen_t bindLen;
    if (s->peer.ss_family == AF_INET6) {
        struct sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        std::memcpy(&bindAddr, &a, sizeof(a));
        bindLen = sizeof(struct sockaddr_in6);
    } else {
        struct sockaddr_in a{};
        a.sin_family = AF_INET;
        std::memcpy(&bindAddr, &a, sizeof(a));
        bindLen = sizeof(struct sockaddr_in);
    }
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&bindAddr), bindLen) != 0) {
        closeSocket(fd);
        return kInvalidSocket;
    }
    if (!setSocketNonBlocking(fd)) {
        closeSocket(fd);
        return kInvalidSocket;
    }

    s->localLen = sizeof(s->local);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&s->local), &s->localLen) != 0) {
        closeSocket(fd);
        return kInvalidSocket;
    }
    return fd;
}

// __wtConnect(url) -> sessionId (>=1) or 0 on immediate failure.
uint32_t connectSession(const std::string& url) {
    std::string host;
    int port = 0;
    std::string path;
    if (!parseUrl(url, host, port, path)) {
        return 0;
    }

    auto sess = std::make_unique<Session>();
    Session* s = sess.get();
    s->id = g_nextSessionId++;
    s->host = host;
    s->port = port;
    s->path = path;

    if (!resolvePeer(host, port, &s->peer, &s->peerLen)) {
        std::cerr << "[WebTransport] DNS resolution failed for " << host << std::endl;
        return 0;
    }

    s->sock = createUdpSocket(s);
    if (s->sock == kInvalidSocket) {
        std::cerr << "[WebTransport] failed to create UDP socket" << std::endl;
        return 0;
    }

    // quiche config.
    s->config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!s->config) return 0;
    static const uint8_t alpn[] = "\x02h3";  // length-prefixed "h3"
    quiche_config_set_application_protos(s->config, alpn, sizeof(alpn) - 1);
    quiche_config_set_max_idle_timeout(s->config, 30000);
    quiche_config_set_max_recv_udp_payload_size(s->config, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(s->config, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(s->config, 10 * 1024 * 1024);
    quiche_config_set_initial_max_stream_data_bidi_local(s->config, 1 * 1024 * 1024);
    quiche_config_set_initial_max_stream_data_bidi_remote(s->config, 1 * 1024 * 1024);
    quiche_config_set_initial_max_stream_data_uni(s->config, 1 * 1024 * 1024);
    quiche_config_set_initial_max_streams_bidi(s->config, 100);
    quiche_config_set_initial_max_streams_uni(s->config, 100);
    quiche_config_set_disable_active_migration(s->config, true);
    quiche_config_verify_peer(s->config, false);  // TODO: serverCertificateHashes
    // Disable GREASE: quiche would otherwise open an extra unidirectional stream
    // with a reserved type and then close it. That stream consumes the first WT
    // unidirectional stream id, so reusing it later fails (the id is "collected").
    quiche_config_grease(s->config, false);
    quiche_config_enable_dgram(s->config, true, 1024, 1024);
    quiche_config_set_cc_algorithm(s->config, QUICHE_CC_CUBIC);

    // h3 config with extended CONNECT + WebTransport SETTINGS.
    s->h3config = quiche_h3_config_new();
    if (!s->h3config) return 0;
    quiche_h3_config_enable_extended_connect(s->h3config, true);
    const uint64_t wtSettings[] = {
        SETTINGS_WT_MAX_SESSIONS, 1,
        SETTINGS_WEBTRANSPORT_MAX_SESSIONS_DRAFT, 1,
    };
    quiche_h3_config_set_additional_settings(s->h3config, wtSettings, 2);

    // Random source connection id.
    uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
    std::random_device rd;
    for (auto& b : scid) b = static_cast<uint8_t>(rd());

    s->conn = quiche_connect(
        host.c_str(), scid, sizeof(scid),
        reinterpret_cast<struct sockaddr*>(&s->local), s->localLen,
        reinterpret_cast<struct sockaddr*>(&s->peer), s->peerLen, s->config);
    if (!s->conn) {
        std::cerr << "[WebTransport] quiche_connect failed" << std::endl;
        return 0;
    }

    flushEgress(s);  // send the initial QUIC handshake packet(s)

    uint32_t id = s->id;
    g_sessions[id] = std::move(sess);
    return id;
}

void closeSession(uint32_t id, uint64_t code, const std::string& reason) {
    Session* s = findSession(id);
    if (!s) return;
    if (s->conn && !quiche_conn_is_closed(s->conn)) {
        quiche_conn_close(s->conn, /*app=*/true, code,
                          reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
        flushEgress(s);
    }
    s->wantClose = true;
}

// Returns 0 on success, -1 on failure (e.g. queue full or session gone).
int sendDatagram(uint32_t id, const uint8_t* data, size_t len) {
    Session* s = findSession(id);
    if (!s || !s->conn || !s->wtReady || s->connectStreamId < 0) return -1;
    std::vector<uint8_t> packet;
    varintEncode(packet, static_cast<uint64_t>(s->connectStreamId) / 4);
    packet.insert(packet.end(), data, data + len);
    ssize_t r = quiche_conn_dgram_send(s->conn, packet.data(), packet.size());
    flushEgress(s);
    return r < 0 ? -1 : 0;
}

// Attempts to flush a single stream's buffered outbound bytes to quiche. Safe to
// call repeatedly; advances the buffer by however much quiche accepts.
void pumpStream(Session* s, uint64_t streamId, StreamState& st) {
    if (!st.isOutgoing || st.sendError || st.finSent) return;
    if (st.outBuf.empty() && !st.outFin) return;

    uint64_t errCode = 0;
    // quiche only applies the FIN once the final byte is written, so it is safe
    // to pass outFin even on a partial write.
    ssize_t w = quiche_conn_stream_send(s->conn, streamId, st.outBuf.data(),
                                        st.outBuf.size(), st.outFin, &errCode);
    if (w == QUICHE_ERR_DONE) {
        return;  // no capacity right now; retry next frame
    }
    if (w < 0) {
        st.sendError = true;
        std::cerr << "[WebTransport] stream " << streamId << " send error: " << w << std::endl;
        return;
    }
    st.outBuf.erase(st.outBuf.begin(), st.outBuf.begin() + w);
    if (st.outBuf.empty() && st.outFin) {
        st.finSent = true;
    }
}

// Flushes every outgoing stream that still has buffered data.
void pumpStreamSends(Session* s) {
    if (!s->conn) return;
    for (auto& [streamId, st] : s->streams) {
        pumpStream(s, streamId, st);
    }
}

// __wtCreateStream(id, bidi) -> streamId or -1.
int64_t createStream(uint32_t id, bool bidi) {
    Session* s = findSession(id);
    if (!s || !s->conn || !s->wtReady || s->connectStreamId < 0) return -1;

    uint64_t streamId = bidi ? s->nextClientBidi : s->nextClientUni;
    if (bidi) {
        s->nextClientBidi += 4;
    } else {
        s->nextClientUni += 4;
    }

    auto& st = s->streams[streamId];
    st.isOutgoing = true;
    st.serverInitiated = false;
    st.isUni = !bidi;
    st.headerConsumed = true;  // inbound (bidi echo) on our stream is pure payload

    // The WT signal frame + session id must lead the stream.
    varintEncode(st.outBuf, bidi ? WT_STREAM_BIDI_SIGNAL : WT_STREAM_UNI_SIGNAL);
    varintEncode(st.outBuf, static_cast<uint64_t>(s->connectStreamId));

    pumpStream(s, streamId, st);
    flushEgress(s);
    return static_cast<int64_t>(streamId);
}

// __wtStreamWrite(id, streamId, data, fin) -> bytes accepted or -1.
int64_t streamWrite(uint32_t id, uint64_t streamId, const uint8_t* data, size_t len, bool fin) {
    Session* s = findSession(id);
    if (!s || !s->conn) return -1;
    auto it = s->streams.find(streamId);
    if (it == s->streams.end() || !it->second.isOutgoing) return -1;
    StreamState& st = it->second;
    if (st.sendError) return -1;

    st.outBuf.insert(st.outBuf.end(), data, data + len);
    if (fin) st.outFin = true;

    pumpStream(s, streamId, st);
    flushEgress(s);
    return static_cast<int64_t>(len);
}

void streamShutdown(uint32_t id, uint64_t streamId, uint64_t code) {
    Session* s = findSession(id);
    if (!s || !s->conn) return;
    quiche_conn_stream_shutdown(s->conn, streamId, QUICHE_SHUTDOWN_WRITE, code);
    quiche_conn_stream_shutdown(s->conn, streamId, QUICHE_SHUTDOWN_READ, code);
    flushEgress(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-frame driver
// ---------------------------------------------------------------------------

void processEvents(size_t maxEvents) {
    // Advance each session's state machine and collect events.
    std::vector<uint32_t> toTeardown;
    for (auto& [id, sessPtr] : g_sessions) {
        Session* s = sessPtr.get();
        if (!s->conn) continue;

        // Drain inbound UDP, feed quiche, read the data plane, and fire timers.
        pumpSocket(s);

        // QUIC handshake completion.
        if (!s->established && quiche_conn_is_established(s->conn)) {
            s->established = true;
        }
        // Create the h3 connection and send CONNECT once QUIC is up.
        if (s->established && !s->h3Created) {
            s->h3 = quiche_h3_conn_new_with_transport(s->conn, s->h3config);
            s->h3Created = true;
            if (s->h3) {
                sendConnectRequest(s);
            } else {
                s->failed = true;
                g_events.push({s->id, EventType::Error, -1, 0, false, {}, "Failed to create HTTP/3 connection"});
            }
            flushEgress(s);
        }
        // Drive the CONNECT handshake.
        if (s->h3Created && !s->wtReady && !s->failed) {
            pollHandshake(s);
            if (s->wtReady && !s->reportedReady) {
                s->reportedReady = true;
                // Detach the HTTP/3 layer now that the CONNECT handshake is done.
                // quiche routes all incoming unidirectional streams into an
                // attached h3 connection (which would drain WebTransport streams),
                // so freeing it lets server-initiated WT streams surface to the
                // transport layer where we read them directly. The underlying QUIC
                // streams (including the CONNECT/session stream) stay open.
                if (s->h3) {
                    quiche_h3_conn_free(s->h3);
                    s->h3 = nullptr;
                }
                g_events.push({s->id, EventType::Ready, -1, 0, false, {}, ""});
            }
            flushEgress(s);
        }
        // Data plane: datagram/stream READS happen in pumpSocket() (right after
        // quiche_conn_recv) so server-initiated streams are not garbage-collected
        // before we read them. Here we only retry blocked writes and flush egress.
        if (s->wtReady) {
            pumpStreamSends(s);  // retry any congestion-blocked writes
            flushEgress(s);
        }

        // Connection-level closure / failure detection.
        if (quiche_conn_is_closed(s->conn)) {
            if (!s->reportedClosed) {
                s->reportedClosed = true;
                bool isApp = false;
                uint64_t errCode = 0;
                const uint8_t* reason = nullptr;
                size_t reasonLen = 0;
                std::string msg;
                if (quiche_conn_peer_error(s->conn, &isApp, &errCode, &reason, &reasonLen) && reason) {
                    msg.assign(reinterpret_cast<const char*>(reason), reasonLen);
                }
                g_events.push({s->id, EventType::Closed, -1, errCode, false, {}, msg});
            }
            toTeardown.push_back(id);
        } else if (s->failed) {
            // Handshake failure without a transport-level close yet.
            if (!s->reportedClosed) {
                s->reportedClosed = true;
                g_events.push({s->id, EventType::Closed, -1, 0, false, {}, "WebTransport session failed"});
            }
            if (s->conn && !quiche_conn_is_closed(s->conn)) {
                quiche_conn_close(s->conn, true, 0, nullptr, 0);
                flushEgress(s);
            }
            toTeardown.push_back(id);
        }
    }

    // Drain the event queue to JS (on the main thread).
    size_t dispatched = 0;
    while (!g_events.empty() && dispatched++ < maxEvents) {
        Event e = std::move(g_events.front());
        g_events.pop();
        dispatchEvent(e);
    }

    // Free finished sessions (Session dtor closes the socket and frees quiche
    // objects). Safe here: we are no longer iterating g_sessions.
    for (uint32_t id : toTeardown) {
        g_sessions.erase(id);
    }
}

bool hasActiveSessions() {
    return !g_sessions.empty();
}

void init() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    if (std::getenv("MYSTRAL_WT_QUICHE_LOG")) {
        quiche_enable_debug_logging(
            [](const char* line, void*) { std::cerr << "[quiche] " << line << std::endl; },
            nullptr);
    }
}

void shutdown() {
    for (auto& [id, sessPtr] : g_sessions) {
        Session* s = sessPtr.get();
        if (s->conn && !quiche_conn_is_closed(s->conn)) {
            quiche_conn_close(s->conn, true, 0, nullptr, 0);
            flushEgress(s);
        }
    }
    g_sessions.clear();  // Session dtors close sockets and free quiche objects
    if (g_hasDispatch && g_engine) {
        g_engine->unprotect(g_dispatch);
        g_hasDispatch = false;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---------------------------------------------------------------------------
// JS bindings
// ---------------------------------------------------------------------------

namespace {

// Extracts bytes from a Uint8Array / ArrayBuffer argument.
std::vector<uint8_t> argToBytes(js::Engine* eng, js::JSValueHandle v) {
    std::vector<uint8_t> out;
    size_t size = 0;
    void* data = eng->getArrayBufferData(v, &size);
    if (data && size > 0) {
        out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
    return out;
}

}  // namespace

bool initBindings(js::Engine* engine) {
    g_engine = engine;

    engine->setGlobalProperty("__wtConnect",
        engine->newFunction("__wtConnect", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newNumber(0);
            std::string url = g_engine->toString(args[0]);
            uint32_t id = connectSession(url);
            return g_engine->newNumber(id);
        }));

    engine->setGlobalProperty("__wtClose",
        engine->newFunction("__wtClose", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() >= 1) {
                uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
                uint64_t code = args.size() >= 2 ? static_cast<uint64_t>(g_engine->toNumber(args[1])) : 0;
                std::string reason = args.size() >= 3 ? g_engine->toString(args[2]) : "";
                closeSession(id, code, reason);
            }
            return g_engine->newUndefined();
        }));

    engine->setGlobalProperty("__wtSendDatagram",
        engine->newFunction("__wtSendDatagram", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) return g_engine->newNumber(-1);
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            auto bytes = argToBytes(g_engine, args[1]);
            int r = sendDatagram(id, bytes.data(), bytes.size());
            return g_engine->newNumber(r);
        }));

    engine->setGlobalProperty("__wtCreateStream",
        engine->newFunction("__wtCreateStream", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) return g_engine->newNumber(-1);
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            bool bidi = g_engine->toBoolean(args[1]);
            int64_t sid = createStream(id, bidi);
            return g_engine->newNumber(static_cast<double>(sid));
        }));

    engine->setGlobalProperty("__wtStreamWrite",
        engine->newFunction("__wtStreamWrite", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 4) return g_engine->newNumber(-1);
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            uint64_t sid = static_cast<uint64_t>(g_engine->toNumber(args[1]));
            auto bytes = argToBytes(g_engine, args[2]);
            bool fin = g_engine->toBoolean(args[3]);
            int64_t w = streamWrite(id, sid, bytes.data(), bytes.size(), fin);
            return g_engine->newNumber(static_cast<double>(w));
        }));

    engine->setGlobalProperty("__wtStreamShutdown",
        engine->newFunction("__wtStreamShutdown", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() >= 2) {
                uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
                uint64_t sid = static_cast<uint64_t>(g_engine->toNumber(args[1]));
                uint64_t code = args.size() >= 3 ? static_cast<uint64_t>(g_engine->toNumber(args[2])) : 0;
                streamShutdown(id, sid, code);
            }
            return g_engine->newUndefined();
        }));

    // Register the JS dispatcher target. The polyfill (below) defines
    // globalThis.__wtDispatch; we grab and protect it after eval.
    extern const char* kWebTransportPolyfill;
    if (!engine->eval(kWebTransportPolyfill, "<webtransport-polyfill>")) {
        std::cerr << "[WebTransport] polyfill eval failed: " << engine->getException() << std::endl;
        return false;
    }

    g_dispatch = engine->getGlobalProperty("__wtDispatch");
    if (engine->isFunction(g_dispatch)) {
        engine->protect(g_dispatch);
        g_hasDispatch = true;
    } else {
        std::cerr << "[WebTransport] __wtDispatch not defined by polyfill" << std::endl;
        return false;
    }
    return true;
}

void resetBindings() {
    if (g_hasDispatch && g_engine) {
        g_engine->unprotect(g_dispatch);
    }
    g_dispatch = {};
    g_hasDispatch = false;
    g_engine = nullptr;
}

}  // namespace webtransport
}  // namespace mystral

#else  // !MYSTRAL_HAS_QUICHE

// ---------------------------------------------------------------------------
// Stub: WebTransport unavailable (quiche not compiled in).
// ---------------------------------------------------------------------------

#include "mystral/js/engine.h"

namespace mystral {
namespace webtransport {

void init() {}
void shutdown() {}
void processEvents(size_t) {}
bool hasActiveSessions() { return false; }

bool initBindings(js::Engine* engine) {
    // Provide a WebTransport that always rejects so feature-detecting code can
    // handle the absence gracefully.
    const char* stub = R"JS(
globalThis.WebTransport = class WebTransport {
  constructor() {
    const err = new Error('WebTransport is not supported in this build (quiche not compiled in)');
    this.ready = Promise.reject(err);
    this.closed = Promise.reject(err);
    this.ready.catch(() => {});
    this.closed.catch(() => {});
  }
  close() {}
};
)JS";
    engine->eval(stub, "<webtransport-stub>");
    return true;
}

void resetBindings() {}

}  // namespace webtransport
}  // namespace mystral

#endif
