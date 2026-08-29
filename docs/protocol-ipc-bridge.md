# TFRS Addon Bridge Protocol (Arma extension ↔ voice client, local IPC)

This connection only exists on the same machine, between the (newly written) Arma 3 extension DLL
and the running `Tfrs.VoiceClient` process. Unlike the SQF↔extension protocol (see below), there is
**no legacy compatibility to preserve here** — TeamSpeak never knew about this link, both ends are
brand new. So it's kept deliberately simple: a named pipe with line-delimited JSON instead of
shared memory with a binary format.

- Pipe name: `\\.\pipe\TFRS_VoiceBridge`
- Roles: **voice client** = `NamedPipeServerStream` (creates/listens), **Arma extension** =
  `NamedPipeClientStream` (connects, with retry, in case the voice client isn't running yet).
- Framing: UTF-8 text, **one JSON message per line** (`\n`-terminated). No length prefix needed —
  the pipe is loss-free locally (named pipes are reliable, unlike the UDP network protocol).
- On disconnect, the extension automatically tries to reconnect (e.g. the voice client was
  restarted); the voice client accepts new connections again after a disconnect.

## Messages: extension → voice client

### `units` — periodic full snapshot of audible units

```json
{"t":"units","u":[
  {"uid":"76561198000000001","gain":0.82,"az":1.047,"muted":false,"fx":"sw","err":0.15},
  {"uid":"76561198000000002","gain":0.10,"az":-2.5,"muted":false,"fx":"direct","err":0.0}
],"myUid":"76561198000000003","localTx":{"active":true,"freq":"31.05N","range":1500,"sub":"digital"},"addonVersion":"1.0.0"}
```

- `myUid`: the local player's own real Steam UID (`getPlayerUID`), once known (see the `UID`
  command in [`protocol-extension-legacy.md`](protocol-extension-legacy.md)) — empty string until
  then. The voice client uses this as its own relay identity, overriding whatever it started with
  (a CLI `--uid`, a stale persisted one, or a machine-name placeholder) the moment it changes —
  requiring a human to correctly type/pass their own Steam64 ID is fragile in practice (two
  different people forgot to on the same test session) and the addon always knows it authoritatively
  anyway. If this causes an identity change while already connected to the relay under a different
  UID, the client disconnects and reconnects under the corrected one.
- `fx`: which effect chain the client should apply — `"direct"` (direct speech, no radio
  distortion), `"sw"`, `"lr"`, `"airborne"`, `"dd"` (diver), `"phone"`, `"speaker"` (ground/vehicle
  loudspeaker), `"intercom"`. Determines the filter chain from
  [`dsp-audio-pipeline.md`](dsp-audio-pipeline.md) section 4.
- `err`: `errorLevel` (0.0-1.0) for the radio distortion chain (foldback/ringmod mix, or for
  `"dd"` the sample-drop probability) — already computed by the extension from distance/range/
  antenna loss, see `dsp-audio-pipeline.md` sections 1 & 6. Unused (0) when `fx:"direct"`.

- Sent by the addon on every relevant change (practically every simulation tick, or every 50-100ms
  during active radio/voice communication).
- **This is always the full state, never a delta**: any UID missing from a new `units` message is
  considered inaudible (gain 0) from that point on. This makes the client self-healing against
  missed updates without needing sequence numbers/acks.
- `gain`: 0.0-1.0 (optionally >1.0 for deliberate boost), already fully computed by the addon
  (distance attenuation, terrain occlusion, antenna/radio quality, that radio's volume setting,
  etc.) — the client applies **no distance attenuation of its own**.
- `az`: azimuth in radians relative to the local player's facing direction, `0` = straight ahead,
  positive = clockwise (right), range `-π..π`. Translated by the client into simple equal-power
  stereo panning (no HRTF/binaural — good enough for a clearly perceivable direction, see
  `PlaybackMixer`).
- `muted`: hard mute independent of `gain` (e.g. encryption key mismatch).
- `addonVersion`: `TFAR_ADDON_VERSION`, the addon's own version string (via `SETCFG addon_version`,
  which arrives after the extension's first `NEEDCFG` round-trip — empty until then). Logged by
  the voice client on first receipt purely as a diagnostic, to catch a client/server/addon version
  mismatch quickly instead of it looking like a mystery bug.
- `localTx`: `null` when we're not currently transmitting on a radio, otherwise the frequency/
  range/subtype of what we're sending (from the `TANGENT`/`TANGENT_LR` command — see
  [`protocol-extension-legacy.md`](protocol-extension-legacy.md)). The voice client relays this to
  the voice server (a `RadioTxUpdate`/`RadioTxBroadcast` packet pair, see
  [`protocol-network.md`](protocol-network.md)), and every OTHER client that receives it forwards
  it to its own local extension as a `tx` message (below) — this is the only way any extension
  finds out someone else is transmitting on a frequency, since the TeamSpeak client-to-client
  channel this used to travel over doesn't exist here. Without it, radio never has anyone to route
  audio for (a fallback heuristic exists for SW-only bring-up before this existed — see the
  extension's README "Fallback heuristic" section — but it doesn't cover LR and isn't
  frequency-accurate).

### `local` — optional transmit override

```json
{"t":"local","transmitOverride":null}
```

- `transmitOverride: true` forces transmission regardless of the local PTT/VAD/always-on mode
  (e.g. an in-game radio PTT key).
- `transmitOverride: false` forces silence (e.g. player unconscious/dead — equivalent to the old
  `KILLED` command).
- `null` (or omitting the message entirely) → the mode configured locally in the voice client
  applies normally.

## Messages: voice client → extension

### `status` — connection status (on change + as a ~1s heartbeat)

```json
{"t":"status","connected":true,"sessionId":42,"micMuted":false,"speakerMuted":false,"transmitting":false,"error":null}
```

Replaces the old `TS_INFO PING`/`PONG` query (`fnc_isTeamSpeakPluginEnabled.sqf`) — the extension
answers the corresponding `callExtension` command from the last-received `status` instead of
polling it live over a second connection.

### `roster` — known server participants (on change)

```json
{"t":"roster","clients":[{"uid":"76561198000000001","name":"Foo"}]}
```

**Not currently sent by the voice client.** Nickname↔UID resolution is handled authoritatively by
the addon itself now (the `UID` command in
[`protocol-extension-legacy.md`](protocol-extension-legacy.md), driven by Arma's own
`getPlayerUID` — no cross-referencing of display names, which aren't guaranteed unique). If a
future client ever sends this again, the extension treats it as merge-only, best-effort diagnostic
data — it will never clear or override an entry that `UID` already established.

### `tx` — who else is currently transmitting on radio (full snapshot, on change)

```json
{"t":"tx","u":[{"uid":"76561198000000001","freq":"31.05N","range":1500,"sub":"digital"}]}
```

Fed entirely by other players' `localTx` (relayed through the voice server — see above), not
anything the local extension computes. **Full snapshot, like `units`**: whoever is missing is no
longer transmitting; the extension also independently expires an entry after 1.5s in case this
stops arriving (sender disconnected without a clean "stopped" update). `sub` matches `TANGENT`'s
subtype field (`digital`, `digital_lr`, `airborne`, `dd`, `phone`).

## Interplay with the SQF extension protocol

The SQF↔extension protocol (`callExtension` tab-strings, see `docs/protocol-extension-legacy.md`)
stays **byte-compatible** with the original — only the extension DLL's internal implementation
changes: instead of sharing memory with the TS3 plugin, it translates SQF commands (`FREQ`, `POS`,
`TANGENT`, …) into `units`/`local` messages on this pipe, and answers synchronous SQF requests
(`TS_INFO`, `IS_SPEAKING`, …) from the most recently received `status`/`roster` state.
