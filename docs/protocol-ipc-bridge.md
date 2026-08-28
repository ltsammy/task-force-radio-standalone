# TFRS Addon-Bridge-Protokoll (Arma-Extension ↔ Voice-Client, lokale IPC)

Diese Verbindung existiert nur auf demselben Rechner zwischen der (neu geschriebenen) Arma-3-
Extension-DLL und dem laufenden `Tfrs.VoiceClient`-Prozess. Anders als das SQF↔Extension-Protokoll
(siehe unten) gibt es hier **keine Altlast-Kompatibilität zu wahren** — TeamSpeak kannte diese
Verbindung nicht, ich ersetze beide Enden komplett neu. Deshalb bewusst einfach gehalten: benannte
Pipe + zeilenweises JSON statt Shared Memory + Binärformat.

- Pipe-Name: `\\.\pipe\TFRS_VoiceBridge`
- Rollen: **Voice-Client** = `NamedPipeServerStream` (erstellt/lauscht), **Arma-Extension** =
  `NamedPipeClientStream` (verbindet sich, mit Retry, falls Voice-Client noch nicht läuft).
- Framing: UTF-8-Text, **eine JSON-Nachricht pro Zeile** (`\n`-terminiert). Kein Längenpräfix nötig,
  lokal verlustfrei (Named Pipes sind reliable, im Gegensatz zum UDP-Netzwerkprotokoll).
- Bei Verbindungsabbruch versucht die Extension automatisch neu zu verbinden (z. B. Voice-Client
  wurde neu gestartet); der Voice-Client akzeptiert nach einem Disconnect wieder neue Verbindungen.

## Nachrichten: Extension → Voice-Client

### `units` — periodisches Voll-Snapshot der hörbaren Einheiten

```json
{"t":"units","u":[
  {"uid":"76561198000000001","gain":0.82,"az":1.047,"muted":false,"fx":"sw","err":0.15},
  {"uid":"76561198000000002","gain":0.10,"az":-2.5,"muted":false,"fx":"direct","err":0.0}
]}
```

- `fx`: welche Effektkette der Client anwenden soll — `"direct"` (Direktsprache, keine
  Funkverzerrung), `"sw"`, `"lr"`, `"airborne"`, `"dd"` (Diver), `"phone"`, `"speaker"`
  (Boden-/Fahrzeug-Lautsprecher), `"intercom"`. Bestimmt die Filterkette aus
  [`dsp-audio-pipeline.md`](dsp-audio-pipeline.md) Abschnitt 4.
- `err`: `errorLevel` (0.0-1.0) für die Funkverzerrungskette (Foldback/Ringmod-Mix bzw. bei `"dd"`
  die Sample-Drop-Wahrscheinlichkeit) — von der Extension bereits aus Distanz/Reichweite/Antennen-
  Loss berechnet, siehe `dsp-audio-pipeline.md` Abschnitt 1 &amp; 6. Bei `fx:"direct"` ungenutzt (0).

- Wird vom Addon bei jeder relevanten Änderung gesendet (praktisch jeden Simulationsschritt bzw.
  alle 50-100 ms während aktiver Funk-/Sprachkommunikation).
- **Dies ist immer der vollständige Zustand**, kein Delta: jede UID, die in einer neuen `units`-
  Nachricht fehlt, gilt ab sofort als nicht hörbar (Gain 0). Das macht den Client selbstheilend
  gegenüber verpassten Updates, ohne dass Sequenznummern/Acks nötig wären.
- `gain`: 0.0-1.0 (ggf. >1.0 für bewusste Verstärkung), vom Addon bereits vollständig berechnet
  (Entfernungsdämpfung, Terrain-Okklusion, Antennen-/Funkqualität, Lautstärke-Setting des
  jeweiligen Funkgeräts etc.) — der Client wendet **keine eigene Entfernungsdämpfung** an.
- `az`: Azimut in Radiant relativ zur Blickrichtung des lokalen Spielers, `0` = genau vorne,
  positiv = im Uhrzeigersinn (rechts), Bereich `-π..π`. Wird vom Client in einfaches
  Equal-Power-Stereo-Panning übersetzt (kein HRTF/Binaural — reicht für klar wahrnehmbare
  Richtung, siehe `PlaybackMixer`).
- `muted`: harte Stummschaltung unabhängig von `gain` (z. B. Verschlüsselungscode passt nicht).

### `local` — optionale Sende-Übersteuerung

```json
{"t":"local","transmitOverride":null}
```

- `transmitOverride: true` erzwingt Senden unabhängig vom lokalen PTT/VAD/Dauersenden-Modus
  (z. B. spielinterne Funk-Taste).
- `transmitOverride: false` erzwingt Stummschaltung (z. B. Spieler bewusstlos/tot — entspricht dem
  alten `KILLED`-Kommando).
- `null` (oder Nachricht ganz weglassen) → der lokal im Voice-Client eingestellte Modus gilt normal.

## Nachrichten: Voice-Client → Extension

### `status` — Verbindungsstatus (bei Änderung + als Heartbeat alle ~1 s)

```json
{"t":"status","connected":true,"sessionId":42,"micMuted":false,"speakerMuted":false,"transmitting":false,"error":null}
```

Ersetzt die alte `TS_INFO PING`/`PONG`-Abfrage (`fnc_isTeamSpeakPluginEnabled.sqf`) — die
Extension beantwortet das entsprechende `callExtension`-Kommando aus dem zuletzt empfangenen
`status` statt es live über eine zweite Verbindung abzufragen.

### `roster` — bekannte Server-Teilnehmer (bei Änderung)

```json
{"t":"roster","clients":[{"uid":"76561198000000001","name":"Foo"}]}
```

Damit kann die Extension/SQF-Seite Voice-Server-Teilnehmer gegen Arma-Spieler-UIDs abgleichen,
falls das für Diagnose/UI gebraucht wird.

## Zusammenspiel mit dem SQF-Extension-Protokoll

Das SQF↔Extension-Protokoll (`callExtension`-Tab-Strings, siehe `docs/protocol-extension-legacy.md`,
sobald aus `old/` extrahiert) bleibt **byte-kompatibel** zur Originalversion — nur die interne
Implementierung der Extension-DLL ändert sich: statt Shared Memory mit dem TS3-Plugin zu teilen,
übersetzt sie SQF-Kommandos (`FREQ`, `POS`, `TANGENT`, …) in `units`/`local`-Nachrichten auf dieser
Pipe, und beantwortet synchrone SQF-Anfragen (`TS_INFO`, `IS_SPEAKING`, …) aus dem zuletzt
empfangenen `status`/`roster`-Zustand.
