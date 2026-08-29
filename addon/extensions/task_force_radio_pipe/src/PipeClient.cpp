#include "PipeClient.h"

#include <windows.h>

#include <chrono>
#include <cstdio>

#include "Json.h"

namespace tfrs {

namespace {

const wchar_t* const kPipeName = L"\\\\.\\pipe\\TFRS_VoiceBridge";

constexpr size_t kMaxOutQueue = 64;
constexpr size_t kMaxReadBuffer = 1 << 20;  // 1 MiB, guards against a broken peer
constexpr DWORD kPollIntervalMs = 10;
constexpr DWORD kReconnectIntervalMs = 500;

// Diagnostic-only. The extension otherwise has zero visibility anywhere (no RPT
// hookup, nothing) into why it thinks the bridge is up or down, which made a
// real-world divergence between this and the voice client's own belief
// impossible to root-cause from either side alone -- the voice client already
// logs its own connect/disconnect. Written next to its crash.log so both are in
// one place. Best-effort: any failure here must never affect the actual bridge
// connection, so every error is swallowed.
void logLine(const std::string& message) {
    wchar_t appData[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    std::wstring dir(appData);
    dir += L"\\Tfrs";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\VoiceClient";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\extension.log";

    HANDLE file = CreateFileW(dir.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);

    const std::string line = std::string(prefix) + message + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

PipeClient::PipeClient() : m_pipe(INVALID_HANDLE_VALUE) {}

PipeClient::~PipeClient() {
    stop(true);
}

void PipeClient::start(MessageHandler handler) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;

    m_handler = std::move(handler);
    m_running.store(true);
    m_thread = std::thread(&PipeClient::threadMain, this);
}

void PipeClient::stop(bool join) {
    m_running.store(false);
    if (join && m_thread.joinable()) {
        m_thread.join();
        closeConnection();
    }
}

void PipeClient::setUnitsMessage(std::string line) {
    std::lock_guard<std::mutex> lock(m_outMutex);
    m_pendingUnits = std::move(line);
    m_hasPendingUnits = true;
}

void PipeClient::sendLine(std::string line) {
    std::lock_guard<std::mutex> lock(m_outMutex);
    if (m_outQueue.size() >= kMaxOutQueue) m_outQueue.pop_front();
    m_outQueue.push_back(std::move(line));
}

bool PipeClient::tryConnect() {
    HANDLE handle = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE,
                                0,        // no sharing
                                nullptr,  // default security
                                OPEN_EXISTING,
                                0,  // blocking, non overlapped
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Logged sparingly (~every 10s at the 500ms retry cadence) so a voice
        // client that simply hasn't been launched yet doesn't spam the log, while
        // a reconnect that's stuck still leaves a trail of *why* CreateFileW keeps
        // failing -- ERROR_PIPE_BUSY (231) points at the voice client still
        // holding the single pipe instance open; anything else points elsewhere.
        if ((++m_connectFailCount % 20) == 1) {
            logLine("reconnect attempt failed, error=" + std::to_string(GetLastError()));
        }
        return false;
    }
    m_connectFailCount = 0;

    DWORD mode = PIPE_READMODE_BYTE;
    // Failure is not fatal: byte mode is the default for a .NET
    // NamedPipeServerStream anyway.
    SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);

    m_pipe = handle;
    m_readBuffer.clear();
    m_connected.store(true);
    logLine("connected to voice client");
    return true;
}

void PipeClient::closeConnection(const std::string& reason) {
    HANDLE handle = static_cast<HANDLE>(m_pipe);
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
        CloseHandle(handle);
    }
    m_pipe = INVALID_HANDLE_VALUE;
    const bool wasConnected = m_connected.exchange(false);
    m_readBuffer.clear();
    // Only a real connected->disconnected transition is logged -- stop()'s
    // shutdown close passes no reason and must stay silent.
    if (wasConnected && !reason.empty()) logLine("bridge pipe lost: " + reason);
}

bool PipeClient::writeAll(const std::string& data) {
    HANDLE handle = static_cast<HANDLE>(m_pipe);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;

    size_t written = 0;
    while (written < data.size()) {
        DWORD chunk = 0;
        const DWORD toWrite = static_cast<DWORD>(data.size() - written);
        if (!WriteFile(handle, data.data() + written, toWrite, &chunk, nullptr)) {
            m_lastWriteError = GetLastError();
            return false;
        }
        if (chunk == 0) {
            m_lastWriteError = 0;
            return false;
        }
        written += chunk;
    }
    return true;
}

void PipeClient::pumpReads() {
    HANDLE handle = static_cast<HANDLE>(m_pipe);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD err = GetLastError();
            closeConnection("PeekNamedPipe failed, error=" + std::to_string(err));
            return;
        }
        if (available == 0) break;

        char buffer[4096];
        DWORD toRead = available;
        if (toRead > sizeof(buffer)) toRead = static_cast<DWORD>(sizeof(buffer));

        DWORD read = 0;
        if (!ReadFile(handle, buffer, toRead, &read, nullptr) || read == 0) {
            const DWORD err = GetLastError();
            closeConnection("ReadFile failed, error=" + std::to_string(err));
            return;
        }

        if (m_readBuffer.size() + read > kMaxReadBuffer) {
            // Peer is not sending newline terminated messages; resynchronise.
            m_readBuffer.clear();
        }
        m_readBuffer.append(buffer, read);

        size_t start = 0;
        for (;;) {
            const size_t nl = m_readBuffer.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = m_readBuffer.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) dispatchLine(line);
            start = nl + 1;
        }
        if (start > 0) m_readBuffer.erase(0, start);
    }
}

void PipeClient::dispatchLine(const std::string& line) {
    if (!m_handler) return;
    json::Value value;
    if (!json::parse(line, value)) return;
    if (!value.isObject()) return;
    m_handler(value);
}

void PipeClient::threadMain() {
    while (m_running.load()) {
        if (!m_connected.load()) {
            if (!tryConnect()) {
                Sleep(kReconnectIntervalMs);
                continue;
            }
        }

        // 1) drain the outgoing side
        std::string unitsLine;
        bool haveUnits = false;
        std::deque<std::string> outgoing;
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            if (m_hasPendingUnits) {
                unitsLine.swap(m_pendingUnits);
                m_hasPendingUnits = false;
                haveUnits = true;
            }
            outgoing.swap(m_outQueue);
        }

        bool ok = true;
        while (ok && !outgoing.empty()) {
            ok = writeAll(outgoing.front());
            outgoing.pop_front();
        }
        if (ok && haveUnits) ok = writeAll(unitsLine);

        if (!ok) {
            closeConnection("WriteFile failed, error=" + std::to_string(m_lastWriteError));
            Sleep(kReconnectIntervalMs);
            continue;
        }

        // 2) read whatever the voice client sent us
        pumpReads();
        if (!m_connected.load()) {
            Sleep(kReconnectIntervalMs);
            continue;
        }

        Sleep(kPollIntervalMs);
    }

    closeConnection();
}

}  // namespace tfrs
