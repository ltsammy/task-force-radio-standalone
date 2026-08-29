#include "VoiceSession.h"

#include <utility>

namespace tfrs {
namespace voice {

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
    };
    callbacks.onRemoteLeft = [this](uint32_t sessionId) { m_playback.removeSource(sessionId); };
    callbacks.onRadioTx = [](uint32_t /*senderSessionId*/, bool /*active*/, const std::string&,
                             uint16_t, const std::string&) {
        // Phase 4 routes this into State::setRemoteTx(); nothing consumes it yet in Phase 2.
    };
    callbacks.onConnectionStateChanged = [this](bool connected) {
        m_playback.setDebugForceAudible(connected && m_network.debugForceAudible());
        if (!connected) m_playback.removeAllSources();
    };
    m_network.start(std::move(callbacks));

    m_transmit.start([this](const uint8_t* opus, size_t len, bool isLast) {
        m_network.sendVoiceFrame(opus, len, isLast);
    });

    // Phase 2 placeholder connect target -- Phase 6 replaces this with CBA-settings-driven values
    // (ServerHost/ServerPort/ServerPassword) read from State's config store, and Phase 4 replaces
    // the identity with State::myUid() (the extension's own getPlayerUID-resolved value).
    setServer("127.0.0.1", 9987, "");
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

}  // namespace voice
}  // namespace tfrs
