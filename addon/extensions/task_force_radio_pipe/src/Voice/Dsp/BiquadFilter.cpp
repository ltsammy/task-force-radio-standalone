#include "BiquadFilter.h"

#include <cmath>
#include <utility>

namespace tfrs {
namespace voice {

namespace {
// Not M_PI: that's a non-standard extension MSVC only exposes with _USE_MATH_DEFINES. Kept in
// double precision (unlike Util.h's float kPi) to match the C# reference's Math.PI exactly through
// the coefficient computation.
constexpr double kPi = 3.14159265358979323846;
}  // namespace

BiquadFilter::BiquadFilter(double b0, double b1, double b2, double a0, double a1, double a2)
    : m_b0(b0 / a0), m_b1(b1 / a0), m_b2(b2 / a0), m_a1(a1 / a0), m_a2(a2 / a0) {}

BiquadFilter BiquadFilter::rbjLowPass(double sampleRate, double cutoffHz, double q) {
    const double w0 = 2 * kPi * cutoffHz / sampleRate;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double al = sn / (2 * q);
    const double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
    const double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
    return BiquadFilter(b0, b1, b2, a0, a1, a2);
}

BiquadFilter BiquadFilter::rbjHighPass(double sampleRate, double cutoffHz, double q) {
    const double w0 = 2 * kPi * cutoffHz / sampleRate;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double al = sn / (2 * q);
    const double b0 = (1 + cs) / 2, b1 = -(1 + cs), b2 = (1 + cs) / 2;
    const double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
    return BiquadFilter(b0, b1, b2, a0, a1, a2);
}

BiquadFilter BiquadFilter::rbjBandPass(double sampleRate, double centerHz, double q) {
    const double w0 = 2 * kPi * centerHz / sampleRate;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double al = sn / (2 * q);
    const double b0 = sn / 2, b1 = 0, b2 = -sn / 2;
    const double a0 = 1 + al, a1 = -2 * cs, a2 = 1 - al;
    return BiquadFilter(b0, b1, b2, a0, a1, a2);
}

void BiquadFilter::reset() {
    m_x1 = m_x2 = m_y1 = m_y2 = 0;
}

float BiquadFilter::process(float input) {
    const double x0 = input;
    const double y0 = m_b0 * x0 + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;
    m_x2 = m_x1;
    m_x1 = x0;
    m_y2 = m_y1;
    m_y1 = y0;
    return static_cast<float>(y0);
}

void BiquadFilter::process(float* buffer, size_t count) {
    for (size_t i = 0; i < count; ++i) buffer[i] = process(buffer[i]);
}

CascadedFilter::CascadedFilter(std::vector<BiquadFilter> stages) : m_stages(std::move(stages)) {}

CascadedFilter CascadedFilter::bandPass(double sampleRate, double centerHz, double bandwidthHz,
                                        int order) {
    const double q = centerHz / bandwidthHz;
    const int count = order > 1 ? order : 1;
    std::vector<BiquadFilter> stages;
    stages.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) stages.push_back(BiquadFilter::rbjBandPass(sampleRate, centerHz, q));
    return CascadedFilter(std::move(stages));
}

void CascadedFilter::reset() {
    for (auto& s : m_stages) s.reset();
}

void CascadedFilter::process(float* buffer, size_t count) {
    for (auto& stage : m_stages) stage.process(buffer, count);
}

}  // namespace voice
}  // namespace tfrs
