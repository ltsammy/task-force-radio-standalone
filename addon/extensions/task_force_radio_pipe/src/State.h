// Central, mutex protected state of the extension.
//
// Fed by the legacy SQF protocol (docs/protocol-extension-legacy.md) and, since the native voice
// port's Phase 4, read directly by Extension.cpp's tick to drive src/Voice/VoiceSession -- no more
// named-pipe bridge/JSON in between (see docs/dsp-audio-pipeline.md section 6 for what the
// snapshot itself means; the wire format it used to travel over is retired).
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Util.h"

namespace tfrs {

using Clock = std::chrono::steady_clock;

// One entry of the SW/LR frequency list sent by `FREQ`.
struct FreqSetting {
    int volume = 0;
    int stereoMode = 0;  // 0 = stereo, 1 = leftOnly, 2 = rightOnly
    std::string radioClassname;
};

// `vehicleID` field of `POS`: "no" or netID<0x10>isolation<0x10>slot<0x10>velocity
struct VehicleDesc {
    std::string name = "no";
    float isolation = 0.0f;
    int intercomSlot = -1;
    Vec3 velocity;
};

struct RemoteClient {
    std::string nickname;
    Vec3 position;
    Vec3 viewDirection;
    bool canSpeak = true;
    bool canUseSW = false;
    bool canUseLR = false;
    bool canUseDD = false;
    VehicleDesc vehicle;
    int terrainInterception = 0;
    float voiceVolumeMultiplier = 1.0f;
    // This unit's own direct-speech range in meters (whisper/normal/yelling) -- POS's additive
    // 15th token (docs/protocol-extension-legacy.md). The audibility solver uses THIS, not the
    // local listener's own speakVolume, matching the original TS3 plugin's clientData->voiceVolume
    // (old/ts/src/plugin.cpp). Defaults to TFAR_VOLUME_NORMAL's value for third-party callers still
    // on the 14-token legacy POS format.
    float speakRangeMeters = 20.0f;
    int objectInterception = 0;
    bool isSpectating = false;
    bool isEnemyToPlayer = false;
    bool dead = false;
    Clock::time_point lastUpdate;

    // Position extrapolated with the vehicle velocity, like
    // clientData::getClientPosition() in the original.
    Vec3 extrapolatedPosition(Clock::time_point now) const;
};

// One external speaker radio from `SPEAKERS`.
struct SpeakerData {
    std::string radioId;
    std::vector<std::string> frequencies;
    std::string ownerNickname;
    Vec3 pos;  // null vector -> use the owner's position
    int volume = 0;
    VehicleDesc vehicle;
    float waveZ = 0.0f;
};

// One antenna from `RadioTwrAdd`.
struct AntennaData {
    std::string netId;
    Vec3 pos;
    float range = 0.0f;
};

// Optional bridge input: what a remote player is currently transmitting.
// Without it the extension cannot know the sender's frequency/range/subtype,
// see README.md ("Offene Punkte").
struct RemoteTx {
    std::string nickname;
    std::string frequency;
    float range = 0.0f;
    std::string subtype;  // digital | digital_lr | airborne | dd | phone
    Clock::time_point received;
};

// One line of the `units` snapshot.
struct AudibleUnit {
    std::string uid;
    std::string nickname;
    float gain = 0.0f;
    float az = 0.0f;
    bool muted = false;
    const char* fx = "direct";
    float err = 0.0f;
    int stereoMode = 0;  // 0 = stereo, 1 = leftOnly, 2 = rightOnly (from FreqSetting::stereoMode)
};

class State {
public:
    // -- legacy SQF protocol ------------------------------------------------
    void handlePos(const std::vector<std::string>& tokens);
    void handleFreq(const std::vector<std::string>& tokens);
    void handleTangent(const std::vector<std::string>& tokens);
    void handleSpeakers(const std::vector<std::string>& tokens);
    void handleSetCfg(const std::vector<std::string>& tokens);
    void handleKilled(const std::string& nickname);
    void handleReleaseAllTangents(const std::string& nickname);
    void handleAddRadioTowers(const std::string& payload);
    void handleDelRadioTowers(const std::string& payload);
    void handleDataFrame();
    void handleMissionEnd();

    // Additive, non-legacy command (not part of docs/protocol-extension-legacy.md's compatibility
    // table): the SQF side reports each unit's real getPlayerUID once, so nickname<->relay-UID
    // resolution never depends on name matching (nicknames aren't guaranteed unique; UIDs are).
    void handleUid(const std::string& nickname, const std::string& uid);

    // Two character answer of IS_SPEAKING / IS_SPEAKING_BULK.
    std::string speakingPair(const std::string& nickname) const;

    // TS_INFO sub commands.
    std::string tsInfo(const std::string& sub);

    // True while the extension still wants the game to push its configuration
    // (answered as "NEEDCFG" to DFRAME).
    bool needsConfig();

    // -- native voice port (src/Voice/VoiceSession) --------------------------
    // Extension.cpp's tick is the sole integration point between this legacy/solver module and
    // src/Voice/ -- neither depends on the other's headers, keeping the dependency graph one-way.

    // Runs the full audibility solver (same logic buildUnitsMessage used to serialize to JSON) and
    // returns one row per currently-audible remote unit, `uid` already resolved.
    std::vector<AudibleUnit> computeAudibleUnits();

    // Empty until the "UID" command (fnc_sendPlayerInfo.sqf) has run for us -- deliberately NOT
    // falling back to the nickname the way a remote unit's uid does, so a caller can tell "not
    // known yet" apart from a real UID and keep waiting instead of connecting under the wrong
    // identity.
    std::string myUid() const;
    std::string myNickname() const;

    // Outer optional: whether the override changed since the last call (nullopt = no change,
    // nothing to do). Inner optional: the new value, or nullopt to clear any override.
    std::optional<std::optional<bool>> takeTransmitOverride();

    void setLocalTransmitting(bool transmitting);
    // uid resolved to a nickname via the UID-command-populated roster; no-op if unknown. Mirrors
    // the old bridge's "tx" message semantics exactly, including marking the sender as "talking"
    // for IS_SPEAKING.
    void setRemoteTx(const std::string& uid, bool active, const std::string& freq, float range,
                     const std::string& subtype);
    // Drives TS_INFO PING -- true once VoiceSession reports the voice-server connection as up.
    void setVoiceConnected(bool connected);

    // Public, self-locking readers for the voice_-prefixed CBA settings (unlike the private
    // configBool/configFloat below, which assume m_mutex is already held by an internal solver
    // caller) -- Extension.cpp's tick reads these to drive VoiceSession every cycle.
    std::string voiceConfigString(const char* key, const std::string& fallback) const;
    float voiceConfigFloat(const char* key, float fallback) const;
    bool voiceConfigBool(const char* key, bool fallback) const;

    // What WE are currently transmitting, for VoiceSession to relay via RadioTxUpdate every tick
    // (unconditionally, not just on change -- the receiving side's 1.5s expiry depends on a steady
    // refresh stream to self-heal a single dropped UDP packet). freq/range/subtype are meaningless
    // when active is false.
    struct LocalTxState {
        bool active = false;
        std::string freq;
        float range = 0.0f;
        std::string subtype;
    };
    LocalTxState localTxState() const;

private:
    // All private helpers below assume m_mutex is already held.
    const RemoteClient* findClientLocked(const std::string& nickname) const;
    float effectiveDistance(const Vec3& myPos, const RemoteClient& other,
                            Clock::time_point now) const;
    float antennaLoss(const Vec3& from, float maxDistanceToAnt, const Vec3& to) const;
    bool configBool(const char* key, bool fallback) const;
    float configFloat(const char* key, float fallback) const;
    void queueLocalMessageLocked(std::optional<bool> value);
    void addAudibleForClientLocked(const RemoteClient& me, const RemoteClient& other,
                                   Clock::time_point now, std::vector<AudibleUnit>& out);
    std::string uidForLocked(const std::string& nickname) const;

    mutable std::mutex m_mutex;

    // Registry of every player we ever got a POS for.
    std::unordered_map<std::string, RemoteClient> m_clients;

    // Local player state (FREQ / TANGENT / SPEAKERS / RadioTwr*).
    std::string m_myNickname;
    std::map<std::string, FreqSetting> m_swFrequencies;
    std::map<std::string, FreqSetting> m_lrFrequencies;
    bool m_alive = false;
    int m_speakVolumeMeters = 20;
    float m_wavesLevel = 0.0f;
    float m_terrainInterceptionCoefficient = 7.0f;
    float m_globalVolume = 1.0f;
    float m_receivingDistanceMultiplicator = 1.0f;
    float m_speakerDistance = 20.0f;

    bool m_tangentPressed = false;
    bool m_tangentIsLr = false;
    std::string m_txFrequency;
    std::string m_txSubtype;
    std::string m_txRadioClassname;
    float m_txRange = 0.0f;

    std::vector<SpeakerData> m_speakers;
    std::vector<AntennaData> m_antennas;
    std::unordered_map<std::string, std::string> m_config;

    // Voice-connection state, set by Extension.cpp's tick from VoiceSession.
    bool m_voiceConnected = false;
    bool m_statusTransmitting = false;
    std::unordered_map<std::string, std::string> m_nameToUid;
    std::unordered_set<std::string> m_talkingNames;
    std::unordered_map<std::string, RemoteTx> m_remoteTx;

    // Config handshake.
    bool m_haveConfig = false;
    Clock::time_point m_configRequestedAt;

    // Result of the last snapshot, used to answer IS_SPEAKING.
    std::unordered_set<std::string> m_receivingFrom;
    bool m_receivingAnyRadio = false;

    // Intercom ducking looks at the previous frame: an incoming LR
    // transmission and intercom audio never come from the same sender, so the
    // information has to be carried across senders (and thus across frames).
    bool m_lrIncoming = false;
    bool m_lrIncomingPending = false;

    std::optional<std::optional<bool>> m_pendingTransmitOverride;
};

}  // namespace tfrs
