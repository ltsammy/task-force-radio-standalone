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
    m_pathScratch.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_stereoFramePos = m_stereoFrame.size();  // force a decode on the first render()
}

RadioEffectChain& RemoteVoiceSource::chainForPath(size_t index, SourceEffect effect) {
    if (m_chains.size() <= index) m_chains.resize(index + 1);
    PathChain& slot = m_chains[index];
    if (!slot.chain || slot.effect != effect) {
        slot.effect = effect;
        slot.chain = std::make_unique<RadioEffectChain>(effect);
    }
    return *slot.chain;
}

void RemoteVoiceSource::enqueueOpusFrame(const uint8_t* opus, size_t opusLen, bool isLast) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pending.push_back(PendingFrame{std::vector<uint8_t>(opus, opus + opusLen), isLast});
    while (m_pending.size() > kMaxQueuedFrames) m_pending.pop_front();
}

void RemoteVoiceSource::setStates(const RemoteSourcePaths& states) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_states = states;
}

bool RemoteVoiceSource::tryProduceNextFrame() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_renderStates = m_states;
    }
    // Drop paths that carry nothing, so "can we hear this person at all" is just "is the list
    // empty" from here on.
    m_renderStates.erase(std::remove_if(m_renderStates.begin(), m_renderStates.end(),
                                        [](const RemoteSourceState& s) {
                                            return s.muted || !(s.gain > 0.0f);
                                        }),
                         m_renderStates.end());

    if (m_renderStates.empty()) {
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
        std::lock_guard<std::mutex> lock(m_queueMutex);  // NOLINT: mirrors the muted-source drain
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

    // Decode once, then run EVERY way we can hear this person over its own copy and sum them.
    // This is what the original does -- old/ts/src/plugin.cpp loops over the reception paths,
    // copies the decoded buffer per path (`originalBuffer.copy()`), applies that path's effect and
    // gain, and finishes with `radio_buffer.mixIntoAdditive(sampleBuffer)`. Picking a single
    // "loudest" path instead was this port's own simplification, and it was the root cause behind
    // three separate reports in a row: a speaker silencing the radio in your ear, a driver's LR
    // transmission vanishing whenever a speaker radio was nearby, and intercom never being heard
    // inside a vehicle because direct speech always outweighed it.
    std::fill(m_stereoFrame.begin(), m_stereoFrame.end(), 0.0f);
    for (size_t p = 0; p < m_renderStates.size(); ++p) {
        const RemoteSourceState& state = m_renderStates[p];

        std::copy(m_monoFrame.begin(), m_monoFrame.end(), m_pathScratch.begin());
        chainForPath(p, state.effect).process(m_pathScratch.data(), m_pathScratch.size(),
                                              state.errorLevel);

        float leftGain, rightGain;
        channelGains(state, leftGain, rightGain);
        for (size_t i = 0; i < m_pathScratch.size(); ++i) {
            m_stereoFrame[i * 2] += m_pathScratch[i] * leftGain;
            m_stereoFrame[i * 2 + 1] += m_pathScratch[i] * rightGain;
        }
    }
    // Clamped once on the sum, not per path -- clamping each path first would let a loud one
    // silently eat the headroom the others need. PlaybackMixer soft-clips the full mix after this.
    for (float& sample : m_stereoFrame) sample = std::clamp(sample, -1.0f, 1.0f);

    // Chains for paths that no longer exist would otherwise keep their filter state forever.
    if (m_chains.size() > m_renderStates.size()) m_chains.resize(m_renderStates.size());
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

    // A radio cue only belongs in our ears while we are actually receiving that person ON A RADIO,
    // and it should arrive in the same ear and at the same level as that radio. The trigger itself
    // fires on the network thread for every RadioTxBroadcast the relay sends, from every client,
    // with no idea whether we share a frequency -- so without this, anyone close enough to be
    // audible as direct speech also delivered their start/stop beeps for channels we are not even
    // on. Re-evaluated on every render call, not latched at trigger time, so a cue for a
    // transmission we DO receive still starts as soon as the audibility solver's next tick
    // (~66ms) says so.
    RemoteSourceState state;
    bool haveRadioPath = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const RemoteSourceState& candidate : m_states) {
            if (candidate.muted || !(candidate.gain > 0.0f)) continue;
            if (!isRadioReception(candidate.effect)) continue;
            state = candidate;
            haveRadioPath = true;
            break;
        }
    }
    if (!haveRadioPath) return;

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
