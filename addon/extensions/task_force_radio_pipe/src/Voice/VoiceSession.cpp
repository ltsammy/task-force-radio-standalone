#include "VoiceSession.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace tfrs {
namespace voice {

namespace {
// Matches State.cpp's own kTxExpiry -- a remote transmission announcement is dropped when it is
// not refreshed. Keeping both sides at the same value isn't load-bearing (State independently
// expires whatever VoiceSession last pushed it), but keeps the two "who's transmitting" views
// from disagreeing about staleness for longer than necessary.
constexpr auto kTxExpiry = std::chrono::milliseconds(1500);
}  // namespace

VoiceSession::VoiceSession() = default;

VoiceSession::~VoiceSession() {
    stop();
}

void VoiceSession::start() {
    m_playback.start();

    VoiceNetworkClient::Callbacks callbacks;
    callbacks.onVoiceFrame = [this](uint32_t sessionId, uint16_t /*sequence*/, bool isLast,
                                    const uint8_t* opus, size_t opusLen) {
        // Sequence still isn't consumed (PLC covers gaps), but isLast is: the sender emits exactly
        // one frame flagged LastFrame when it stops transmitting, and dropping that flag meant
        // end-of-talkspurt could only ever be inferred from the queue running dry -- i.e. every
        // single normal end of a transmission burned through the full 200ms concealment budget
        // first, and then logged itself as a packet-loss event.
        m_playback.enqueueOpusFrame(sessionId, opus, opusLen, isLast);
    };
    callbacks.onRemoteJoined = [this](uint32_t sessionId, const std::string& uid,
                                      const std::string& name) {
        m_playback.addSource(sessionId, uid);
        {
            std::lock_guard<std::mutex> lock(m_rosterMutex);
            m_uidToSession[uid] = sessionId;
            if (!name.empty()) m_nameToSession[name] = sessionId;
        }
        // Starts muted (RemoteVoiceSource's own default) until the next applyAudibility() tick
        // (~66ms away, matching Extension.cpp's existing snapshot cadence) sets its real state --
        // same as the C# reference, which doesn't force sources audible on join either.
    };
    callbacks.onRemoteLeft = [this](uint32_t sessionId) {
        m_playback.removeSource(sessionId);
        std::lock_guard<std::mutex> lock(m_rosterMutex);
        for (auto it = m_uidToSession.begin(); it != m_uidToSession.end(); ++it) {
            if (it->second == sessionId) {
                m_uidToSession.erase(it);
                break;
            }
        }
        for (auto it = m_nameToSession.begin(); it != m_nameToSession.end(); ++it) {
            if (it->second == sessionId) {
                m_nameToSession.erase(it);
                break;
            }
        }
    };
    callbacks.onRadioTx = [this](uint32_t senderSessionId, bool active, const std::string& freq,
                                 uint16_t range, const std::string& sub) {
        std::string uid;
        {
            std::lock_guard<std::mutex> lock(m_rosterMutex);
            for (const auto& entry : m_uidToSession) {
                if (entry.second == senderSessionId) {
                    uid = entry.first;
                    break;
                }
            }
        }
        if (uid.empty()) return;  // not (yet) in the roster -- ignore, matches applyAudibility's policy

        bool wasActive = false;
        std::string lastSubtype;
        {
            std::lock_guard<std::mutex> lock(m_txCacheMutex);
            const auto it = m_txCache.find(senderSessionId);
            wasActive = (it != m_txCache.end());
            if (wasActive) lastSubtype = it->second.info.subtype;

            if (active) {
                m_txCache[senderSessionId] = TxCacheEntry{RadioTxInfo{uid, true, freq, range, sub},
                                                           std::chrono::steady_clock::now()};
            } else {
                m_txCache.erase(senderSessionId);
            }
        }

        // Radio start/stop beep, edge-triggered: onRadioTx fires on every RadioTxBroadcast, which
        // resends every tick while active (see sendRadioTx's own doc comment) -- only the actual
        // active/inactive transition should play a cue, not every refresh. The end cue uses the
        // cached subtype, not `sub`, since the wire protocol leaves `sub` meaningless on an
        // active=false packet (RadioTxUpdate's own doc comment).
        if (active && !wasActive) {
            m_playback.triggerRemoteBeep(senderSessionId, sub, /*start=*/true);
        } else if (!active && wasActive) {
            m_playback.triggerRemoteBeep(senderSessionId, lastSubtype, /*start=*/false);
        }
    };
    callbacks.onConnectionStateChanged = [this](bool connected) {
        m_playback.setDebugForceAudible(connected && m_network.debugForceAudible());
        if (!connected) {
            m_playback.removeAllSources();
            std::lock_guard<std::mutex> lock(m_rosterMutex);
            m_uidToSession.clear();
            m_nameToSession.clear();
            std::lock_guard<std::mutex> txLock(m_txCacheMutex);
            m_txCache.clear();
        }
    };
    m_network.start(std::move(callbacks));

    m_transmit.start([this](const uint8_t* opus, size_t len, bool isLast) {
        m_network.sendVoiceFrame(opus, len, isLast);
    });

    // No setServer() call here: m_network starts with an empty host/port-0 config, which
    // VoiceNetworkClient::threadMain() correctly treats as "not configured yet, wait" rather than
    // attempting to connect anywhere -- Extension.cpp's tick calls setServer() every cycle with
    // State's real voice_serverHost/Port/Password CBA settings (see fnc_initCBASettings.sqf) the
    // moment they're known. setIdentity() gets the same treatment with State::myUid()/myNickname().
    setIdentity("0", "TFRS");
}

void VoiceSession::stop() {
    m_transmit.stop();
    m_network.stop();
    m_playback.stop();
}

void VoiceSession::setServer(const std::string& host, uint16_t port, const std::string& password) {
    m_network.setServer(host, port, password);
}

void VoiceSession::setIdentity(const std::string& uid, const std::string& name) {
    m_network.setIdentity(uid, name);
}

void VoiceSession::applyAudibility(const std::vector<AudibilityUpdate>& units) {
    // Matches VoiceSessionCoordinator.OnUnitsReceived's own early return: while debug-force-
    // audible is on, every source was already set fully audible once at creation
    // (PlaybackMixer::addSource) -- ignore whatever the solver computed so it can't silence/pan
    // anyone back, keeping this a clean test of raw voice transport.
    if (m_playback.debugForceAudible()) return;

    // Several rows can resolve to the SAME session -- one per way we currently hear that person
    // (their radio, a speaker relaying it, direct speech) -- and all of them get mixed, so they
    // are grouped rather than letting whichever arrives last win.
    std::unordered_map<uint32_t, RemoteSourcePaths> pathsBySession;
    {
        std::lock_guard<std::mutex> lock(m_rosterMutex);
        for (const AudibilityUpdate& unit : units) {
            uint32_t sessionId = 0;
            bool resolved = false;
            const auto uidIt = m_uidToSession.find(unit.uid);
            if (uidIt != m_uidToSession.end()) {
                sessionId = uidIt->second;
                resolved = true;
            } else if (!unit.nickname.empty()) {
                // Fallback: the remote handshook under its nickname because its own getPlayerUID
                // had not resolved yet (see State::myUid()), or ours for it has not. Without this
                // the source stays permanently silent for this one listener -- the exact shape of
                // the "some people can't hear one specific player" reports.
                const auto nameIt = m_nameToSession.find(unit.nickname);
                if (nameIt != m_nameToSession.end()) {
                    sessionId = nameIt->second;
                    resolved = true;
                }
            }
            if (!resolved) continue;  // not on the voice server (yet)

            pathsBySession[sessionId].push_back(RemoteSourceState{
                unit.gain, unit.azimuth, unit.muted, unit.effect, unit.errorLevel,
                unit.stereoMode});
        }
    }

    std::vector<uint32_t> seenSessionIds;
    seenSessionIds.reserve(pathsBySession.size());
    for (const auto& entry : pathsBySession) {
        seenSessionIds.push_back(entry.first);
        m_playback.setSourceStates(entry.first, entry.second);
    }

    // Full-replace semantics: anything previously active but missing from this snapshot goes
    // silent, matching VoiceSessionCoordinator.OnUnitsReceived exactly.
    const std::unordered_set<uint32_t> seenSet(seenSessionIds.begin(), seenSessionIds.end());
    for (uint32_t sessionId : m_lastActiveSessionIds) {
        if (seenSet.find(sessionId) == seenSet.end()) {
            m_playback.setSourceStates(sessionId, RemoteSourcePaths{});
        }
    }

    m_lastActiveSessionIds = std::move(seenSessionIds);
}

void VoiceSession::setAddonOverride(bool hasOverride, bool overrideValue) {
    m_transmit.setAddonOverride(hasOverride, overrideValue);
}

void VoiceSession::sendRadioTx(bool active, const std::string& freq, uint16_t range,
                               const std::string& sub) {
    m_network.sendRadioTx(active, freq, range, sub);
}

std::vector<RadioTxInfo> VoiceSession::currentRadioTx() const {
    std::vector<RadioTxInfo> result;
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_txCacheMutex);
    for (const auto& entry : m_txCache) {
        if (now - entry.second.receivedAt < kTxExpiry) result.push_back(entry.second.info);
    }
    return result;
}

}  // namespace voice
}  // namespace tfrs
