#include "NoiseSuppressor.h"

#include <algorithm>

#include <rnnoise.h>

namespace tfrs {
namespace voice {

namespace {
constexpr int kRnnoiseFrameSize = 480;  // rnnoise_get_frame_size(), fixed for this model
}  // namespace

NoiseSuppressor::NoiseSuppressor() : m_state(rnnoise_create(nullptr)) {}

NoiseSuppressor::~NoiseSuppressor() {
    if (m_state != nullptr) rnnoise_destroy(m_state);
}

void NoiseSuppressor::process(const float* in960, float* out960) {
    if (m_state == nullptr) {
        if (out960 != in960) std::copy(in960, in960 + 960, out960);
        return;
    }

    float scratch[kRnnoiseFrameSize];
    for (int half = 0; half < 2; ++half) {
        const float* in = in960 + half * kRnnoiseFrameSize;
        float* out = out960 + half * kRnnoiseFrameSize;
        for (int i = 0; i < kRnnoiseFrameSize; ++i) scratch[i] = in[i] * 32768.0f;
        rnnoise_process_frame(m_state, scratch, scratch);
        for (int i = 0; i < kRnnoiseFrameSize; ++i) {
            out[i] = std::clamp(scratch[i] / 32768.0f, -1.0f, 1.0f);
        }
    }
}

}  // namespace voice
}  // namespace tfrs
