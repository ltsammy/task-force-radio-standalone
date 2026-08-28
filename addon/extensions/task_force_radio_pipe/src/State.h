// Central, mutex protected state of the extension.
//
// Fed by the legacy SQF protocol (docs/protocol-extension-legacy.md) and by
// the bridge (docs/protocol-ipc-bridge.md).  Produces the `units` snapshot
// described in docs/dsp-audio-pipeline.md section 6.
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Util.h"

namespace tfrs {
namespace json {
class Value;
}

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

    // Two character answer of IS_SPEAKING / IS_SPEAKING_BULK.
    std::string speakingPair(const std::string& nickname) const;

    // TS_INFO sub commands.
    std::string tsInfo(const std::string& sub);

    // True while the extension still wants the game to push its configuration
    // (answered as "NEEDCFG" to DFRAME).
    bool needsConfig();

    // -- bridge -------------------------------------------------------------
    void onBridgeMessage(const json::Value& message);
    void onBridgeConnected();
    void onBridgeDisconnected();

    // Builds the `units` line (without trailing '\n'). Returns an empty string
    // when nothing changed in a way that is worth sending.
    std::string buildUnitsMessage();

    // Pending `local` message, or an empty string.
    std::string takePendingLocalMessage();

private:
    // All private helpers below assume m_mutex is already held.
    const RemoteClient* findClientLocked(const std::string& nickname) const;
    float effectiveDistance(const Vec3& myPos, const RemoteClient& other,
                            Clock::time_point now) const;
    float antennaLoss(const Vec3& from, float maxDistanceToAnt, const Vec3& to) const;
    bool configBool(const char* key, bool fallback) const;
    float configFloat(const char* key, float fallback) const;
    void queueLocalMessageLocked(const char* value);
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

    // Bridge state.
    bool m_bridgeConnected = false;
    bool m_haveStatus = false;
    bool m_statusConnected = false;
    bool m_statusTransmitting = false;
    std::string m_serverName = "TFRS Voice Server";
    std::string m_serverUid = "TFRS";
    std::string m_channelName = "TFRS";
    std::unordered_map<std::string, std::string> m_nameToUid;
    std::unordered_set<std::string> m_talkingNames;
    std::unordered_map<std::string, RemoteTx> m_remoteTx;
    bool m_everReceivedTx = false;

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

    std::string m_pendingLocalMessage;
};

}  // namespace tfrs
