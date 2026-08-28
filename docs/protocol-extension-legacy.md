# Legacy protocol: SQF ↔ extension DLL (`"task_force_radio_pipe" callExtension`)

**This protocol is the compatibility boundary.** `addons/` stays unchanged — every SQF file keeps
calling `callExtension` exactly as in the original. The new extension DLL must implement input and
output byte-exact as described here, but internally translates everything to the bridge protocol
([`protocol-ipc-bridge.md`](protocol-ipc-bridge.md)) to the new voice client instead of TeamSpeak
shared memory. Extracted from `old/` (source: `extensions/task_force_radio_pipe/`,
`ts/src/CommandProcessor.*`, `ts/src/helpers.*`, `addons/core/functions/plugin/*.sqf`).

## Basic rule: sync vs. async

The **only** decision is made by the last character of the input string:

- Input ends in `~` → **async** (fire-and-forget). Response is immediately `"OK"`, except:
  - Command starts with `D` (DFRAME) and the extension needs fresh config → `"NEEDCFG"`
  - Command starts with `M` (MISSIONEND) → response is an **empty string** `""`
- No `~` → **sync**, the extension blocks until a response or a 1000ms timeout (then `""`).
- Extension not connected/ready → `"Not connected to TeamSpeak"` (string **deliberately left
  unchanged** — SQF checks for this exact text, see below).
- Unknown command → sync: `"UNKNOWN COMMAND"`, async: silently ignored.

Delimiters: `\t` (main fields), `0x0A` (`TF_new_line`, sub-delimiter level 2), `0x0B`
(`TF_vertical_tab`, list separator), `|` (frequency list), `;` (antenna data), `0x10` (vehicle ID).
`isTrue(s)` == `s == "true" || s == "1"`. Empty tokens from splitting are preserved.

## Command table

| Command | Sync/Async | Caller (SQF) |
|---|---|---|
| `TS_INFO` | Sync | `addons/core/functions/plugin/fnc_get{TeamSpeakServerName,TeamSpeakServerUID,TeamSpeakChannelName,TeamSpeakChannelID,TeamspeakPluginVersion}.sqf`, `fnc_isTeamSpeakPluginEnabled.sqf` |
| `POS` | Async | `fnc_preparePositionCoordinates.sqf` → `fnc_sendPlayerInfo.sqf` |
| `IS_SPEAKING` | Sync | `fnc_isSpeaking.sqf` |
| `IS_SPEAKING_BULK` | Sync | `fnc_processPlayerPositions.sqf` |
| `FREQ` | Async | `fnc_sendFrequencyInfo.sqf` |
| `KILLED` | Async | `fnc_sendPlayerKilled.sqf` |
| `TRACK` | Async | `fnc_sessionTracker.sqf`, `fnc_betaTracker.sqf` (telemetry — treat as a no-op, just return `"OK"`) |
| `DFRAME` | Async | `fnc_pluginNextDataFrame.sqf` |
| `SPEAKERS` | Async | `fnc_sendSpeakerRadios.sqf` |
| `TANGENT` / `TANGENT_LR` | Async | `fnc_doSRTransmit(End).sqf`, `fnc_doLRTransmit(End).sqf`, `fnc_onSpeakVolumeModifierPressed/Released.sqf` (direct speech: freq=`"directSpeechFreq"`, subtype=`"directSpeech"`) |
| `RELEASE_ALL_TANGENTS` | Async | `fnc_releaseAllTangents.sqf` |
| `SETCFG` | Async | `fnc_setPluginSetting.sqf` |
| `MISSIONEND` | Async | `fnc_onMissionEnd.sqf` |
| `RadioTwrAdd` / `RadioTwrDel` | Async | `fnc_pluginAddRadioTower.sqf`, `fnc_pluginRemoveRadioTower.sqf` (max 50 towers per call!) |
| `collectDebugInfo` | Async | `fnc_initKeybinds.sqf` — diagnostics, a no-op returning `"OK"` is enough |

**Limits:** max 2044 bytes per message, 300 async slots in the original shared memory (no longer
relevant for the new implementation since we have no fixed shared-memory size, but SQF still sends
radio towers in batches of 50 — just handle it the same way).

### `TS_INFO` (sync) — health check & metadata

`TS_INFO\t<SUB>` → `SUB` ∈ `{SERVER, SERVERUID, CHANNEL, CHANNELID, PING, VERSION}`.

- `PING` → **must** return `"PONG"` — this is `fnc_isTeamSpeakPluginEnabled.sqf`, the central "is
  the voice client active" check. Answer from the most recently received bridge `status`
  (`connected: true` → `PONG`, otherwise timeout/empty).
- `SERVER`, `SERVERUID`, `CHANNEL` → raw strings, no TS semantics needed; sensible values: the
  server name or a fixed ID of the voice server the client is connected to (from `status`/
  connection metadata). Can be answered with reasonable placeholders as long as they aren't
  empty/an error (no SQF code evaluates the content structurally, only whether a string comes
  back).
- `CHANNELID` → decimal uint64 string. There is no channel concept in the new server anymore (1
  password = 1 "room") → return the constant `"0"`.
- `VERSION` → plugin version string (free choice, e.g. the voice client's version).

### `POS` (async, 13 fields) — **must always return `"OK"`**

```
POS \t nickname \t [x,y,z] \t [dx,dy,dz] \t canSpeak \t canUseSW \t canUseLR \t canUseDD(0/1) \t vehicleID \t terrainInterception \t voiceVolume \t objectInterception \t isSpectating \t isEnemyToPlayer ~
```

`vehicleID` = `"no"` or `netID<0x10>isolation<0x10>intercomSlot<0x10>velocity`. Any response other
than exactly `"OK"` triggers a visible hint popup for the player (`fnc_sendPlayerInfo.sqf`) — this
is the single most important rule from the whole research pass.

### `IS_SPEAKING` / `IS_SPEAKING_BULK` (sync)

- `IS_SPEAKING\t<nick>` → exactly 2 characters: `[0]` = currently speaking (`0`/`1`), `[1]` =
  currently being received/heard (`0`/`1`). Unknown client → `"00"`.
- `IS_SPEAKING_BULK\t<nick1>\t<nick2>\t…` → one 2-character pair + `\t` per player, **including
  the last element** (trailing tab). The number of pairs must exactly match the number of
  requested names, or SQF discards the whole result.

### `FREQ` (async, 10 fields)

```
FREQ \t swFreqArray \t lrFreqArray \t alive \t speakVolume \t nickname \t waves \t terrainInterceptionCoefficient \t globalVolume \t receivingDistanceMultiplicator \t speakerDistance ~
```

`alive` is compared **exactly** against the string `"true"` (not `isTrue`). Frequency array
element: `"<freq><radioCode>|<volume 0..10>|<stereoMode 0/1/2>|<radioClassname>"`, comma-separated
inside `[...]`. `stereoMode`: `0=stereo, 1=leftOnly, 2=rightOnly`. No SW/LR available →
`["No_SW_Radio"]` or `["No_LR_Radio"]` respectively.

### `TANGENT` / `TANGENT_LR` (async) — PTT state

```
TANGENT[_LR] \t PRESSED|RELEASED \t <freq+radioCode> \t <rangeMeters> \t <subtype> [\t <radioClassname>, PRESSED only]
```

`subtype` ∈ `digital` (SW), `digital_lr` (LR), `airborne`, `dd` (diver), `phone`, `directSpeech`
(direct speech — **not** recognized as its own effect type, since the original code distinguishes
by string **length** and `"directSpeech"` (length 12) falls through to `invalid` → direct speech
effectively gets none of the radio effects, only the plain 3D distance chain). Only sent in
multiplayer (`isMultiplayer`).

### `SPEAKERS` (async) — external radio loudspeakers (vehicles, dropped radios)

```
SPEAKERS \t speaker(0x0B)speaker(0x0B)... ~
speaker = radio_id(0x0A)freq|freq|...(0x0A)nickname(0x0A)[x,y,z]|[](0x0A)volume(0x0A)vehicle[(0x0A)waveZ]
```

Empty `tokens[1]` → clear the speaker list (no error response needed).

### `SETCFG` (async) — plugin configuration

```
SETCFG \t key \t value \t STRING|BOOL|SCALAR ~
```

20 valid keys (from `fnc_sendPluginConfig.sqf`, sent in response to `NEEDCFG`):
`full_duplex, addon_version, serious_channelName, serious_channelPassword, intercomVolume,
intercomEnabled, pluginTimeout, headsetLowered, spectatorNotHearEnemies,
spectatorCanHearFriendlies, tangentReleaseDelay, moveWhileTabbedOut, intercomDucking,
minimumPluginVersion, objectInterceptionStrength, voiceCone, allowDebugging,
noAutomoveSpectator, disableAutomaticMute, muteSpectators`. `minimumPluginVersion` comes first and
the extension should simply accept it (no forced-update behavior needed like in the original).

### `DFRAME` (async) — heartbeat & the only back-channel

`DFRAME~` — response is normally `"OK"`. Responding `"NEEDCFG"` triggers `fnc_sendPluginConfig.sqf`
on the SQF side, which fires all 21 `SETCFG` calls. The extension should answer `NEEDCFG` once on
first start (or whenever it has lost its configuration, e.g. after the voice client reconnects) to
get the current configuration from the game.

### `KILLED`, `RELEASE_ALL_TANGENTS`, `MISSIONEND`, `RadioTwrAdd`/`RadioTwrDel`, `collectDebugInfo`

All async, `"OK"` (or an empty string for `MISSIONEND`) as the response. `KILLED`/
`RELEASE_ALL_TANGENTS` should internally set the transmit override in the bridge protocol
(`transmitOverride:false` on KILLED, cleared on RELEASE_ALL_TANGENTS). `RadioTwrAdd`/`RadioTwrDel`
maintain a local antenna list in the extension (for later range/loss computation, if implemented —
see `docs/dsp-audio-pipeline.md`, antenna loss formula).

## Key pitfalls (don't forget)

1. `~` is the entire sync/async distinguishing mechanism — no header, no length field.
2. `POS` **always** replies `"OK"`, otherwise the player gets a hint popup.
3. `IS_SPEAKING_BULK` has a trailing tab after the last pair.
4. `MISSIONEND` replies with an empty string, not `"OK"`.
5. Keep `"Not connected to TeamSpeak"` as the error text in case SQF code (now or in community
   addons) matches against it — when in doubt, use the exact original text.
