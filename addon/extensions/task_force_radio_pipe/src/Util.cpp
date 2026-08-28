#include "Util.h"

#include <cmath>
#include <cstdlib>

namespace tfrs {

float Vec3::length() const {
    return std::sqrt(x * x + y * y + z * z);
}

float Vec3::distanceTo(const Vec3& o) const {
    const float dx = x - o.x;
    const float dy = y - o.y;
    const float dz = z - o.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 parseVec3(const std::string& coordinateString) {
    Vec3 result;
    if (coordinateString.length() < 3) return result;

    std::string inner = coordinateString;
    if (inner.front() == '[') {
        // Strip the surrounding brackets. Be defensive about a missing ']'.
        const size_t end = inner.back() == ']' ? inner.length() - 2 : inner.length() - 1;
        if (end == 0) return result;
        inner = inner.substr(1, end);
    }

    const std::vector<std::string> coords = split(inner, ',');
    if (coords.size() == 2) {
        result.x = parseArmaNumber(coords[0]);
        result.y = parseArmaNumber(coords[1]);
    } else if (coords.size() == 3 || coords.size() == 4) {
        // Old TF_fnc_position overrides may return 4 elements, we only want 3.
        result.x = parseArmaNumber(coords[0]);
        result.y = parseArmaNumber(coords[1]);
        result.z = parseArmaNumber(coords[2]);
    }
    return result;
}

float distanceUnderwater(const Vec3& a, const Vec3& b) {
    if (a.z > 0.0f && b.z > 0.0f) return 0.0f;         // never crosses the water line
    if (a.z < 0.0f && b.z < 0.0f) return a.distanceTo(b);

    const Vec3& lower = (a.z < b.z) ? a : b;
    const Vec3& upper = (a.z > b.z) ? a : b;

    const Vec3 diff = upper - lower;
    if (diff.z == 0.0f || lower.z == 0.0f) return 0.0f;  // degenerate, avoid div by zero

    const float divisor = diff.z / (lower.z * -1.0f);
    if (divisor == 0.0f) return 0.0f;

    const Vec3 toWater(diff.x / divisor, diff.y / divisor, diff.z / divisor);
    const Vec3 waterLineIntersect = lower + toWater;

    if (a.z < 0.0f) return a.distanceTo(waterLineIntersect);
    return b.distanceTo(waterLineIntersect);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    std::string::size_type lastPos = 0;
    const std::string::size_type length = s.length();

    while (lastPos < length + 1) {
        std::string::size_type pos = s.find_first_of(delim, lastPos);
        if (pos == std::string::npos) pos = length;
        elems.emplace_back(s, lastPos, pos - lastPos);
        lastPos = pos + 1;
    }
    return elems;
}

std::vector<std::string> splitLimit(const std::string& s, char delim, size_t maxTokens) {
    std::vector<std::string> elems;
    if (maxTokens == 0) return elems;

    std::string::size_type lastPos = 0;
    const std::string::size_type length = s.length();

    while (lastPos < length + 1) {
        std::string::size_type pos = s.find_first_of(delim, lastPos);
        if (pos == std::string::npos) pos = length;
        elems.emplace_back(s, lastPos, pos - lastPos);
        if (elems.size() >= maxTokens) return elems;
        lastPos = pos + 1;
    }
    return elems;
}

bool isTrue(const std::string& s) {
    if (s.length() != 4) return s.length() == 1 && s[0] == '1';
    return s == "true";
}

float parseArmaNumber(const std::string& s) {
    if (s.empty()) return 0.0f;
    return std::strtof(s.c_str(), nullptr);
}

int parseArmaNumberToInt(const std::string& s) {
    return static_cast<int>(std::lround(parseArmaNumber(s)));
}

std::string convertNickname(const std::string& nickname) {
    if (nickname.empty()) return nickname;
    if (nickname.front() != ' ' && nickname.back() != ' ') return nickname;

    std::string out = nickname;
    size_t i = 0;
    while (i < out.size() && out[i] == ' ') {
        out[i] = '_';
        ++i;
    }
    size_t j = out.size();
    while (j > 0 && out[j - 1] == ' ') {
        out[j - 1] = '_';
        --j;
    }
    return out;
}

float volumeMultiplier(float volumeValue) {
    const float normalized = (volumeValue + 1.0f) / 10.0f;
    return std::pow(normalized, 4.0f);
}

float volumeAttenuation(float distance, bool shouldPlayerHear, float maxAudible,
                        float multiplier) {
    if (distance <= 1.0f) return 1.0f;
    float maxDistance = shouldPlayerHear ? maxAudible * multiplier : kCantSpeakDistance;
    if (maxDistance <= 0.0f) return 0.0f;

    const float gain = std::pow(10.0f, ((distance / (maxDistance * 2.0f)) * -60.0f) / 20.0f);
    if (gain < 0.001f) return 0.0f;
    return gain < 1.0f ? gain : 1.0f;
}

float wrapPi(float angle) {
    while (angle > kPi) angle -= 2.0f * kPi;
    while (angle < -kPi) angle += 2.0f * kPi;
    return angle;
}

float azimuthTo(const Vec3& listener, const Vec3& viewDirection, const Vec3& target) {
    const Vec3 delta = target - listener;
    if (delta.x == 0.0f && delta.y == 0.0f) return 0.0f;

    // Compass style bearings: atan2(east, north) grows clockwise.
    const float bearingToTarget = std::atan2(delta.x, delta.y);

    float viewBearing = 0.0f;
    if (viewDirection.x != 0.0f || viewDirection.y != 0.0f) {
        viewBearing = std::atan2(viewDirection.x, viewDirection.y);
    }
    return wrapPi(bearingToTarget - viewBearing);
}

float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace tfrs
