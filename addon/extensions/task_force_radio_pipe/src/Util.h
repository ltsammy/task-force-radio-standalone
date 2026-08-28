// Small, dependency free helpers shared by the whole extension.
//
// The parsing helpers deliberately mirror the semantics of the original
// `old/ts/src/helpers.*` so that the byte level protocol described in
// docs/protocol-extension-legacy.md keeps behaving identically.
#pragma once

#include <string>
#include <vector>

namespace tfrs {

// ---------------------------------------------------------------------------
// Constants (ported from old/ts/src/common.hpp)
// ---------------------------------------------------------------------------
constexpr float kPi = 3.14159265358979323846f;
constexpr float kCantSpeakDistance = 5.0f;   // CANT_SPEAK_DISTANCE
constexpr float kCantSpeakGain = 14.0f;      // CANT_SPEAK_GAIN
constexpr float kDdMinDistance = 70.0f;      // DD_MIN_DISTANCE
constexpr float kDdMaxDistance = 300.0f;     // DD_MAX_DISTANCE
constexpr float kUnderwaterLevel = -1.1f;    // UNDERWATER_LEVEL
// AntennaConnection::isNull() in the original is `loss == 7.f`; the value is
// used as "no antenna" sentinel because a real loss is never above 1.
constexpr float kNoAntennaLoss = 7.0f;

// ---------------------------------------------------------------------------
// Vector math (ported from old/ts/src/datatypes.*)
// ---------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float f) const { return Vec3(x * f, y * f, z * f); }

    float length() const;
    float distanceTo(const Vec3& o) const;
    bool isNull() const { return x == 0.0f && y == 0.0f && z == 0.0f; }
};

// Parses "[1.5,2,3]" (Arma array syntax) as well as a bare "1.5,2,3".
// Anything unparseable yields a null vector, exactly like the original.
Vec3 parseVec3(const std::string& coordinateString);

// Distance of the segment a->b that lies below the water line (z < 0).
// Direct port of dataType::Position3D::distanceUnderwater.
float distanceUnderwater(const Vec3& a, const Vec3& b);

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

// Splits on `delim`, keeping empty tokens (matches helpers::split).
std::vector<std::string> split(const std::string& s, char delim);

// Same, but stops after `maxTokens` tokens. The last token stops at the next
// delimiter, it does NOT contain the remainder of the string (this is the
// behaviour of helpers::split(.., maxTokens) in the original).
std::vector<std::string> splitLimit(const std::string& s, char delim, size_t maxTokens);

// helpers::isTrue -> "true" or "1"
bool isTrue(const std::string& s);

float parseArmaNumber(const std::string& s);
int parseArmaNumberToInt(const std::string& s);

// Leading/trailing spaces are replaced by '_' (CommandProcessor::convertNickname).
std::string convertNickname(const std::string& nickname);

// ---------------------------------------------------------------------------
// Audio math (docs/dsp-audio-pipeline.md section 1 and 7)
// ---------------------------------------------------------------------------

// volumeLevel = ((radioVolume0to10 + 1) / 10) ^ 4
float volumeMultiplier(float volumeValue);

// -30 dB at `maxAudible`, -60 dB at twice that, hard cut below -60 dB.
float volumeAttenuation(float distance, bool shouldPlayerHear, float maxAudible,
                        float multiplier = 1.0f);

// Wraps an angle into [-pi, pi].
float wrapPi(float angle);

// Azimuth of `target` relative to `viewDirection`, as defined by
// docs/protocol-ipc-bridge.md: 0 = straight ahead, positive = clockwise/right.
// Arma world axes: x = east, y = north.
float azimuthTo(const Vec3& listener, const Vec3& viewDirection, const Vec3& target);

float clampf(float v, float lo, float hi);

}  // namespace tfrs
