// Thin wrapper around the vendored RNNoise (third_party/rnnoise, upstream v0.1.1) for real-time
// mic noise suppression -- toggleable via TFAR_Voice_NoiseSuppression (TransmitController).
//
// RNNoise processes 480-sample (10ms) frames at raw int16 scale (-32768..32767, NOT normalized
// -1..1 -- confirmed against upstream's own rnnoise_demo.c, which casts straight from `short`
// with no division). This project's audio pipeline is 960-sample (20ms) frames of normalized
// float throughout, so process() runs RNNoise twice per call (two 480-sample halves) and
// handles the scale conversion both ways internally -- callers never see raw RNNoise units.
#pragma once

#include <cstddef>

struct DenoiseState;

namespace tfrs {
namespace voice {

class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // in/out: exactly 960 normalized float samples (-1..1, 20ms @ 48kHz); safe to call with
    // out == in for in-place use. Not thread-safe -- matches the rest of TransmitController's
    // capture-callback-thread-only state.
    void process(const float* in960, float* out960);

private:
    DenoiseState* m_state = nullptr;
};

}  // namespace voice
}  // namespace tfrs
