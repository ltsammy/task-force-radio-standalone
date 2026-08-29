// Simplified stereo panning (docs/dsp-audio-pipeline.md section 2) -- a deliberate stand-in for
// the original's Clunk/KEMAR HRTF convolution, using the same gain law TFAR itself used for
// speaker radios. Direct port of Audio/Dsp/Panning.cs.
#pragma once

#include <cmath>
#include <utility>

namespace tfrs {
namespace voice {

struct Panning {
    // azimuthRadians: 0 = front, positive = clockwise (right), range -pi..pi. Uses sin(), NOT
    // cos(): the original formula's angle is measured from the right axis, not from "front" --
    // cos() against this azimuth convention would pan dead-ahead audio hard to one side instead of
    // centering it. This project has already shipped the wrong convention once (see
    // addon/extensions/task_force_radio_pipe/README.md "Important for the voice client" section
    // 1) -- do not "simplify" this back to cos().
    static std::pair<float, float> compute(float azimuthRadians) {
        const float s = std::sin(azimuthRadians);
        const float left = -0.37525f * s + 0.625f;
        const float right = 0.37525f * s + 0.625f;
        return {left, right};
    }
};

}  // namespace voice
}  // namespace tfrs
