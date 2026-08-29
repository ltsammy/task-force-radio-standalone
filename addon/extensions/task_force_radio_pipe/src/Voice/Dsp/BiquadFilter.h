// Direct-Form-I biquad, RBJ Audio EQ Cookbook coefficient convention (a0-normalized). Direct port
// of Audio/Dsp/BiquadFilter.cs. See docs/dsp-audio-pipeline.md section 4.
#pragma once

#include <cstddef>
#include <vector>

namespace tfrs {
namespace voice {

class BiquadFilter {
public:
    static BiquadFilter rbjLowPass(double sampleRate, double cutoffHz, double q);
    static BiquadFilter rbjHighPass(double sampleRate, double cutoffHz, double q);
    // Constant skirt-gain bandpass (RBJ cookbook), parameterized by center freq + Q.
    static BiquadFilter rbjBandPass(double sampleRate, double centerHz, double q);

    void reset();
    float process(float input);
    void process(float* buffer, size_t count);

private:
    BiquadFilter(double b0, double b1, double b2, double a0, double a1, double a2);

    double m_b0 = 0, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
    double m_x1 = 0, m_x2 = 0, m_y1 = 0, m_y2 = 0;
};

// Cascaded biquad stages. Only the BandPass factory is ported -- the C# reference's
// ButterworthLowPass has zero callers there either (RadioEffectChain only ever uses
// CascadedFilter.BandPass), so it's not ported here.
class CascadedFilter {
public:
    // Approximation for the DSPFilters "order N Butterworth bandpass" stages used by the original
    // TS3 plugin: N cascaded constant-skirt-gain RBJ bandpass biquads at the same center/Q. Not a
    // literal Butterworth bandpass derivation, but audibly equivalent for the narrow effect bands
    // used here -- see docs/dsp-audio-pipeline.md section 4/8.
    static CascadedFilter bandPass(double sampleRate, double centerHz, double bandwidthHz, int order);

    void reset();
    void process(float* buffer, size_t count);

private:
    explicit CascadedFilter(std::vector<BiquadFilter> stages);
    std::vector<BiquadFilter> m_stages;
};

}  // namespace voice
}  // namespace tfrs
