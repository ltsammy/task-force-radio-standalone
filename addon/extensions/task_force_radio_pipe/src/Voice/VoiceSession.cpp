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
    callbacks.onVoiceFrame = [this](uint32_t sessionId, uint16_t /*sequence*/, bool /*isLast*/,
                                    const uint8_t* opus, size_t opusLen) {
        // Sequence/isLast aren't consumed yet -- RemoteVoiceSource's jitter buffer doesn't need
        // them (PLC covers gaps), and end-of-talkspurt is currently inferred from queue draining,
        // matching the C# reference's own EnqueueOpusFrame signature.
        m_playback.enqueueOpusFrame(sessionId, opus, opusLen);
    };
    callbacks.onRemoteJoined = [this](uint32_t sessionId, const std::string& uid,
                                      const std::string& /*name*/) {
        m_playback.addSource(sessionId, uid);
        {
            std::lock_guard<std::mutex> lock(m_rosterMutex);
            m_uidToSession[uid] = sessionId;
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

        std::lock_guard<std::mutex> lock(m_txCacheMutex);
        if (active) {
            m_txCache[senderSessionId] = TxCacheEntry{RadioTxInfo{uid, true, freq, range, sub},
                                                       std::chrono::steady_clock::now()};
        } else {
            m_txCache.erase(senderSessionId);
        }
    };
    callbacks.onConnectionStateChanged = [this](bool connected) {
        m_playback.setDebugForceAudible(connected && m_network.debugForceAudible());
        if (!connected) {
            m_playback.removeAllSources();
            std::lock_guard<std::mutex> lock(m_rosterMutex);
            m_uidToSession.clear();
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
    std::vector<uint32_t> seenSessionIds;
    seenSessionIds.reserve(units.size());

    {
        std::lock_guard<std::mutex> lock(m_rosterMutex);
        for (const AudibilityUpdate& unit : units) {
            const auto it = m_uidToSession.find(unit.uid);
            if (it == m_uidToSession.end()) continue;  // not on the voice server (yet)

            seenSessionIds.push_back(it->second);
            m_playback.setSourceState(
                it->second, RemoteSourceState{unit.gain, unit.azimuth, unit.muted, unit.effect,
                                              unit.errorLevel});
        }
    }

    // Full-replace semantics: anything previously active but missing from this snapshot goes
    // silent, matching VoiceSessionCoordinator.OnUnitsReceived exactly.
    const std::unordered_set<uint32_t> seenSet(seenSessionIds.begin(), seenSessionIds.end());
    for (uint32_t sessionId : m_lastActiveSessionIds) {
        if (seenSet.find(sessionId) == seenSet.end()) {
            m_playback.setSourceState(sessionId, RemoteSourceState::silent());
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
