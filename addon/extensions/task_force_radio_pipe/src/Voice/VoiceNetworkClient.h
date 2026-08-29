// Native UDP client for Tfrs.VoiceServer (docs/protocol-network.md), replacing
// voice-client/src/Tfrs.VoiceClient/Networking/VoiceNetworkClient.cs. The server is unmodified;
// this must speak its protocol byte-exactly (see Protocol.h).
//
// Unlike the old client, there is no user-facing "Connect" button to retry after a failure --
// this is an always-on background service (owns its own worker thread, mirrors PipeClient.cpp's
// threading shape: single thread, atomics for cross-thread status, a mutex around the socket for
// sends from other threads). The handshake itself still retries up to 5x/1.5s per attempt exactly
// like the C# reference; what's different is the OUTER policy keeps making fresh attempts
// indefinitely (every few seconds) rather than giving up after one failed cycle, since there's no
// user to click "Connect" again.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace tfrs {
namespace voice {

class VoiceNetworkClient {
public:
    struct Callbacks {
        std::function<void(uint32_t sessionId, uint16_t sequence, bool isLast, const uint8_t* opus,
                            size_t opusLen)>
            onVoiceFrame;
        std::function<void(uint32_t sessionId, const std::string& uid, const std::string& name)>
            onRemoteJoined;
        std::function<void(uint32_t sessionId)> onRemoteLeft;
        std::function<void(uint32_t senderSessionId, bool active, const std::string& freq,
                            uint16_t range, const std::string& sub)>
            onRadioTx;
        // Fires on every transition, both ways -- VoiceSession decides what to do (e.g. reset
        // playback state on false).
        std::function<void(bool connected)> onConnectionStateChanged;
    };

    VoiceNetworkClient();
    ~VoiceNetworkClient();
    VoiceNetworkClient(const VoiceNetworkClient&) = delete;
    VoiceNetworkClient& operator=(const VoiceNetworkClient&) = delete;

    // Starts the worker thread. Must not be called from DllMain (loader lock) -- same constraint
    // as PipeClient::start().
    void start(Callbacks callbacks);
    // Signals the worker to stop and joins it. Must not be called from DllMain with intent to
    // join; matches PipeClient::stop()'s DllMain-safety discipline.
    void stop();

    // Safe from any thread; picked up on the worker's next iteration. Changing host/port/password
    // while connected triggers a disconnect+reconnect with the new values.
    void setServer(const std::string& host, uint16_t port, const std::string& password);
    void setIdentity(const std::string& uid, const std::string& name);

    bool isConnected() const { return m_connected.load(); }
    uint32_t sessionId() const { return m_sessionId.load(); }
    bool debugForceAudible() const { return m_debugForceAudible.load(); }
    std::string serverVersion() const;

    // Fire-and-forget, safe from any thread (the capture callback thread for voice frames, the
    // extension's main tick thread for radio-tx). No-ops while not connected.
    void sendVoiceFrame(const uint8_t* opus, size_t opusLen, bool isLast);
    void sendRadioTx(bool active, const std::string& freq, uint16_t range, const std::string& sub);
    void requestRoster();

private:
    struct ServerConfig {
        std::string host;
        uint16_t port = 0;
        std::string password;
    };
    struct Identity {
        std::string uid;
        std::string name;
    };

    void threadMain();
    bool resolveAndOpenSocket(const ServerConfig& config);
    bool tryHandshake(const ServerConfig& config, const Identity& identity);
    void pumpReceive();
    void handlePacket(const uint8_t* data, size_t len);
    void sendRaw(const uint8_t* data, size_t len);
    void closeSocket();
    void setConnected(bool connected);
    bool configChangedLocked() const;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_connected{false};
    std::atomic<uint32_t> m_sessionId{0};
    std::atomic<bool> m_debugForceAudible{false};

    std::thread m_thread;
    Callbacks m_callbacks;

    std::mutex m_configMutex;
    ServerConfig m_config;      // desired config, set via setServer/setIdentity
    Identity m_identity;
    ServerConfig m_activeConfig;  // config the worker actually connected with

    // Opaque to avoid <winsock2.h> here (same reasoning as PipeClient's HANDLE); actually a
    // SOCKET (UINT_PTR). INVALID_SOCKET is (SOCKET)(~0), i.e. all-ones -- matched via a sentinel
    // constant in the .cpp rather than duplicating the winsock typedef.
    void* m_socket = nullptr;

    // Guards both the actual socket writes (sendVoiceFrame runs on the capture callback thread,
    // sendRadioTx/requestRoster on the extension's main tick thread, the worker thread sends
    // ConnectRequest/Ping) and m_voiceSequence below, so a frame is never sent with a duplicate or
    // out-of-order sequence number under concurrent callers.
    std::mutex m_sendMutex;
    uint16_t m_voiceSequence = 0;  // guarded by m_sendMutex; reset to 0 on every fresh connect

    std::string m_serverVersion;
    mutable std::mutex m_serverVersionMutex;

    // Worker-thread-only, no synchronization needed -- only threadMain() ever touches these.
    std::chrono::steady_clock::time_point m_lastPingTime;
    std::chrono::steady_clock::time_point m_lastRecvTime;
};

}  // namespace voice
}  // namespace tfrs
