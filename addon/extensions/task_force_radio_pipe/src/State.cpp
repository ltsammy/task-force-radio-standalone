#include "State.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include <opus.h>

#include "Version.h"

namespace tfrs {

namespace {

// A client whose POS is older than this is not considered part of the world
// any more (original: MILLIS_TO_EXPIRE = 4000 ms, plus some slack).
constexpr std::chrono::milliseconds kClientExpiry(6000);

// A remote transmission announcement is dropped when it is not refreshed.
constexpr std::chrono::milliseconds kTxExpiry(1500);

// Heuristic used only until the voice client sends its first `tx` message,
// see README.md. Roughly the range of a typical long range radio.
constexpr float kAssumedRadioRange = 3000.0f;

// Config keys accepted by SETCFG (docs/protocol-extension-legacy.md).
const char* const kValidConfigKeys[] = {
    "full_duplex",         "addon_version",          "serious_channelName",
    "serious_channelPassword", "intercomVolume",     "intercomEnabled",
    "pluginTimeout",       "headsetLowered",         "spectatorNotHearEnemies",
    "spectatorCanHearFriendlies", "tangentReleaseDelay", "moveWhileTabbedOut",
    "intercomDucking",     "minimumPluginVersion",   "objectInterceptionStrength",
    "voiceCone",           "allowDebugging",         "noAutomoveSpectator",
    "disableAutomaticMute", "muteSpectators"};

bool isValidConfigKey(const std::string& key) {
    for (size_t i = 0; i < sizeof(kValidConfigKeys) / sizeof(kValidConfigKeys[0]); ++i) {
        if (key == kValidConfigKeys[i]) return true;
    }
    return false;
}

VehicleDesc parseVehicle(const std::string& vehicleId) {
    VehicleDesc result;
    if (vehicleId.empty() || vehicleId == "no") return result;

    const std::vector<std::string> parts = split(vehicleId, '\x10');
    if (parts.size() < 3) return result;  // malformed, treat as "no vehicle"

    result.name = parts[0];
    result.isolation = (parts[1] == "turnout") ? 0.0f : parseArmaNumber(parts[1]);
    result.intercomSlot = parseArmaNumberToInt(parts[2]);
    if (parts.size() > 3) result.velocity = parseVec3(parts[3]);
    return result;
}

// helpers::parseFrequencies: `["51.5X|7|0|classname","52.0X|5|1|classname"]`
std::map<std::string, FreqSetting> parseFrequencies(const std::string& raw) {
    std::map<std::string, FreqSetting> result;
    if (raw.length() < 2) return result;

    const std::string inner = raw.substr(1, raw.length() - 2);
    if (inner.empty()) return result;

    const std::vector<std::string> entries = split(inner, ',');
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& entry = entries[i];
        if (entry.length() < 2) continue;
        const std::string unquoted = entry.substr(1, entry.length() - 2);

        const std::vector<std::string> parts = split(unquoted, '|');
        if (parts.size() != 3 && parts.size() != 4) continue;  // e.g. "No_SW_Radio"

        FreqSetting setting;
        setting.volume = parseArmaNumberToInt(parts[1]);
        setting.stereoMode = parseArmaNumberToInt(parts[2]);
        if (parts.size() == 4) setting.radioClassname = parts[3];
        result[parts[0]] = setting;
    }
    return result;
}

// Original: PTTDelayArguments::stringToSubtype. "directSpeech" intentionally
// maps to "invalid" so direct speech never gets a radio effect chain.
const char* subtypeToFx(const std::string& subtype, bool senderUnderwater) {
    if (subtype == "digital") return senderUnderwater ? "dd" : "sw";
    if (subtype == "digital_lr") return "lr";
    if (subtype == "airborne") return "airborne";
    if (subtype == "dd") return "dd";
    if (subtype == "phone") return "phone";
    return nullptr;
}

}  // namespace

Vec3 RemoteClient::extrapolatedPosition(Clock::time_point now) const {
    if (vehicle.velocity.isNull()) return position;
    const float seconds = std::chrono::duration<float>(now - lastUpdate).count();
    if (seconds <= 0.0f || seconds > 2.0f) return position;  // sanity clamp
    return position + vehicle.velocity * seconds;
}

// ---------------------------------------------------------------------------
// Legacy SQF protocol
// ---------------------------------------------------------------------------

void State::handlePos(const std::vector<std::string>& tokens) {
    if (tokens.size() < 14) return;

    const std::string nickname = convertNickname(tokens[1]);
    if (nickname.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    RemoteClient& client = m_clients[nickname];
    client.nickname = nickname;
    client.position = parseVec3(tokens[2]);
    client.viewDirection = parseVec3(tokens[3]);
    client.canSpeak = isTrue(tokens[4]);
    client.canUseSW = isTrue(tokens[5]);
    client.canUseLR = isTrue(tokens[6]);
    client.canUseDD = isTrue(tokens[7]);
    client.vehicle = parseVehicle(tokens[8]);
    client.terrainInterception = parseArmaNumberToInt(tokens[9]);
    client.voiceVolumeMultiplier = parseArmaNumber(tokens[10]);
    client.objectInterception = parseArmaNumberToInt(tokens[11]);
    client.isSpectating = isTrue(tokens[12]);
    client.isEnemyToPlayer = isTrue(tokens[13]);
    client.dead = false;
    client.lastUpdate = Clock::now();
}

void State::handleFreq(const std::vector<std::string>& tokens) {
    if (tokens.size() < 11) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_swFrequencies = parseFrequencies(tokens[1]);
    m_lrFrequencies = parseFrequencies(tokens[2]);

    const bool wasAlive = m_alive;
    m_alive = (tokens[3] == "true");  // compared against the literal, not isTrue()
    m_speakVolumeMeters = parseArmaNumberToInt(tokens[4]);
    if (m_speakVolumeMeters <= 0) m_speakVolumeMeters = 20;
    m_myNickname = convertNickname(tokens[5]);
    m_wavesLevel = parseArmaNumber(tokens[6]);
    m_terrainInterceptionCoefficient = parseArmaNumber(tokens[7]);
    m_globalVolume = parseArmaNumber(tokens[8]);
    m_receivingDistanceMultiplicator = parseArmaNumber(tokens[9]);
    if (m_receivingDistanceMultiplicator <= 0.0f) m_receivingDistanceMultiplicator = 1.0f;
    m_speakerDistance = parseArmaNumber(tokens[10]);

    // Respawned: lift the mute that KILLED installed.
    if (!wasAlive && m_alive) queueLocalMessageLocked(std::nullopt);
}

void State::handleTangent(const std::vector<std::string>& tokens) {
    // TANGENT[_LR] \t PRESSED|RELEASED \t freq \t range \t subtype [\t classname]
    if (tokens.size() < 5) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const bool pressed = (tokens[1] == "PRESSED");
    if (pressed == m_tangentPressed && m_txFrequency == tokens[2]) return;  // nothing changed

    m_tangentPressed = pressed;
    m_tangentIsLr = (tokens[0] == "TANGENT_LR");
    m_txFrequency = tokens[2];
    m_txSubtype = tokens[4];

    if (pressed) {
        m_txRange = parseArmaNumber(tokens[3]);
        // Guards against exactly the kind of garbage/"inf" input (parseArmaNumber uses strtof,
        // which accepts "inf"/"nan") that turned into a permanent, silent voice-client crash: the
        // voice client's own NaN/Infinity guard (json::number()'s clamp to +-1e12) is still valid
        // JSON, but nowhere near a sane radio range, and the client parses this field as an Int32.
        // Clamp to something no real radio range could ever exceed instead of forwarding it as-is.
        if (!(m_txRange >= 0.0f)) m_txRange = 0.0f;  // also catches NaN (fails every comparison)
        if (m_txRange > 200000.0f) m_txRange = 200000.0f;  // 200km, generous headroom
        m_txRadioClassname = (tokens.size() > 5) ? tokens[5] : std::string();
        queueLocalMessageLocked(true);
    } else {
        m_txRange = 0.0f;
        m_txRadioClassname.clear();
        queueLocalMessageLocked(std::nullopt);
    }
}

void State::handleSpeakers(const std::vector<std::string>& tokens) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speakers.clear();
    if (tokens.size() < 2 || tokens[1].empty()) return;

    const std::vector<std::string> speakerBlocks = split(tokens[1], '\x0B');
    for (size_t i = 0; i < speakerBlocks.size(); ++i) {
        if (speakerBlocks[i].empty()) continue;

        // radio_id \n freqs \n nickname \n pos \n volume \n vehicle [\n waveZ]
        const std::vector<std::string> parts = split(speakerBlocks[i], '\x0A');
        if (parts.size() < 6) continue;

        SpeakerData speaker;
        speaker.radioId = parts[0];
        speaker.frequencies = split(parts[1], '|');
        speaker.ownerNickname = convertNickname(parts[2]);
        speaker.pos = parseVec3(parts[3]);
        speaker.volume = parseArmaNumberToInt(parts[4]);
        speaker.vehicle = parseVehicle(parts[5]);
        if (parts.size() > 6) {
            speaker.waveZ = parseArmaNumber(parts[6]);
        } else {
            speaker.waveZ = speaker.pos.isNull() ? 1.0f : speaker.pos.z;
        }
        m_speakers.push_back(speaker);
    }
}

void State::handleSetCfg(const std::vector<std::string>& tokens) {
    if (tokens.size() < 3) return;
    const std::string& key = tokens[1];
    if (!isValidConfigKey(key)) return;  // silently ignore, never pop a MessageBox

    std::string value = tokens[2];
    if (tokens.size() >= 4 && tokens[3] == "BOOL") {
        value = (value == "true" || value == "TRUE") ? "true" : "false";
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config[key] = value;
    m_haveConfig = true;  // the game answered our NEEDCFG
}

void State::handleKilled(const std::string& nickname) {
    const std::string name = convertNickname(nickname);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<std::string, RemoteClient>::iterator it = m_clients.find(name);
    if (it != m_clients.end()) it->second.dead = true;

    if (!name.empty() && name == m_myNickname) {
        m_alive = false;
        m_tangentPressed = false;
        m_txRange = 0.0f;
        m_txFrequency.clear();
        m_txRadioClassname.clear();
        queueLocalMessageLocked(false);
    }
}

void State::handleUid(const std::string& nickname, const std::string& uid) {
    const std::string name = convertNickname(nickname);
    if (name.empty() || uid.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_nameToUid[name] = uid;
}

void State::handleReleaseAllTangents(const std::string& nickname) {
    const std::string name = convertNickname(nickname);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!name.empty() && !m_myNickname.empty() && name != m_myNickname) return;

    m_tangentPressed = false;
    m_txRange = 0.0f;
    m_txFrequency.clear();
    m_txRadioClassname.clear();
    queueLocalMessageLocked(std::nullopt);
}

void State::handleAddRadioTowers(const std::string& payload) {
    if (payload.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const std::vector<std::string> entries = split(payload, '\x0A');
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].empty()) continue;
        const std::vector<std::string> fields = split(entries[i], ';');
        if (fields.size() != 3) continue;  // netID;range;position

        AntennaData antenna;
        antenna.netId = fields[0];
        antenna.range = parseArmaNumber(fields[1]);
        antenna.pos = parseVec3(fields[2]);
        if (antenna.netId.empty()) continue;

        bool replaced = false;
        for (size_t a = 0; a < m_antennas.size(); ++a) {
            if (m_antennas[a].netId == antenna.netId) {
                m_antennas[a] = antenna;
                replaced = true;
                break;
            }
        }
        if (!replaced) m_antennas.push_back(antenna);
    }
}

void State::handleDelRadioTowers(const std::string& payload) {
    if (payload.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const std::vector<std::string> entries = split(payload, '\x0A');
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].empty()) continue;
        for (size_t a = 0; a < m_antennas.size(); ++a) {
            if (m_antennas[a].netId == entries[i]) {
                m_antennas.erase(m_antennas.begin() + static_cast<std::ptrdiff_t>(a));
                break;
            }
        }
    }
}

void State::handleDataFrame() {
    // The original only bumped a frame counter here; expiry is time based in
    // this implementation, so there is nothing to do beyond keeping the hook.
}

void State::handleMissionEnd() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clients.clear();
    m_speakers.clear();
    m_antennas.clear();
    m_swFrequencies.clear();
    m_lrFrequencies.clear();
    m_remoteTx.clear();
    m_receivingFrom.clear();
    m_receivingAnyRadio = false;
    m_tangentPressed = false;
    m_txFrequency.clear();
    m_txRadioClassname.clear();
    m_txRange = 0.0f;
    m_alive = false;
    m_haveConfig = false;
    m_configRequestedAt = Clock::time_point();
    queueLocalMessageLocked(std::nullopt);
}

std::string State::speakingPair(const std::string& nickname) const {
    const std::string name = convertNickname(nickname);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<std::string, RemoteClient>::const_iterator it = m_clients.find(name);
    if (it == m_clients.end()) return "00";

    const bool isMe = (!m_myNickname.empty() && name == m_myNickname);

    bool speaking = m_talkingNames.find(name) != m_talkingNames.end();
    if (isMe) speaking = speaking || m_tangentPressed || m_statusTransmitting;

    bool receiving;
    if (isMe) {
        receiving = m_receivingAnyRadio;
    } else {
        receiving = m_receivingFrom.find(name) != m_receivingFrom.end();
    }

    std::string result;
    result.push_back(speaking ? '1' : '0');
    result.push_back(receiving ? '1' : '0');
    return result;
}

std::string State::tsInfo(const std::string& sub) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // SERVER/SERVERUID/CHANNEL are fixed: the voice-server relay has no concept of named
    // servers/channels (see docs/protocol-network.md) -- these were already vestigial even under
    // the old bridge, which never actually populated them from the real C# server either.
    if (sub == "SERVER") return "TFRS Voice Server";
    if (sub == "SERVERUID") return "TFRS";
    if (sub == "CHANNEL") return "TFRS";
    if (sub == "CHANNELID") return "0";  // no channel concept in the new server
    if (sub == "VERSION") return kPluginVersion;
    // Debug-only smoke test for the vendored libopus link (Phase 1 of the native voice port) --
    // not part of the legacy protocol, gives a trivial callExtension-based way to confirm the
    // codec actually linked before any code depends on it.
    if (sub == "OPUSVER") return opus_get_version_string();
    if (sub == "PING") return m_voiceConnected ? "PONG" : std::string();
    return "FAIL";
}

bool State::needsConfig() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_haveConfig) return false;

    const Clock::time_point now = Clock::now();
    if (m_configRequestedAt != Clock::time_point() &&
        (now - m_configRequestedAt) < std::chrono::seconds(3)) {
        return false;  // already asked, give the game a moment to answer
    }
    m_configRequestedAt = now;
    return true;
}

// ---------------------------------------------------------------------------
// Native voice port (src/Voice/VoiceSession) -- see State.h's class-level comment.
// ---------------------------------------------------------------------------

std::string State::myUid() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_myNickname.empty()) return std::string();
    std::unordered_map<std::string, std::string>::const_iterator it = m_nameToUid.find(m_myNickname);
    return it != m_nameToUid.end() ? it->second : std::string();
}

std::string State::myNickname() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_myNickname;
}

void State::setLocalTransmitting(bool transmitting) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_statusTransmitting = transmitting;
}

void State::setRemoteTx(const std::string& uid, bool active, const std::string& freq, float range,
                        const std::string& subtype) {
    if (uid.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    std::string name;
    for (std::unordered_map<std::string, std::string>::const_iterator it = m_nameToUid.begin();
         it != m_nameToUid.end(); ++it) {
        if (it->second == uid) {
            name = it->first;
            break;
        }
    }
    if (name.empty()) return;  // not yet resolved via the UID command -- nothing to attach this to

    m_everReceivedTx = true;
    if (!active || freq.empty()) {
        m_remoteTx.erase(name);
        return;
    }

    RemoteTx tx;
    tx.nickname = name;
    tx.frequency = freq;
    tx.range = range;
    tx.subtype = subtype;
    tx.received = Clock::now();
    m_remoteTx[name] = tx;
    m_talkingNames.insert(name);
}

void State::setVoiceConnected(bool connected) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_voiceConnected = connected;
}

std::optional<std::optional<bool>> State::takeTransmitOverride() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::optional<std::optional<bool>> out;
    out.swap(m_pendingTransmitOverride);
    return out;
}

void State::queueLocalMessageLocked(std::optional<bool> value) {
    m_pendingTransmitOverride = value;
}

// ---------------------------------------------------------------------------
// Snapshot computation
// ---------------------------------------------------------------------------

bool State::configBool(const char* key, bool fallback) const {
    std::unordered_map<std::string, std::string>::const_iterator it = m_config.find(key);
    if (it == m_config.end()) return fallback;
    return it->second == "true" || it->second == "1";
}

float State::configFloat(const char* key, float fallback) const {
    std::unordered_map<std::string, std::string>::const_iterator it = m_config.find(key);
    if (it == m_config.end()) return fallback;
    return parseArmaNumber(it->second);
}

const RemoteClient* State::findClientLocked(const std::string& nickname) const {
    std::unordered_map<std::string, RemoteClient>::const_iterator it = m_clients.find(nickname);
    return it == m_clients.end() ? nullptr : &it->second;
}

std::string State::uidForLocked(const std::string& nickname) const {
    std::unordered_map<std::string, std::string>::const_iterator it = m_nameToUid.find(nickname);
    if (it != m_nameToUid.end()) return it->second;
    return nickname;  // fall back to the Arma name so the client can still match
}

float State::effectiveDistance(const Vec3& myPos, const RemoteClient& other,
                               Clock::time_point now) const {
    const float raw = myPos.distanceTo(other.extrapolatedPosition(now));
    const float terrain =
        static_cast<float>(other.terrainInterception) * m_terrainInterceptionCoefficient;
    float result = raw + terrain + terrain * (raw / 2000.0f);
    result *= m_receivingDistanceMultiplicator;
    return result;
}

float State::antennaLoss(const Vec3& from, float maxDistanceToAnt, const Vec3& to) const {
    // Port of AntennaManager::findConnection + Antenna::connectionLoss.
    if (m_antennas.empty() || maxDistanceToAnt <= 0.0f) return kNoAntennaLoss;

    float lowest = 1.0f;
    bool found = false;
    for (size_t i = 0; i < m_antennas.size(); ++i) {
        const AntennaData& antenna = m_antennas[i];
        if (antenna.range <= 0.0f) continue;

        const float distToSender = antenna.pos.distanceTo(from);
        const float distToReceiver = antenna.pos.distanceTo(to);
        if (!(distToSender < maxDistanceToAnt)) continue;   // canBeReachedBy
        if (!(distToReceiver < antenna.range)) continue;    // canReach

        const float lossTo = distToReceiver / antenna.range;
        const float lossFrom = distToSender / maxDistanceToAnt;
        const float loss = lossTo + lossFrom;
        if (loss < lowest) {
            lowest = loss;
            found = true;
        }
    }
    return found ? lowest : kNoAntennaLoss;
}

void State::addAudibleForClientLocked(const RemoteClient& me, const RemoteClient& other,
                                      Clock::time_point now, std::vector<AudibleUnit>& out) {
    const Vec3 myPos = me.extrapolatedPosition(now);
    const Vec3 hisPos = other.extrapolatedPosition(now);
    const float speakVolume = static_cast<float>(m_speakVolumeMeters);

    const bool headsetLowered = configBool("headsetLowered", false);

    AudibleUnit best;
    best.nickname = other.nickname;
    bool haveBest = false;

    // Spectator rules (plugin.cpp, processVoiceData).
    const bool bothSpectating = me.isSpectating && other.isSpectating;
    if (bothSpectating) {
        if (configBool("muteSpectators", false)) return;
        AudibleUnit unit;
        unit.nickname = other.nickname;
        unit.gain = 1.0f;
        unit.az = 0.0f;
        unit.fx = "direct";
        unit.err = 0.0f;
        out.push_back(unit);
        return;
    }
    const bool notHearableInSpectator =
        me.isSpectating &&
        ((other.isEnemyToPlayer && configBool("spectatorNotHearEnemies", true)) ||
         (!other.isEnemyToPlayer && !configBool("spectatorCanHearFriendlies", true)));

    // -- 1) radio -----------------------------------------------------------
    std::unordered_map<std::string, RemoteTx>::const_iterator txIt = m_remoteTx.find(other.nickname);
    const RemoteTx* tx = nullptr;
    if (txIt != m_remoteTx.end() && (now - txIt->second.received) < kTxExpiry) {
        tx = &txIt->second;
    }

    if (tx && !other.isSpectating) {
        const float effDist = effectiveDistance(myPos, other, now);
        std::map<std::string, FreqSetting>::const_iterator swIt =
            m_swFrequencies.find(tx->frequency);
        std::map<std::string, FreqSetting>::const_iterator lrIt =
            m_lrFrequencies.find(tx->frequency);
        const bool onSw = swIt != m_swFrequencies.end();
        const bool onLr = lrIt != m_lrFrequencies.end();

        const float loss = antennaLoss(hisPos, tx->range, myPos);
        const bool inRange = (tx->range > 0.0f && effDist <= tx->range) || loss < kNoAntennaLoss;
        const bool senderUnderwater = (hisPos.z < 0.0f) && !other.canUseSW;

        if ((onSw || onLr) && inRange) {
            const FreqSetting* setting = nullptr;
            bool viaLr = false;
            if (onLr && me.canUseLR) {
                setting = &lrIt->second;
                viaLr = true;
            } else if (onSw && me.canUseSW) {
                setting = &swIt->second;
            }

            // Half duplex: we cannot receive on the radio we transmit with.
            if (setting && !configBool("full_duplex", true) && m_tangentPressed &&
                !m_txRadioClassname.empty() && setting->radioClassname == m_txRadioClassname) {
                setting = nullptr;
            }

            if (setting) {
                const char* fx = subtypeToFx(tx->subtype, senderUnderwater);
                if (fx != nullptr) {
                    float volumeLevel = volumeMultiplier(static_cast<float>(setting->volume));
                    if (headsetLowered) volumeLevel *= 0.1f;

                    AudibleUnit unit;
                    unit.nickname = other.nickname;
                    unit.fx = fx;
                    unit.az = 0.0f;  // radio is "in your ear", the original never pans it
                    unit.gain = (std::strcmp(fx, "phone") == 0) ? volumeLevel * 10.0f
                                                                : volumeLevel * 0.35f;

                    if (std::strcmp(fx, "dd") == 0) {
                        const float underwaterRange =
                            kDdMinDistance +
                            (kDdMaxDistance - kDdMinDistance) * (1.0f - m_wavesLevel);
                        const float uwDist = distanceUnderwater(myPos, hisPos);
                        const float normalDist = myPos.distanceTo(hisPos);
                        const float range = (tx->range > 1.0f) ? tx->range : 1.0f;
                        const float ddError =
                            (uwDist * (range / (underwaterRange > 0.0f ? underwaterRange : 1.0f)) +
                             (normalDist - uwDist)) /
                            range;
                        unit.err = clampf(ddError < loss ? ddError : loss, 0.0f, 1.0f);
                    } else {
                        const float range = (tx->range > 1.0f) ? tx->range : 1.0f;
                        const float distError = effDist / range;
                        unit.err = clampf(distError < loss ? distError : loss, 0.0f, 1.0f);
                    }

                    if (viaLr && setting->volume > 2) m_lrIncomingPending = true;
                    if (unit.gain > 0.0f) {
                        best = unit;
                        haveBest = true;
                    }
                }
            }

            // -- 2) external speakers on the sender's frequency --------------
            for (size_t i = 0; i < m_speakers.size(); ++i) {
                const SpeakerData& speaker = m_speakers[i];
                if (speaker.ownerNickname == other.nickname) continue;  // his own backpack

                bool matches = false;
                for (size_t f = 0; f < speaker.frequencies.size(); ++f) {
                    if (speaker.frequencies[f] == tx->frequency) {
                        matches = true;
                        break;
                    }
                }
                if (!matches) continue;

                Vec3 speakerPos = speaker.pos;
                if (speakerPos.isNull()) {
                    const RemoteClient* owner = findClientLocked(speaker.ownerNickname);
                    if (!owner) continue;
                    speakerPos = owner->extrapolatedPosition(now);
                }
                if (speakerPos.isNull()) continue;
                if (speakerPos.z < 0.0f) continue;  // speakers do not work underwater

                const bool sameVehicleAsMe = (me.vehicle.name == speaker.vehicle.name);
                const float radioVehicleLoss =
                    sameVehicleAsMe
                        ? 0.0f
                        : clampf(me.vehicle.isolation + speaker.vehicle.isolation, 0.0f, 0.99f);

                const float speakerRange = (speaker.volume < 20)
                                               ? (static_cast<float>(speaker.volume) / 10.0f) *
                                                     m_speakerDistance
                                               : static_cast<float>(speaker.volume) * 1.9f;
                const float distFromRadio = myPos.distanceTo(speakerPos);
                const bool shouldHear = other.canSpeak && me.canSpeak;

                float attenuation;
                if (radioVehicleLoss < 0.01f) {
                    attenuation = volumeAttenuation(distFromRadio, shouldHear, speakerRange);
                } else {
                    attenuation =
                        volumeAttenuation(distFromRadio, shouldHear, speakerRange,
                                          1.0f - radioVehicleLoss) *
                        std::pow(1.0f - radioVehicleLoss, 1.2f);
                }

                float volumeLevel = volumeMultiplier(static_cast<float>(speaker.volume));
                AudibleUnit unit;
                unit.nickname = other.nickname;
                unit.fx = "speaker";
                unit.gain = attenuation * volumeLevel * 0.35f;
                unit.az = azimuthTo(myPos, me.viewDirection, speakerPos);
                const float range = (tx->range > 1.0f) ? tx->range : 1.0f;
                const float distError = effDist / range;
                unit.err = clampf(distError < loss ? distError : loss, 0.0f, 1.0f);

                if (unit.gain > 0.0f && (!haveBest || unit.gain > best.gain)) {
                    best = unit;
                    haveBest = true;
                }
            }
        }
    } else if (!m_everReceivedTx && !other.isSpectating && !other.dead && me.canUseSW &&
               !m_swFrequencies.empty()) {
        // Fallback while the voice client does not report remote transmissions
        // yet: assume everybody may be on our first SW frequency so that radio
        // is at least audible during bring-up. See README.md.
        const float effDist = effectiveDistance(myPos, other, now);
        if (effDist <= kAssumedRadioRange) {
            float volumeLevel = volumeMultiplier(
                static_cast<float>(m_swFrequencies.begin()->second.volume));
            if (headsetLowered) volumeLevel *= 0.1f;

            AudibleUnit unit;
            unit.nickname = other.nickname;
            unit.fx = "sw";
            unit.gain = volumeLevel * 0.35f;
            unit.az = 0.0f;
            unit.err = clampf(effDist / kAssumedRadioRange, 0.0f, 1.0f);
            if (unit.gain > 0.0f) {
                best = unit;
                haveBest = true;
            }
        }
    }

    // -- 3) vehicle intercom -----------------------------------------------
    if (configBool("intercomEnabled", true) && tx == nullptr && other.vehicle.name != "no" &&
        other.vehicle.name == me.vehicle.name && other.vehicle.intercomSlot != -1 &&
        other.vehicle.intercomSlot == me.vehicle.intercomSlot) {
        float intercomVolume = configFloat("intercomVolume", 0.3f);
        if (headsetLowered) intercomVolume *= 0.1f;
        if (m_lrIncoming) intercomVolume *= 1.0f - configFloat("intercomDucking", 0.2f);

        AudibleUnit unit;
        unit.nickname = other.nickname;
        unit.fx = "intercom";
        unit.gain = intercomVolume;
        unit.az = 0.0f;
        unit.err = 0.0f;
        if (unit.gain > 0.0f && (!haveBest || unit.gain > best.gain)) {
            best = unit;
            haveBest = true;
        }
    }

    // -- 4) direct speech ---------------------------------------------------
    const float directDistance =
        myPos.distanceTo(hisPos) + 2.0f * static_cast<float>(other.objectInterception);
    if (!other.isSpectating && !notHearableInSpectator &&
        directDistance <= speakVolume + 15.0f) {
        const bool shouldPlayerHear = other.canSpeak && me.canSpeak;
        const float vehicleLoss =
            clampf(me.vehicle.isolation + other.vehicle.isolation, 0.0f, 0.99f);
        const bool isInSameVehicle =
            (me.vehicle.name == other.vehicle.name) && me.vehicle.name != "no";

        float gain;
        if (shouldPlayerHear) {
            if (vehicleLoss < 0.01f || isInSameVehicle) {
                gain = volumeAttenuation(directDistance, true, speakVolume);
            } else {
                gain = volumeAttenuation(directDistance, true, speakVolume, 1.0f - vehicleLoss) *
                       std::pow(1.0f - vehicleLoss, 1.2f);
            }
        } else {
            // "cannot speak" (underwater / gas mask): heavily muffled but loud,
            // the 100 Hz low pass of the original lives in the client.
            gain = volumeAttenuation(directDistance, false, speakVolume) * kCantSpeakGain;
        }
        gain *= other.voiceVolumeMultiplier;

        AudibleUnit unit;
        unit.nickname = other.nickname;
        unit.fx = "direct";
        unit.gain = gain;
        unit.az = azimuthTo(myPos, me.viewDirection, hisPos);
        unit.err = 0.0f;
        if (unit.gain > 0.0f && (!haveBest || unit.gain > best.gain)) {
            best = unit;
            haveBest = true;
        }
    }

    if (haveBest) out.push_back(best);
}

std::vector<AudibleUnit> State::computeAudibleUnits() {
    std::lock_guard<std::mutex> lock(m_mutex);

    const Clock::time_point now = Clock::now();
    std::vector<AudibleUnit> units;
    m_receivingFrom.clear();
    m_receivingAnyRadio = false;
    m_lrIncomingPending = false;

    const RemoteClient* me = m_myNickname.empty() ? nullptr : findClientLocked(m_myNickname);
    if (me != nullptr && (now - me->lastUpdate) < kClientExpiry) {
        for (std::unordered_map<std::string, RemoteClient>::const_iterator it = m_clients.begin();
             it != m_clients.end(); ++it) {
            const RemoteClient& other = it->second;
            if (other.nickname == m_myNickname) continue;
            if ((now - other.lastUpdate) >= kClientExpiry) continue;
            addAudibleForClientLocked(*me, other, now, units);
        }
    }
    m_lrIncoming = m_lrIncomingPending;

    for (size_t i = 0; i < units.size(); ++i) {
        AudibleUnit& unit = units[i];
        unit.gain *= m_globalVolume;
        if (unit.gain < 0.0f) unit.gain = 0.0f;

        if (std::strcmp(unit.fx, "direct") != 0) {
            m_receivingFrom.insert(unit.nickname);
            m_receivingAnyRadio = true;
        }

        unit.uid = uidForLocked(unit.nickname);
    }

    return units;
}

State::LocalTxState State::localTxState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    LocalTxState tx;
    tx.active = m_tangentPressed;
    if (tx.active) {
        tx.freq = m_txFrequency;
        tx.range = m_txRange;
        tx.subtype = m_txSubtype;
    }
    return tx;
}

}  // namespace tfrs
