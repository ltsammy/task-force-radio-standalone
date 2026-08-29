// Per-remote-source radio distortion chain, ported from Audio/Dsp/RadioEffectChain.cs (itself
// ported from the original TS3 plugin's RadioEffect.hpp -- see docs/dsp-audio-pipeline.md
// sections 3-5). One instance per currently-audible sender; holds filter/delay/phase state so it
// must not be shared across senders or reset between frames.
//
// Deliberate simplification vs. the original (documented in dsp-audio-pipeline.md section 8):
// "phone" and "speaker" apply only their bandpass filter (no foldback/delay/ringmod layering with
// the SW chain) -- close enough perceptually, much simpler to keep correct.
#pragma once

#include <cstddef>
#include <memory>
#include <random>
#include <vector>

#include "BiquadFilter.h"

namespace tfrs {
namespace voice {

// Matches the "fx" field of the (now-retired) bridge protocol's units snapshot -- see
// docs/dsp-audio-pipeline.md. Purely an internal enum now that everything lives in one process
// (see the voice port plan doc); State's solver still produces the same values.
enum class SourceEffect {
    Direct,
    Sw,
    Lr,
    Airborne,
    Dd,
    Phone,
    Speaker,
    Intercom,
};

class RadioEffectChain {
public:
    explicit RadioEffectChain(SourceEffect effect);

    // errorLevel: 0..1, from State's computed `err` field.
    void process(float* buffer, size_t count, float errorLevel);

private:
    void processDistortedRadio(float* buffer, size_t count, float rawErrorLevel);
    void processDiverRadio(float* buffer, size_t count, float errorLevel);
    float delay(float input);
    float ringModulation(float input, float mix);
    static float foldback(float input, float threshold);
    static float calcErrorLevel(float errorLevel);

    static constexpr int kSampleRate = 48000;
    static constexpr int kDelaySamples = kSampleRate / 20;  // 50ms

    const SourceEffect m_effect;
    std::unique_ptr<BiquadFilter> m_highPass;
    std::unique_ptr<BiquadFilter> m_lowPass;
    std::unique_ptr<CascadedFilter> m_bandPass;

    std::vector<float> m_delayLine;
    int m_delayPos = 0;
    float m_ringPhase = 0.0f;
    std::mt19937 m_rng{std::random_device{}()};
    std::uniform_real_distribution<double> m_uniform01{0.0, 1.0};
};

}  // namespace voice
}  // namespace tfrs
