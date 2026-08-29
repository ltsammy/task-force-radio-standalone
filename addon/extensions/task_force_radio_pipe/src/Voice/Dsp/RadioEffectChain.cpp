#include "RadioEffectChain.h"

#include <cmath>

namespace tfrs {
namespace voice {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Non-uniform piecewise-linear lookup table mapping raw errorLevel (0..1) to an effective
// distortion level -- copied verbatim from the C# reference, including its exact float literals.
constexpr float kErrorLevels[] = {
    0.0f,          0.150000006f, 0.300000012f, 0.600000024f, 0.899999976f, 0.950000048f,
    0.960000038f,  0.970000029f, 0.980000019f, 0.995000005f, 0.997799993f,
    0.998799993f,  0.99999f,
};
constexpr int kErrorLevelsCount = static_cast<int>(sizeof(kErrorLevels) / sizeof(kErrorLevels[0]));

}  // namespace

RadioEffectChain::RadioEffectChain(SourceEffect effect)
    : m_effect(effect), m_delayLine(static_cast<size_t>(kDelaySamples), 0.0f) {
    switch (effect) {
        case SourceEffect::Sw:
            m_highPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjHighPass(kSampleRate, 900, 0.85));
            m_lowPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjLowPass(kSampleRate, 3000, 2.0));
            break;
        case SourceEffect::Lr:
        case SourceEffect::Intercom:
            m_highPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjHighPass(kSampleRate, 520, 0.97));
            m_lowPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjLowPass(kSampleRate, 1300, 1.0));
            break;
        case SourceEffect::Airborne:
            m_highPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjHighPass(kSampleRate, 1000, 1.0));
            m_lowPass = std::make_unique<BiquadFilter>(BiquadFilter::rbjLowPass(kSampleRate, 4000, 1.0));
            break;
        case SourceEffect::Dd:
            m_bandPass = std::make_unique<CascadedFilter>(CascadedFilter::bandPass(kSampleRate, 1000, 400, 2));
            break;
        case SourceEffect::Phone:
            m_bandPass = std::make_unique<CascadedFilter>(CascadedFilter::bandPass(kSampleRate, 1850, 1550, 2));
            break;
        case SourceEffect::Speaker:
            m_bandPass = std::make_unique<CascadedFilter>(CascadedFilter::bandPass(kSampleRate, 2000, 1000, 1));
            break;
        case SourceEffect::Direct:
            break;  // no radio distortion at all -- just distance gain + panning
    }
}

void RadioEffectChain::process(float* buffer, size_t count, float errorLevel) {
    switch (m_effect) {
        case SourceEffect::Direct:
            return;
        case SourceEffect::Dd:
            processDiverRadio(buffer, count, errorLevel);
            return;
        case SourceEffect::Phone:
        case SourceEffect::Speaker:
            if (m_bandPass) m_bandPass->process(buffer, count);
            return;
        default:
            processDistortedRadio(buffer, count, errorLevel);
            return;
    }
}

void RadioEffectChain::processDistortedRadio(float* buffer, size_t count, float rawErrorLevel) {
    const float errorLevel = calcErrorLevel(rawErrorLevel);

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += std::fabs(buffer[i]);
    const float avg = count > 0 ? sum / static_cast<float>(count) : 0.0f;
    const float threshold = 0.3f * (1.0f - errorLevel) * (avg / 0.005f);

    for (size_t i = 0; i < count; ++i) buffer[i] = foldback(buffer[i], threshold);
    for (size_t i = 0; i < count; ++i) buffer[i] = ringModulation(delay(buffer[i] * 30.0f), errorLevel);

    if (m_highPass) m_highPass->process(buffer, count);
    if (m_lowPass) m_lowPass->process(buffer, count);
}

void RadioEffectChain::processDiverRadio(float* buffer, size_t count, float errorLevel) {
    for (size_t i = 0; i < count; ++i) {
        if (m_uniform01(m_rng) < errorLevel) buffer[i] = 0.0f;
    }
    if (m_bandPass) m_bandPass->process(buffer, count);
    for (size_t i = 0; i < count; ++i) buffer[i] *= 30.0f;
}

float RadioEffectChain::delay(float input) {
    m_delayLine[static_cast<size_t>(m_delayPos)] = input;
    m_delayPos = (m_delayPos + 1) % kDelaySamples;
    return m_delayLine[static_cast<size_t>(m_delayPos)];
}

float RadioEffectChain::ringModulation(float input, float mix) {
    const float modulated = input * std::sin(m_ringPhase * kPi / 2.0f);
    m_ringPhase += 90.0f / static_cast<float>(kSampleRate);
    if (m_ringPhase > 1.0f) m_ringPhase = 0.0f;
    return input * (1.0f - mix) + modulated * mix;
}

float RadioEffectChain::foldback(float input, float threshold) {
    if (threshold < 0.00001f) return 0.0f;
    if (input > threshold || input < -threshold) {
        // Matches C#'s `%` on floats (truncated fmod) -- same semantics as std::fmod in C++.
        input = std::fabs(std::fabs(std::fmod(input - threshold, threshold * 4.0f)) - threshold * 2.0f) -
                threshold;
    }
    return input;
}

float RadioEffectChain::calcErrorLevel(float errorLevel) {
    int part = static_cast<int>(errorLevel * 10.0f);
    if (part < 0) part = 0;
    if (part > kErrorLevelsCount - 2) part = kErrorLevelsCount - 2;
    const float from = kErrorLevels[part];
    const float to = kErrorLevels[part + 1];
    // Deliberately NOT renormalized to the local segment width -- copied exactly from the C#
    // reference; "fixing" this changes the effect audibly (see the header's doc comment).
    return from + (to - from) * (errorLevel - static_cast<float>(part) / 10.0f);
}

}  // namespace voice
}  // namespace tfrs
