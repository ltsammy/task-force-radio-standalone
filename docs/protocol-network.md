# TFRS Network Protocol (voice client ↔ voice server)

UDP, all multi-byte fields **little-endian**. No TCP fallback — the goal is minimal latency, not
delivery guarantees. Implemented in `voice-server/src/Tfrs.VoiceServer/Protocol.cs` and
`voice-client/src/Tfrs.VoiceClient/Networking/Protocol.cs` (both files must be kept in sync by
hand — there is currently no shared project between them).

Every packet starts with a `byte PacketType`. Strings are `byte length` + UTF-8 bytes (max 255
bytes, but much shorter in practice — see table).

Protocol version: `1` (`Protocol.VersionMajor`).

## Constants

| Constant | Value |
|---|---|
| `MaxUdpPayload` | 1200 bytes (safely under typical MTU, no fragmentation) |
| `MaxNameLength` | 32 |
| `MaxUidLength` | 40 |
| `MaxOpusFrameLength` | 500 bytes |
| `MaxFreqLength` | 16 |
| `MaxSubtypeLength` | 16 |

## Packet types

| Value | Name | Direction |
|---|---|---|
| 1 | ConnectRequest | Client → Server |
| 2 | ConnectAccept | Server → Client |
| 3 | ConnectReject | Server → Client |
| 4 | Disconnect | both |
| 5 | Ping | Client → Server |
| 6 | Pong | Server → Client |
| 7 | VoiceUp | Client → Server |
| 8 | VoiceDown | Server → Client |
| 9 | ClientJoined | Server → Client |
| 10 | ClientLeft | Server → Client |
| 11 | RosterRequest | Client → Server |
| 12 | RadioTxUpdate | Client → Server |
| 13 | RadioTxBroadcast | Server → Client |

### 1 — ConnectRequest (Client → Server)

```
byte  packetType = 1
byte  versionMajor
byte  passwordHash[32]      // SHA-256(UTF8(password)), raw, not hex-encoded
byte  uidLen; byte[] uid    // Arma player UID (e.g. Steam64 ID as a string) — max 40 bytes
byte  nameLen; byte[] name  // display name — max 32 bytes
```

The server compares `passwordHash` in constant time (`CryptographicOperations.FixedTimeEquals`)
against `SHA256(TFRS_PASSWORD)`. This is **not** a strong auth scheme (no salt, no replay
protection) — good enough for a private, password-protected game voice relay, not meant for
public, hostile networks.

`uid` is the stable key the Arma-addon-side bridge client (see
[`protocol-ipc-bridge.md`](protocol-ipc-bridge.md)) uses to map a `sessionId` to a game unit.

### 2 — ConnectAccept (Server → Client)

```
byte   packetType = 2
uint32 sessionId       // server-assigned, unique for this connection
uint16 maxClients
byte   debugForceAudible // 0/1 -- see ServerOptions.DebugForceAudible; server-dictated only
```

Immediately after, the server sends one `ClientJoined` packet per already-connected client (no
bulk snapshot, to avoid MTU issues at 200-300 clients).

`debugForceAudible`, when set, tells the client to play every remote source at full volume/no
panning and ignore whatever the Arma extension's bridge otherwise computes (see
`RemoteSourceState.DebugAudible` / `PlaybackEngine.DebugForceAudible` client-side) — a way to test
raw mic-to-speaker transport independent of the addon. It is controlled only by the server's
`TFRS_DEBUG_FORCE_AUDIBLE` env var; the client has no way to request or enable it itself (a
client-side toggle would let a player hear others regardless of real in-game distance).

### 3 — ConnectReject (Server → Client)

```
byte packetType = 3
byte reason   // 1=BadPassword, 2=ServerFull, 3=VersionMismatch, 4=BadRequest
```

### 4 — Disconnect

Client → Server: `byte packetType = 4` (no further fields) — clean connection teardown.
Server → Client is not currently sent actively (timeout/kick weren't implemented due to time
constraints — planned extension, see the `DisconnectReason` enum in the code).

### 5 — Ping (Client → Server)

```
byte   packetType = 5
uint32 clientTimestampMs   // arbitrary monotonic client timestamp, only echoed back
```

Updates the session's `LastSeenUtc` server-side (timeout protection even without active voice
transmission). The client should ping every 5-10s.

### 6 — Pong (Server → Client)

```
byte   packetType = 6
uint32 echoedTimestampMs
```

RTT = `now - echoedTimestampMs` (measured client-side).

### 7 — VoiceUp (Client → Server)

```
byte   packetType = 7
uint16 sequence      // client-local, monotonically increasing sequence counter
byte   flags         // bit0 = LastFrame (last frame of a PTT transmission)
byte[] opusPayload   // 1 Opus frame, max 500 bytes
```

The server identifies the sender via the UDP sender `IPEndPoint` (must have registered previously
via ConnectRequest), **not** via a field in the packet.

### 8 — VoiceDown (Server → Client, relay of VoiceUp)

```
byte   packetType = 8
uint32 senderSessionId
uint16 sequence
byte   flags
byte[] opusPayload
```

Forwarded 1:1 to every other connected client (fire-and-forget, no delivery guarantee, no
server-side jitter buffer). The client must keep its own Opus decoder state and jitter buffer per
`senderSessionId` (Opus decoders aren't stateless — packet-loss concealment only works with
continuous per-stream state).

### 9 — ClientJoined (Server → Client)

```
byte   packetType = 9
uint32 sessionId
byte   uidLen; byte[] uid
byte   nameLen; byte[] name
```

### 10 — ClientLeft (Server → Client)

```
byte   packetType = 10
uint32 sessionId
```

### 11 — RosterRequest (Client → Server)

```
byte packetType = 11
```

The server replies with one `ClientJoined` packet per currently connected client (sent to the
requester). Safety net against lost `ClientJoined`/`ClientLeft` broadcasts (UDP is unreliable) —
the client should call this e.g. every 15-30s and reconcile against its local roster list.

### 12 — RadioTxUpdate (Client → Server)

```
byte   packetType = 12
byte   active        // 0/1
byte   freqLen; byte[] freq   // max 16 bytes, e.g. "31.05N"
uint16 range          // meters, rounded
byte   subLen; byte[] sub     // digital | digital_lr | airborne | dd | phone — max 16 bytes
```

The client's own local radio-transmit state (TANGENT + FREQ, from its Arma extension bridge —
see [`protocol-ipc-bridge.md`](protocol-ipc-bridge.md)'s `localTx`), sent whenever it changes.
`freq`/`range`/`sub` are meaningless when `active=0`.

### 13 — RadioTxBroadcast (Server → Client, relay of RadioTxUpdate)

```
byte   packetType = 13
uint32 senderSessionId
byte   active
byte   freqLen; byte[] freq
uint16 range
byte   subLen; byte[] sub
```

Forwarded 1:1 to every other connected client, same fire-and-forget pattern as VoiceDown — the
server has no concept of frequencies or who can hear whom, it's purely relaying metadata so each
receiving client can forward it to its own local extension as a `tx` message. Without this, no
extension anywhere ever learns who else is transmitting on a frequency, since the TeamSpeak
client-to-client channel this used to travel over doesn't exist here (see
[`protocol-ipc-bridge.md`](protocol-ipc-bridge.md) and the extension's README "real gap" note).

## Deliberate simplifications (MVP)

- No reliable-UDP layer for control packets (Connect/Roster). Mitigated by client-side retries
  (ConnectRequest) and periodic `RosterRequest`.
- No server-side kick/timeout broadcast with a reason to the affected client itself (it notices
  implicitly by the absence of further Pong replies).
- Password hash without salt/nonce — no protection against offline rainbow tables, adequate for
  the threat level of a "private game server".
