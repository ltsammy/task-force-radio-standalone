#include "RemoteVoiceSource.h"

#include <algorithm>
#include <cstring>

#include "Dsp/Panning.h"
#include "Log.h"

namespace tfrs {
namespace voice {

namespace {

// Left/right gains for a source, shared by the voice path and the beep overlay so a start/stop
// cue lands in the same ear, at the same level, as the transmission it belongs to.
//
// stereoMode (the per-radio "which ear" setting, FREQ's 3rd field) is a hard channel cut, not a
// spatial pan -- it overrides azimuth entirely when set. Matches the original TS3 plugin's
// RadioEffect.hpp processRadioEffect: the silenced channel carries none of the signal, and the
// remaining one gets a 1.5x boost to compensate for losing the other ear's share.
void channelGains(const RemoteSourceState& state, float& leftGain, float& rightGain) {
    if (state.stereoMode == 1) {  // leftOnly
        leftGain = state.gain * 1.5f;
        rightGain = 0.0f;
    } else if (state.stereoMode == 2) {  // rightOnly
        leftGain = 0.0f;
        rightGain = state.gain * 1.5f;
    } else {
        const auto [left, right] = Panning::compute(state.azimuthRadians);
        leftGain = left * state.gain;
        rightGain = right * state.gain;
    }
}

// Whether we are currently hearing this source AS RADIO AUDIO, as opposed to hearing the person
// directly or over a vehicle intercom. Only then does a radio start/stop cue belong in our ears.
bool isRadioReception(SourceEffect effect) {
    return effect != SourceEffect::Direct && effect != SourceEffect::Intercom;
}

}  // namespace

RemoteVoiceSource::RemoteVoiceSource(uint32_t sessionId, std::string uid)
    : m_sessionId(sessionId), m_uid(std::move(uid)) {
    m_decodedShorts.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_monoFrame.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_stereoFrame.resize(static_cast<size_t>(OpusFormat::kFrameSamples) * 2);
    m_stereoFramePos = m_stereoFrame.size();  // force a decode on the first render()
    ensureEffectChain(SourceEffect::Direct);
}

void RemoteVoiceSource::ensureEffectChain(SourceEffect effect) {
    if (m_effectChain && effect == m_currentEffect) return;
    m_currentEffect = effect;
    m_effectChain = std::make_unique<RadioEffectChain>(effect);
}

void RemoteVoiceSource::enqueueOpusFrame(const uint8_t* opus, size_t opusLen, bool isLast) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pending.push_back(PendingFrame{std::vector<uint8_t>(opus, opus + opusLen), isLast});
    while (m_pending.size() > kMaxQueuedFrames) m_pending.pop_front();
}

void RemoteVoiceSource::setState(const RemoteSourceState& state) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = state;
}

bool RemoteVoiceSource::tryProduceNextFrame() {
    RemoteSourceState state;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        state = m_state;
    }

    if (state.muted) {
        m_isPlaying = false;
        m_concealmentCount = 0;
        // Actually drop the queued packets, and always report "produced nothing".
        //
        // This used to return m_pending.empty(), i.e. TRUE whenever anything was still queued --
        // but true means "m_stereoFrame now holds a fresh frame", and nothing had touched
        // m_stereoFrame, so render() replayed the LAST audible 20ms over and over. Nothing popped
        // the queue either, so it stayed non-empty forever and the loop never stopped: a source
        // muted mid-talkspurt (walked out of range, retuned, transmission ended) turned into a
        // permanent 50 Hz buzz of that final frame. That is the intermittent
        // noise/interference players have been reporting.
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_pending.clear();
        return false;
    }

    PendingFrame frame;
    bool hasPacket = false;
    bool notEnoughBuffered = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pending.empty()) {
            frame = std::move(m_pending.front());
            m_pending.pop_front();
            hasPacket = true;
            if (!m_isPlaying && static_cast<int>(m_pending.size()) < kJitterTargetFrames - 1) {
                // (Re)starting a talkspurt: wait for a short jitter cushion before audio begins,
                // to smooth out network jitter on the first few frames of a PTT press. Put the
                // packet back rather than drop it -- unless the whole talkspurt is already here
                // (its end-of-talkspurt frame is among what we hold), because then no further
                // frames are coming and waiting for the cushion to fill would stall it forever.
                // A transmission shorter than the cushion used to sit in the queue until the
                // NEXT one arrived and pushed it out.
                const bool haveWholeTalkspurt =
                    frame.isLast ||
                    std::any_of(m_pending.begin(), m_pending.end(),
                                [](const PendingFrame& f) { return f.isLast; });
                if (!haveWholeTalkspurt) {
                    m_pending.push_front(std::move(frame));
                    notEnoughBuffered = true;
                }
            }
        }
    }
    if (notEnoughBuffered) return false;

    if (!hasPacket) {
        if (!m_isPlaying) return false;  // nothing was playing -- stay silent, don't fabricate audio
        if (m_concealmentCount >= kMaxConcealmentFrames) {
            m_isPlaying = false;
            m_concealmentCount = 0;
            // Diagnostic-only, throttled to every 5th occurrence per session so a source stuck
            // repeatedly running dry doesn't flood the log.
            if ((++m_concealmentExhaustedCount % 5) == 1) {
                logLine("playback: session " + std::to_string(m_sessionId) +
                       " ran out of real packets and exhausted PLC (occurrence #" +
                       std::to_string(m_concealmentExhaustedCount) + " for this session)");
            }
            return false;
        }
        ++m_concealmentCount;
        m_decoder.decodePacketLoss(m_decodedShorts.data());
    } else {
        m_isPlaying = true;
        m_concealmentCount = 0;
        m_decoder.decode(frame.data.data(), static_cast<int>(frame.data.size()),
                         m_decodedShorts.data());
        // The sender's explicit end-of-talkspurt frame (a deliberate frame of digital silence).
        // Clearing m_isPlaying here is what stops the next dry queue from being treated as packet
        // loss: no 200ms of concealment audio smeared onto the end of every transmission, no
        // bogus "exhausted PLC" log line, and the jitter cushion is rebuilt for the next one.
        if (frame.isLast) m_isPlaying = false;
    }

    for (size_t i = 0; i < m_decodedShorts.size(); ++i) m_monoFrame[i] = m_decodedShorts[i] / 32768.0f;

    ensureEffectChain(state.effect);
    m_effectChain->process(m_monoFrame.data(), m_monoFrame.size(), state.errorLevel);

    float leftGain, rightGain;
    channelGains(state, leftGain, rightGain);
    for (size_t i = 0; i < m_monoFrame.size(); ++i) {
        m_stereoFrame[i * 2] = std::clamp(m_monoFrame[i] * leftGain, -1.0f, 1.0f);
        m_stereoFrame[i * 2 + 1] = std::clamp(m_monoFrame[i] * rightGain, -1.0f, 1.0f);
    }
    return true;
}

void RemoteVoiceSource::triggerBeep(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(m_beepMutex);
    m_pendingBeepSamples = samples;
    m_pendingBeepCount = count;
}

void RemoteVoiceSource::render(float* out, size_t frameCount) {
    size_t written = 0;
    while (written < frameCount) {
        if (m_stereoFramePos >= m_stereoFrame.size()) {
            if (!tryProduceNextFrame()) {
                std::memset(out + written * 2, 0, (frameCount - written) * 2 * sizeof(float));
                written = frameCount;
                break;
            }
            m_stereoFramePos = 0;
        }

        const size_t availableFrames = (m_stereoFrame.size() - m_stereoFramePos) / 2;
        const size_t toCopy = std::min(availableFrames, frameCount - written);
        std::memcpy(out + written * 2, m_stereoFrame.data() + m_stereoFramePos,
                    toCopy * 2 * sizeof(float));
        m_stereoFramePos += toCopy * 2;
        written += toCopy;
    }

    // Pick up any pending beep trigger, then mix whatever's currently playing on top of the voice
    // output above (which may just be the silence written by the loop's memset branch -- a beep
    // should still play even if nobody's actively talking on the radio right now).
    {
        std::lock_guard<std::mutex> lock(m_beepMutex);
        if (m_pendingBeepSamples != nullptr) {
            m_beepSamples = m_pendingBeepSamples;
            m_beepTotal = m_pendingBeepCount;
            m_beepPos = 0;
            m_pendingBeepSamples = nullptr;
        }
    }

    if (m_beepPos >= m_beepTotal) return;

    RemoteSourceState state;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        state = m_state;
    }
    if (state.muted) return;  // matches tryProduceNextFrame's own muted gate
    // A radio cue only belongs in our ears while we are actually receiving that person ON A RADIO.
    // The trigger itself fires on the network thread for every RadioTxBroadcast the relay sends,
    // from every client, with no idea whether we share a frequency -- so without this, anyone
    // standing close enough to be audible as direct speech also delivered their start/stop beeps
    // for channels we are not even on. Re-evaluated on every render call, not latched at trigger
    // time, so a cue for a transmission we DO receive still starts as soon as the audibility
    // solver's next tick (~66ms) says so.
    if (!isRadioReception(state.effect)) return;

    float leftGain, rightGain;
    channelGains(state, leftGain, rightGain);
    const size_t toMix = std::min(frameCount, m_beepTotal - m_beepPos);
    for (size_t i = 0; i < toMix; ++i) {
        const float s = m_beepSamples[m_beepPos + i] / 32768.0f;
        out[i * 2] = std::clamp(out[i * 2] + s * leftGain, -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(out[i * 2 + 1] + s * rightGain, -1.0f, 1.0f);
    }
    m_beepPos += toMix;
}

}  // namespace voice
}  // namespace tfrs
