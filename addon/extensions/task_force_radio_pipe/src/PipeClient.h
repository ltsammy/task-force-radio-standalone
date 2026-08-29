// Named pipe client for the bridge to Tfrs.VoiceClient.
//
// Protocol: docs/protocol-ipc-bridge.md
//   pipe   : \\.\pipe\TFRS_VoiceBridge
//   role   : voice client = server, this extension = client (CreateFileW + retry)
//   framing: UTF-8, one JSON message per '\n' terminated line
//
// The whole connection lives in a background thread so that the synchronous
// RVExtension call path (Arma main thread) never blocks on I/O.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace tfrs {
namespace json {
class Value;
}

class PipeClient {
public:
    using MessageHandler = std::function<void(const json::Value&)>;

    PipeClient();
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Starts the worker thread. Safe to call more than once (no-op then).
    // Must NOT be called from DllMain (loader lock).
    void start(MessageHandler handler);

    // Signals the worker to stop. `join` must be false when called from
    // DllMain, otherwise the loader lock can deadlock.
    void stop(bool join);

    bool isConnected() const { return m_connected.load(); }

    // Latest full `units` snapshot. Coalescing: a newer snapshot replaces an
    // unsent older one, so a stalled client can never build up a backlog.
    void setUnitsMessage(std::string line);

    // Small ordered queue for everything else (`local`, ...). Oldest entries
    // are dropped when the queue overflows.
    void sendLine(std::string line);

private:
    void threadMain();
    bool tryConnect();
    // `reason` is also the log-worthiness switch: empty means "don't log this"
    // (used by the ordinary stop()-triggered close, which isn't a divergence).
    void closeConnection(const std::string& reason = std::string());
    bool writeAll(const std::string& data);
    void pumpReads();
    void dispatchLine(const std::string& line);

    void* m_pipe = nullptr;  // HANDLE, kept opaque to avoid <windows.h> here
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_connected{false};

    std::thread m_thread;
    MessageHandler m_handler;

    std::mutex m_outMutex;
    std::string m_pendingUnits;
    bool m_hasPendingUnits = false;
    std::deque<std::string> m_outQueue;

    std::string m_readBuffer;

    // Diagnostics only, touched solely from the pipe I/O thread -- see the
    // logLine()-related comment in PipeClient.cpp. DWORD avoided here on purpose
    // (same reasoning as m_pipe above); it's unsigned long on Windows anyway.
    unsigned m_connectFailCount = 0;
    unsigned long m_lastWriteError = 0;
};

}  // namespace tfrs
