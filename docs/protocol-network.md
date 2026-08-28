# TFRS Netzwerkprotokoll (Voice-Client ↔ Voice-Server)

UDP, alle Mehrbyte-Felder **little-endian**. Kein TCP-Fallback — Ziel ist minimale Latenz, nicht
Zustellgarantie. Implementiert in `voice-server/src/Tfrs.VoiceServer/Protocol.cs` und
`voice-client/src/Tfrs.VoiceClient/Networking/Protocol.cs` (beide Dateien müssen synchron gehalten
werden, es gibt aktuell kein gemeinsames Shared-Projekt).

Jedes Paket beginnt mit einem `byte PacketType`. Strings sind `byte length` + UTF-8-Bytes
(max. 255 Byte, praktisch aber viel kürzer begrenzt, siehe Tabelle).

Protokollversion: `1` (`Protocol.VersionMajor`).

## Konstanten

| Konstante | Wert |
|---|---|
| `MaxUdpPayload` | 1200 Byte (bleibt sicher unter typischer MTU, keine Fragmentierung) |
| `MaxNameLength` | 32 |
| `MaxUidLength` | 40 |
| `MaxOpusFrameLength` | 500 Byte |

## Pakettypen

| Wert | Name | Richtung |
|---|---|---|
| 1 | ConnectRequest | Client → Server |
| 2 | ConnectAccept | Server → Client |
| 3 | ConnectReject | Server → Client |
| 4 | Disconnect | beide |
| 5 | Ping | Client → Server |
| 6 | Pong | Server → Client |
| 7 | VoiceUp | Client → Server |
| 8 | VoiceDown | Server → Client |
| 9 | ClientJoined | Server → Client |
| 10 | ClientLeft | Server → Client |
| 11 | RosterRequest | Client → Server |

### 1 — ConnectRequest (Client → Server)

```
byte  packetType = 1
byte  versionMajor
byte  passwordHash[32]      // SHA-256(UTF8(password)), roh, nicht hex-kodiert
byte  uidLen; byte[] uid    // Arma Player-UID (z. B. Steam64-ID als String) — max 40 Byte
byte  nameLen; byte[] name  // Anzeigename — max 32 Byte
```

Der Server vergleicht `passwordHash` konstant-zeitig (`CryptographicOperations.FixedTimeEquals`)
gegen `SHA256(TFRS_PASSWORD)`. Das ist **kein** starkes Auth-Schema (kein Salt, kein Replay-Schutz) —
ausreichend für einen privaten, passwortgeschützten Spiel-Voice-Relay, aber nicht für öffentliche,
feindliche Netzwerke gedacht.

`uid` ist der stabile Schlüssel, über den der Arma-Addon-seitige Bridge-Client (siehe
[`protocol-ipc-bridge.md`](protocol-ipc-bridge.md)) eine `sessionId` einer Spieleinheit zuordnet.

### 2 — ConnectAccept (Server → Client)

```
byte   packetType = 2
uint32 sessionId       // vom Server vergebene, für die Verbindung eindeutige ID
uint16 maxClients
```

Direkt danach schickt der Server für jeden bereits verbundenen Client ein `ClientJoined`-Paket
(kein Bulk-Snapshot, um MTU-Probleme bei 200-300 Clients zu vermeiden).

### 3 — ConnectReject (Server → Client)

```
byte packetType = 3
byte reason   // 1=BadPassword, 2=ServerFull, 3=VersionMismatch, 4=BadRequest
```

### 4 — Disconnect

Client → Server: `byte packetType = 4` (kein weiteres Feld) — sauberer Verbindungsabbau.
Server → Client wird aktuell nicht aktiv gesendet (Timeout/Kick sind aus Zeitgründen nicht
implementiert — geplante Erweiterung, siehe `DisconnectReason`-Enum im Code).

### 5 — Ping (Client → Server)

```
byte   packetType = 5
uint32 clientTimestampMs   // beliebiger monotoner Client-Zeitstempel, wird nur gespiegelt
```

Aktualisiert serverseitig `LastSeenUtc` der Session (Timeout-Schutz auch ohne aktive Sprachübertragung).
Client sollte alle 5-10 s pingen.

### 6 — Pong (Server → Client)

```
byte   packetType = 6
uint32 echoedTimestampMs
```

RTT = `now - echoedTimestampMs` (Client-seitig gemessen).

### 7 — VoiceUp (Client → Server)

```
byte   packetType = 7
uint16 sequence      // Client-lokaler, fortlaufender Sequenzzähler
byte   flags         // Bit0 = LastFrame (letztes Frame einer PTT-Übertragung)
byte[] opusPayload   // 1 Opus-Frame, max. 500 Byte
```

Der Server identifiziert den Sender über die UDP-Absender-`IPEndPoint` (muss zuvor per
ConnectRequest registriert worden sein), **nicht** über ein Feld im Paket.

### 8 — VoiceDown (Server → Client, Relay von VoiceUp)

```
byte   packetType = 8
uint32 senderSessionId
uint16 sequence
byte   flags
byte[] opusPayload
```

Wird 1:1 an alle anderen verbundenen Clients weitergeleitet (fire-and-forget, keine Zustellgarantie,
kein Server-seitiger Jitter-Buffer). Client muss pro `senderSessionId` einen eigenen Opus-Decoder-
State und Jitter-Buffer halten (Opus-Decoder sind nicht zustandslos — Packet-Loss-Concealment
funktioniert nur mit kontinuierlichem State pro Stream).

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

Server antwortet mit einem `ClientJoined`-Paket pro aktuell verbundenem Client (an den Anfragenden).
Sicherheitsnetz gegen verlorene `ClientJoined`/`ClientLeft`-Broadcasts (UDP ist unzuverlässig) —
Client sollte das z. B. alle 15-30 s aufrufen und lokal gegen seine Roster-Liste abgleichen.

## Bewusste Vereinfachungen (MVP)

- Kein Reliable-UDP-Layer für Control-Pakete (Connect/Roster). Mitigiert durch Client-seitige
  Retries (ConnectRequest) bzw. periodisches `RosterRequest`.
- Kein serverseitiger Kick/Timeout-Broadcast mit Grund an den betroffenen Client selbst (er merkt
  es implizit am Fehlen weiterer Pong-Antworten).
- Passwort-Hash ohne Salt/Nonce — kein Schutz gegen Offline-Rainbow-Table, ausreichend für den
  Bedrohungsgrad "privater Spielserver".
