// Arma 3 extension entry point.
//
// The exported signature is identical to the original
// old/extensions/task_force_radio_pipe/task_force_radio_pipe.cpp (which does
// NOT export RVExtensionVersion / RVExtensionArgs, so neither do we).
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "CommandProcessor.h"
#include "Json.h"
#include "PipeClient.h"
#include "State.h"
#include "Voice/VoiceSession.h"

namespace {

// ~15 Hz snapshot rate, well inside the "every 50-100 ms" the bridge protocol
// asks for while radio traffic is active.
const DWORD kSnapshotIntervalMs = 66;

// Everything is heap allocated and deliberately never freed: running static
// destructors (or joining threads) while the loader lock is held during
// DLL_PROCESS_DETACH is a classic source of shutdown deadlocks.
tfrs::State* g_state = nullptr;
tfrs::PipeClient* g_pipe = nullptr;
tfrs::CommandProcessor* g_processor = nullptr;
std::thread* g_senderThread = nullptr;
// Native voice port (mic capture, Opus, UDP networking to Tfrs.VoiceServer, playback/mixing) --
// see the port's plan doc. Phase 2: started unconditionally alongside everything else, entirely
// independent of the pipe/SQF-facing state above until Phase 4 wires them together.
tfrs::voice::VoiceSession* g_voice = nullptr;

std::once_flag g_initFlag;
std::atomic<bool> g_senderRunning(false);
std::atomic<bool> g_shuttingDown(false);
std::atomic<bool> g_ready(false);

void senderMain() {
    bool lastConnected = false;
    while (g_senderRunning.load()) {
        const bool connected = g_pipe->isConnected();
        if (connected != lastConnected) {
            if (connected) {
                g_state->onBridgeConnected();
            } else {
                g_state->onBridgeDisconnected();
            }
            lastConnected = connected;
        }

        if (connected) {
            std::string local = g_state->takePendingLocalMessage();
            if (!local.empty()) {
                local += '\n';
                g_pipe->sendLine(local);
            }
            std::string units = g_state->buildUnitsMessage();
            units += '\n';
            g_pipe->setUnitsMessage(units);
        }

        Sleep(kSnapshotIntervalMs);
    }
}

void initialize() {
    g_state = new tfrs::State();
    g_pipe = new tfrs::PipeClient();
    g_processor = new tfrs::CommandProcessor(*g_state, *g_pipe);

    g_pipe->start([](const tfrs::json::Value& message) { g_state->onBridgeMessage(message); });
    g_senderRunning.store(true);
    g_senderThread = new std::thread(senderMain);

    g_voice = new tfrs::voice::VoiceSession();
    g_voice->start();

    g_ready.store(true);
}

void ensureStarted() {
    // Never called from DllMain: creating threads under the loader lock
    // deadlocks. The first callExtension from SQF bootstraps everything.
    std::call_once(g_initFlag, initialize);
}

}  // namespace

extern "C" {

__declspec(dllexport) void __stdcall RVExtension(char* output, int outputSize,
                                                 const char* function) {
    if (output == nullptr || outputSize <= 0) return;
    output[0] = '\0';
    if (function == nullptr || function[0] == '\0') return;
    if (g_shuttingDown.load()) return;

    try {
        ensureStarted();
        if (!g_ready.load()) return;
        const std::string answer = g_processor->process(std::string(function));
        if (!answer.empty()) {
            strncpy_s(output, static_cast<size_t>(outputSize), answer.c_str(), _TRUNCATE);
        }
    } catch (...) {
        // Never let an exception escape into the game's main thread.
        output[0] = '\0';
    }
    output[outputSize - 1] = '\0';
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            // Only flag the shutdown. Joining threads inside DllMain would
            // deadlock on the loader lock; Arma never unloads the extension so
            // the OS reclaims everything at process exit.
            g_shuttingDown.store(true);
            g_senderRunning.store(false);
            if (g_pipe != nullptr) g_pipe->stop(false);
            break;
        default:
            break;
    }
    return TRUE;
}
