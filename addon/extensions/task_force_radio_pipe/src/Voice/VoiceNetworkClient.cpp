#include "VoiceNetworkClient.h"

// WIN32_LEAN_AND_MEAN is defined project-wide (CMakeLists.txt), so <windows.h> never pulls in the
// legacy Winsock 1.1 <winsock.h> -- include order between it and <winsock2.h> is therefore safe
// either way; <windows.h> is included explicitly here for <bcrypt.h>'s base type dependencies.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>

#include "Log.h"
#include "Protocol.h"

namespace tfrs {
namespace voice {

// Not pulled in via <mstcpip.h> just for this one constant -- _WSAIOW is already provided by
// <winsock2.h> above, and the value is a fixed part of the Winsock ABI.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

constexpr int kHandshakeRetries = 5;
constexpr auto kHandshakeTimeout = std::chrono::milliseconds(1500);
constexpr auto kPingInterval = std::chrono::seconds(5);
constexpr auto kServerTimeout = std::chrono::seconds(15);
// Adapted from the C# reference's "5 attempts then give up, wait for the user to click Connect
// again": there is no user here, this is an always-on background service, so instead of giving up
// we just keep making fresh 5x1.5s handshake attempts indefinitely, a few seconds apart.
constexpr auto kReconnectInterval = std::chrono::seconds(2);
constexpr int kSocketRecvTimeoutMs = 300;
// Upper bound on how many datagrams one pumpReceive() call will drain before handing control back
// to threadMain's periodic housekeeping. See pumpReceive() for why an unbounded drain is a bug.
constexpr int kMaxPacketsPerPump = 256;

bool sha256(const std::string& input, uint8_t out[32]) {
    bool ok = false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        PUCHAR data = reinterpret_cast<PUCHAR>(const_cast<char*>(input.data()));
        if (BCryptHashData(hash, data, static_cast<ULONG>(input.size()), 0) == 0) {
            if (BCryptFinishHash(hash, out, 32, 0) == 0) ok = true;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace

VoiceNetworkClient::VoiceNetworkClient() = default;

VoiceNetworkClient::~VoiceNetworkClient() {
    stop();
}

void VoiceNetworkClient::start(Callbacks callbacks) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    m_callbacks = std::move(callbacks);
    m_running.store(true);
    m_thread = std::thread(&VoiceNetworkClient::threadMain, this);
}

void VoiceNetworkClient::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    closeSocket();
}

void VoiceNetworkClient::setServer(const std::string& host, uint16_t port,
                                   const std::string& password) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config.host = host;
    m_config.port = port;
    m_config.password = password;
}

void VoiceNetworkClient::setIdentity(const std::string& uid, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_identity.uid = uid;
    m_identity.name = name;
}

std::string VoiceNetworkClient::serverVersion() const {
    std::lock_guard<std::mutex> lock(m_serverVersionMutex);
    return m_serverVersion;
}

bool VoiceNetworkClient::configChangedLocked() const {
    return m_config.host != m_activeConfig.host || m_config.port != m_activeConfig.port ||
           m_config.password != m_activeConfig.password ||
           // Only the uid, not the display name: the name is informational server-side, and
           // churning the connection over a cosmetic rename would cost a real audio gap for
           // nothing. An empty uid is never treated as a change -- it only means "nothing better
           // is known yet", and reconnecting to hand the relay a worse identity helps no one.
           (!m_identity.uid.empty() && m_identity.uid != m_activeUid);
}

void VoiceNetworkClient::setConnected(bool connected) {
    m_connected.store(connected);
    if (!connected) {
        m_sessionId.store(0);
        m_debugForceAudible.store(false);
    }
    if (m_callbacks.onConnectionStateChanged) m_callbacks.onConnectionStateChanged(connected);
}

void VoiceNetworkClient::closeSocket() {
    if (m_socket != nullptr) {
        closesocket(reinterpret_cast<SOCKET>(m_socket));
        m_socket = nullptr;
    }
}

bool VoiceNetworkClient::resolveAndOpenSocket(const ServerConfig& config) {
    closeSocket();

    addrinfo hints{};
    hints.ai_family = AF_INET;  // matches the server, which is deliberately IPv4-only
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(config.port);
    const int gaiResult = getaddrinfo(config.host.c_str(), portStr.c_str(), &hints, &result);
    if (gaiResult != 0 || result == nullptr) {
        // Throttled: a not-yet-configured or misconfigured host retries every kReconnectInterval
        // forever, and every attempt would fail identically -- logging all of them would just
        // spam the file without adding information after the first one.
        if ((++m_resolveFailCount % 10) == 1) {
            logLine("DNS resolution failed for '" + config.host + ":" + portStr +
                   "', getaddrinfo error=" + std::to_string(gaiResult));
        }
        return false;
    }
    m_resolveFailCount = 0;

    const SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        const DWORD err = WSAGetLastError();
        freeaddrinfo(result);
        logLine("socket() failed, error=" + std::to_string(err));
        return false;
    }

    // "Connecting" a UDP socket filters incoming datagrams to this one peer and lets us use
    // send/recv instead of sendto/recvfrom -- matches the C# reference's UdpClient.Connect().
    const int connectResult = connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (connectResult != 0) {
        const DWORD err = WSAGetLastError();
        closesocket(s);
        logLine("connect() failed for '" + config.host + ":" + portStr +
               "', error=" + std::to_string(err));
        return false;
    }

    const DWORD timeoutMs = kSocketRecvTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof(timeoutMs));

    // Windows-only trap, and a nasty one for a *connected* UDP socket: if any ICMP "port
    // unreachable" comes back for a datagram we sent (a NAT/firewall on the path answering for a
    // mapping it has dropped, the relay host being briefly unreachable, ...), Winsock latches that
    // onto the socket and every later recv() returns WSAECONNRESET -- the socket is deaf from then
    // on even though sends keep succeeding. That is indistinguishable from "the server never
    // answered" in the handshake loop below. Turning the behavior off makes recv() ignore the ICMP
    // error, which is what every other platform does anyway.
    BOOL reportConnReset = FALSE;
    DWORD ioctlBytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportConnReset, sizeof(reportConnReset), nullptr, 0,
             &ioctlBytes, nullptr, nullptr);

    m_socket = reinterpret_cast<void*>(s);
    return true;
}

bool VoiceNetworkClient::tryHandshake(const ServerConfig& config, const Identity& identity) {
    uint8_t passwordHash[32];
    if (!sha256(config.password, passwordHash)) return false;

    const std::string uid = truncateUtf8(identity.uid, kMaxUidLength);
    const std::string name = truncateUtf8(identity.name, kMaxNameLength);

    uint8_t buf[1 + 1 + 32 + 1 + kMaxUidLength + 1 + kMaxNameLength];
    PacketWriter writer(buf, sizeof(buf));
    writer.writeByte(static_cast<uint8_t>(PacketType::ConnectRequest));
    writer.writeByte(kVersionMajor);
    writer.writeBytes(passwordHash, sizeof(passwordHash));
    writer.writeString8(uid);
    writer.writeString8(name);

    uint8_t recvBuf[2048];
    // Every recv() failure other than the expected SO_RCVTIMEO timeout, remembered for the
    // give-up log below. Without this, "the server never answered" and "our own socket errored"
    // produced the exact same log line, which is not enough to tell a network path problem from a
    // local one.
    int lastRecvError = 0;

    for (int attempt = 0; attempt < kHandshakeRetries; ++attempt) {
        // A FRESH socket per attempt, not one socket for all of them. A new socket means a new
        // local port, and therefore a new NAT/conntrack mapping the whole way out -- which is the
        // entire difference between five tries and one.
        //
        // This matters because of a reproducible failure seen on the production relay: a client
        // whose connection dropped kept retrying for 2m21s without a single ConnectRequest ever
        // reaching the server, and recovered the instant an unrelated application opened its own
        // UDP flow to the same host. Retrying five times down one wedged mapping is not five
        // tries, it is one; the affected players sit behind NATs handing out a narrow, low source
        // port range (observed: 6099 / 8734 / 8922 for one player), where a single unlucky mapping
        // is exactly the kind of thing that stays broken until something forces a new one.
        if (!resolveAndOpenSocket(config)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        const SOCKET s = reinterpret_cast<SOCKET>(m_socket);

        if (send(s, reinterpret_cast<const char*>(writer.data()), static_cast<int>(writer.size()),
                 0) < 0) {
            logLine("handshake: send() failed, error=" + std::to_string(WSAGetLastError()) +
                   " -- trying a fresh socket");
            continue;  // this socket is broken; the next attempt opens a new one
        }

        const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = recv(s, reinterpret_cast<char*>(recvBuf), sizeof(recvBuf), 0);
            if (n <= 0) {
                if (n < 0) {
                    const int err = WSAGetLastError();
                    if (err != WSAETIMEDOUT) lastRecvError = err;
                }
                continue;  // SO_RCVTIMEO timeout or a transient error -- keep waiting
            }

            const PacketType type = static_cast<PacketType>(recvBuf[0]);
            if (type == PacketType::ConnectAccept) {
                try {
                    PacketReader reader(recvBuf, static_cast<size_t>(n));
                    reader.readByte();  // packet type, already switched on above
                    const uint32_t sid = reader.readUInt32();
                    bool debugAudible = false;
                    std::string version;
                    // Read the trailing fields defensively -- a shorter packet (older/different
                    // server) is tolerated, matching the C# reference exactly.
                    if (reader.remaining() >= 2) reader.readUInt16();  // maxClients, informational
                    if (reader.remaining() >= 1) debugAudible = reader.readByte() != 0;
                    if (reader.remaining() >= 1) {
                        try {
                            version = reader.readString8(kMaxVersionLength);
                        } catch (...) {
                            // malformed trailing version field -- ignore, keep the empty default
                        }
                    }
                    m_sessionId.store(sid);
                    m_debugForceAudible.store(debugAudible);
                    {
                        std::lock_guard<std::mutex> lock(m_serverVersionMutex);
                        m_serverVersion = version;
                    }
                    logLine("connected to voice server, sessionId=" + std::to_string(sid));
                } catch (...) {
                    continue;  // malformed accept -- keep waiting within this attempt
                }
                m_handshakeSilentFailCount = 0;
                return true;
            }
            if (type == PacketType::ConnectReject) {
                const uint8_t reason = (n >= 2) ? recvBuf[1] : 0;
                logLine("connect rejected, reason=" + std::to_string(reason) +
                       " (1=BadPassword 2=ServerFull 3=VersionMismatch 4=BadRequest)");
                return false;  // rejected outright, retrying with the same credentials won't help
            }
            // Anything else (e.g. a stray late packet from a previous session) -- ignore, keep
            // waiting for a real reply.
        }
    }
    // Every attempt sent, none got any reply at all (not even a reject) -- distinct from DNS/socket
    // failure (already logged in resolveAndOpenSocket) and from an explicit reject (already logged
    // above). Throttled the same way: threadMain() retries this every ~kReconnectInterval forever,
    // and it fails identically each time, so only log every 10th occurrence.
    if ((++m_handshakeSilentFailCount % 10) == 1) {
        logLine("handshake: sent " + std::to_string(kHandshakeRetries) +
               " ConnectRequest(s) to '" + config.host + ":" + std::to_string(config.port) +
               "', got zero reply (recv errors: " +
               (lastRecvError == 0 ? std::string("none, only timeouts")
                                   : std::to_string(lastRecvError)) +
               ") -- check that outbound/inbound UDP to that port isn't being "
               "blocked by a firewall or antivirus on this PC");
    }
    return false;
}

void VoiceNetworkClient::handlePacket(const uint8_t* data, size_t len) {
    if (len < 1) return;
    PacketReader reader(data, len);
    const PacketType type = static_cast<PacketType>(reader.readByte());

    switch (type) {
        case PacketType::VoiceDown: {
            const uint32_t senderSessionId = reader.readUInt32();
            const uint16_t sequence = reader.readUInt16();
            const uint8_t flags = reader.readByte();
            size_t opusLen = 0;
            const uint8_t* opus = reader.readRemaining(opusLen);
            if (m_callbacks.onVoiceFrame) {
                m_callbacks.onVoiceFrame(senderSessionId, sequence,
                                         (flags & kVoiceFlagsLastFrame) != 0, opus, opusLen);
            }
            break;
        }
        case PacketType::ClientJoined: {
            const uint32_t sid = reader.readUInt32();
            const std::string uid = reader.readString8(kMaxUidLength);
            const std::string name = reader.readString8(kMaxNameLength);
            if (m_callbacks.onRemoteJoined) m_callbacks.onRemoteJoined(sid, uid, name);
            break;
        }
        case PacketType::ClientLeft: {
            const uint32_t sid = reader.readUInt32();
            if (m_callbacks.onRemoteLeft) m_callbacks.onRemoteLeft(sid);
            break;
        }
        case PacketType::RadioTxBroadcast: {
            const uint32_t sid = reader.readUInt32();
            const bool active = reader.readByte() != 0;
            const std::string freq = reader.readString8(kMaxFreqLength);
            const uint16_t range = reader.readUInt16();
            const std::string sub = reader.readString8(kMaxSubtypeLength);
            if (m_callbacks.onRadioTx) m_callbacks.onRadioTx(sid, active, freq, range, sub);
            break;
        }
        case PacketType::Pong:
            // RTT measurement isn't consumed by anything yet (no UI to show it natively) -- the
            // watchdog timer reset below (every received datagram counts) is what matters.
            break;
        default:
            // ConnectAccept/ConnectReject only matter during tryHandshake(); anything else
            // (including a stray ConnectRequest/RosterRequest, which this client never expects to
            // receive) is silently ignored, matching the C# reference's no-default-case switch.
            break;
    }
}

void VoiceNetworkClient::sendRaw(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_socket == nullptr || !m_connected.load()) return;
    send(reinterpret_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(data),
         static_cast<int>(len), 0);
    // Send failures are treated as transient (matches SendSafeAsync in the C# reference) -- the
    // watchdog is what actually detects a truly dead connection, not a single failed send.
}

void VoiceNetworkClient::sendVoiceFrame(const uint8_t* opus, size_t opusLen, bool isLast) {
    if (opus == nullptr || opusLen == 0 || opusLen > kMaxOpusFrameLength) return;
    if (!m_connected.load()) return;

    uint8_t buf[1 + 2 + 1 + kMaxOpusFrameLength];
    uint16_t sequence;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        sequence = m_voiceSequence++;  // wraps at 65536, matches the C# reference
        if (m_socket == nullptr || !m_connected.load()) return;

        PacketWriter writer(buf, sizeof(buf));
        writer.writeByte(static_cast<uint8_t>(PacketType::VoiceUp));
        writer.writeUInt16(sequence);
        writer.writeByte(isLast ? kVoiceFlagsLastFrame : kVoiceFlagsNone);
        writer.writeBytes(opus, opusLen);
        send(reinterpret_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(writer.data()),
             static_cast<int>(writer.size()), 0);
    }
}

void VoiceNetworkClient::sendRadioTx(bool active, const std::string& freq, uint16_t range,
                                     const std::string& sub) {
    const std::string truncFreq = truncateUtf8(freq, kMaxFreqLength);
    const std::string truncSub = truncateUtf8(sub, kMaxSubtypeLength);

    uint8_t buf[1 + 1 + 1 + kMaxFreqLength + 2 + 1 + kMaxSubtypeLength];
    PacketWriter writer(buf, sizeof(buf));
    writer.writeByte(static_cast<uint8_t>(PacketType::RadioTxUpdate));
    writer.writeByte(active ? 1 : 0);
    writer.writeString8(truncFreq);
    writer.writeUInt16(range);
    writer.writeString8(truncSub);
    sendRaw(writer.data(), writer.size());
}

void VoiceNetworkClient::requestRoster() {
    const uint8_t packet[1] = {static_cast<uint8_t>(PacketType::RosterRequest)};
    sendRaw(packet, sizeof(packet));
}

void VoiceNetworkClient::pumpReceive() {
    const SOCKET s = reinterpret_cast<SOCKET>(m_socket);
    uint8_t buf[2048];

    // Bounded on purpose. This loop used to run until recv() came back empty, which means until
    // the socket had been idle for a full SO_RCVTIMEO (300ms) -- and on a populated server that
    // never happens: a client receives a datagram roughly every 1.3ms, so pumpReceive() simply
    // never returned and everything threadMain does after it never ran. Measured on the live
    // relay with 25 clients connected: over 15 seconds it received 5112 RadioTxUpdates and
    // exactly ZERO Pings and ZERO RosterRequests, where ~75 of each were due.
    //
    // Two things were starved by that, both of which look like unrelated bugs from the outside:
    //
    //  - requestRoster(), the safety net that re-asks the server who is connected. Without it a
    //    ClientJoined lost from the burst the server sends on connect is never recovered, so a
    //    player stays permanently deaf to exactly the people who were already there when they
    //    joined -- while everyone who joins later arrives fine on their own broadcast.
    //  - the Ping, which is a silent listener's only keepalive once the client has nothing else
    //    to send. The server drops a session it has not heard from for 20s.
    //
    // Returning early loses nothing: whatever is still queued is drained on the next iteration,
    // a few hundred microseconds later.
    for (int drained = 0; drained < kMaxPacketsPerPump; ++drained) {
        const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) {
            return;  // SO_RCVTIMEO timeout, or a genuine recv error -- nothing more to read
        }
        m_lastRecvTime = std::chrono::steady_clock::now();
        try {
            handlePacket(buf, static_cast<size_t>(n));
        } catch (...) {
            // Malformed datagram -- drop just this one and keep the connection alive, same
            // policy as the C# reference ("any parse exception... is caught and ignored").
        }
    }
}

void VoiceNetworkClient::threadMain() {
    while (m_running.load()) {
        ServerConfig config;
        Identity identity;
        bool shouldReconnect = false;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            config = m_config;
            identity = m_identity;
            if (m_connected.load() && configChangedLocked()) shouldReconnect = true;
        }

        if (config.host.empty() || config.port == 0) {
            // Not configured yet -- driven by the voice_serverHost/Port CBA settings (see
            // fnc_initCBASettings.sqf). Wait rather than spin. Throttled the same way as the
            // resolve-failure log below: this state can persist indefinitely if a mission simply
            // never sets these, and that must not spam the file forever.
            if ((++m_notConfiguredLogCount % 10) == 1) {
                logLine("waiting: voice server not configured (host='" + config.host +
                       "', port=" + std::to_string(config.port) +
                       ") -- set Voice Server Address/Port under Configure Addons -> TFAR");
            }
            std::this_thread::sleep_for(kReconnectInterval);
            continue;
        }
        m_notConfiguredLogCount = 0;

        if (shouldReconnect) {
            closeSocket();
            setConnected(false);
        }

        if (!m_connected.load()) {
            // tryHandshake() opens its own socket per attempt -- see its doc comment.
            if (tryHandshake(config, identity)) {
                {
                    std::lock_guard<std::mutex> lock(m_configMutex);
                    m_activeConfig = config;
                    m_activeUid = identity.uid;
                }
                {
                    std::lock_guard<std::mutex> lock(m_sendMutex);
                    m_voiceSequence = 0;
                }
                m_lastPingTime = std::chrono::steady_clock::now();
                m_lastRecvTime = m_lastPingTime;
                setConnected(true);
            } else {
                closeSocket();
                std::this_thread::sleep_for(kReconnectInterval);
                continue;
            }
        }

        pumpReceive();

        if (!m_running.load()) break;

        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastPingTime >= kPingInterval) {
            uint8_t pingBuf[1 + 4];
            PacketWriter writer(pingBuf, sizeof(pingBuf));
            writer.writeByte(static_cast<uint8_t>(PacketType::Ping));
            const auto nowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            writer.writeUInt32(static_cast<uint32_t>(nowMs.count()));
            sendRaw(writer.data(), writer.size());
            requestRoster();  // cheap safety net against a dropped ClientJoined/Left broadcast
            m_lastPingTime = now;
        }
        if (now - m_lastRecvTime > kServerTimeout) {
            // No datagram of any kind (even malformed) for kServerTimeout -- declare the
            // connection lost. The outer loop's next iteration reconnects automatically.
            logLine("connection lost: no datagram received for " +
                   std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kServerTimeout).count()) +
                   "s (server crashed/restarted/network cut)");
            closeSocket();
            setConnected(false);
        }
    }
    closeSocket();
}

}  // namespace voice
}  // namespace tfrs
