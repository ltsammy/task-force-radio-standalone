# Legacy-Protokoll: SQF ↔ Extension-DLL (`"task_force_radio_pipe" callExtension`)

**Dieses Protokoll ist die Kompatibilitätsgrenze.** `addons/` bleibt unverändert — jede SQF-Datei
ruft weiterhin exakt so `callExtension` auf wie im Original. Die neue Extension-DLL muss Input und
Output byte-exakt wie hier beschrieben implementieren, übersetzt intern aber alles auf das
Bridge-Protokoll ([`protocol-ipc-bridge.md`](protocol-ipc-bridge.md)) zum neuen Voice-Client statt
auf TeamSpeak-Shared-Memory. Extrahiert aus `old/` (Quelle: `extensions/task_force_radio_pipe/`,
`ts/src/CommandProcessor.*`, `ts/src/helpers.*`, `addons/core/functions/plugin/*.sqf`).

## Grundregel: Sync vs. Async

Die **einzige** Entscheidung fällt am letzten Zeichen des Input-Strings:

- Endet der Input auf `~` → **Async** (fire-and-forget). Antwort ist sofort `"OK"`, außer:
  - Command beginnt mit `D` (DFRAME) und die Extension braucht frische Config → `"NEEDCFG"`
  - Command beginnt mit `M` (MISSIONEND) → Antwort ist **Leerstring** `""`
- Kein `~` → **Sync**, Extension blockiert bis Antwort oder 1000 ms Timeout (dann `""`).
- Extension nicht verbunden/bereit → `"Not connected to TeamSpeak"` (String **bewusst unverändert**
  lassen — SQF prüft exakt auf diesen Text, siehe unten).
- Unbekannter Command → Sync: `"UNKNOWN COMMAND"`, Async: still ignorieren.

Delimiter: `\t` (Hauptfelder), `0x0A` (`TF_new_line`, Sub-Delimiter Ebene 2), `0x0B`
(`TF_vertical_tab`, Listentrenner), `|` (Frequenzliste), `;` (Antennendaten), `0x10` (VehicleID).
`isTrue(s)` == `s == "true" || s == "1"`. Leere Tokens beim Split bleiben erhalten.

## Befehlstabelle

| Command | Sync/Async | Aufrufer (SQF) |
|---|---|---|
| `TS_INFO` | Sync | `addons/core/functions/plugin/fnc_get{TeamSpeakServerName,TeamSpeakServerUID,TeamSpeakChannelName,TeamSpeakChannelID,TeamspeakPluginVersion}.sqf`, `fnc_isTeamSpeakPluginEnabled.sqf` |
| `POS` | Async | `fnc_preparePositionCoordinates.sqf` → `fnc_sendPlayerInfo.sqf` |
| `IS_SPEAKING` | Sync | `fnc_isSpeaking.sqf` |
| `IS_SPEAKING_BULK` | Sync | `fnc_processPlayerPositions.sqf` |
| `FREQ` | Async | `fnc_sendFrequencyInfo.sqf` |
| `KILLED` | Async | `fnc_sendPlayerKilled.sqf` |
| `TRACK` | Async | `fnc_sessionTracker.sqf`, `fnc_betaTracker.sqf` (Telemetrie — als No-Op behandeln, nur `"OK"` liefern) |
| `DFRAME` | Async | `fnc_pluginNextDataFrame.sqf` |
| `SPEAKERS` | Async | `fnc_sendSpeakerRadios.sqf` |
| `TANGENT` / `TANGENT_LR` | Async | `fnc_doSRTransmit(End).sqf`, `fnc_doLRTransmit(End).sqf`, `fnc_onSpeakVolumeModifierPressed/Released.sqf` (Direktsprache: Freq=`"directSpeechFreq"`, Subtype=`"directSpeech"`) |
| `RELEASE_ALL_TANGENTS` | Async | `fnc_releaseAllTangents.sqf` |
| `SETCFG` | Async | `fnc_setPluginSetting.sqf` |
| `MISSIONEND` | Async | `fnc_onMissionEnd.sqf` |
| `RadioTwrAdd` / `RadioTwrDel` | Async | `fnc_pluginAddRadioTower.sqf`, `fnc_pluginRemoveRadioTower.sqf` (max. 50 Towers/Call!) |
| `collectDebugInfo` | Async | `fnc_initKeybinds.sqf` — Diagnose, No-Op mit `"OK"` reicht |

**Limits:** max. 2044 Byte pro Nachricht, 300 Async-Slots im Original-Shared-Memory (für die neue
Implementierung nicht mehr relevant, da wir keine feste Shared-Memory-Größe haben, aber SQF sendet
weiterhin in 50er-Batches bei Radio-Towers — einfach mitverarbeiten).

### `TS_INFO` (Sync) — Health-Check & Metadaten

`TS_INFO\t<SUB>` → `SUB` ∈ `{SERVER, SERVERUID, CHANNEL, CHANNELID, PING, VERSION}`.

- `PING` → **muss** `"PONG"` liefern — das ist `fnc_isTeamSpeakPluginEnabled.sqf`, der zentrale
  "ist der Voice-Client aktiv"-Check. Aus dem zuletzt empfangenen Bridge-`status` beantworten
  (`connected: true` → `PONG`, sonst Timeout/leer).
- `SERVER`, `SERVERUID`, `CHANNEL` → rohe Strings, keine TS-Semantik nötig; sinnvoll: Servername
  bzw. eine feste ID des Voice-Servers, an den der Client verbunden ist (aus `status`/Verbindungs-
  Metadaten). Können mit sinnvollen Platzhaltern beantwortet werden, solange sie nicht leer/Fehler
  sind (kein SQF-Code wertet den Inhalt strukturell aus, nur ob ein String zurückkommt).
- `CHANNELID` → dezimaler uint64-String. Es gibt kein Channel-Konzept mehr im neuen Server (1
  Passwort = 1 "Raum") → konstant `"0"` liefern.
- `VERSION` → Plugin-Versionsstring (frei wählbar, z. B. Voice-Client-Version).

### `POS` (Async, 13 Felder) — **muss immer `"OK"` liefern**

```
POS \t nickname \t [x,y,z] \t [dx,dy,dz] \t canSpeak \t canUseSW \t canUseLR \t canUseDD(0/1) \t vehicleID \t terrainInterception \t voiceVolume \t objectInterception \t isSpectating \t isEnemyToPlayer ~
```

`vehicleID` = `"no"` oder `netID<0x10>isolation<0x10>intercomSlot<0x10>velocity`. Jede andere
Antwort als exakt `"OK"` erzeugt einen sichtbaren Hint beim Spieler (`fnc_sendPlayerInfo.sqf`) —
das ist die wichtigste Einzel-Regel aus der ganzen Recherche.

### `IS_SPEAKING` / `IS_SPEAKING_BULK` (Sync)

- `IS_SPEAKING\t<nick>` → exakt 2 Zeichen: `[0]` = spricht gerade (`0`/`1`), `[1]` = wird gerade
  empfangen/gehört (`0`/`1`). Unbekannter Client → `"00"`.
- `IS_SPEAKING_BULK\t<nick1>\t<nick2>\t…` → pro Spieler ein 2-Zeichen-Paar + `\t`, **auch beim
  letzten Element** (abschließender Tab). Anzahl der Paare muss exakt der Anzahl angefragter Namen
  entsprechen, sonst verwirft SQF das gesamte Ergebnis.

### `FREQ` (Async, 10 Felder)

```
FREQ \t swFreqArray \t lrFreqArray \t alive \t speakVolume \t nickname \t waves \t terrainInterceptionCoefficient \t globalVolume \t receivingDistanceMultiplicator \t speakerDistance ~
```

`alive` wird **exakt** gegen den String `"true"` verglichen (kein `isTrue`). Frequenz-Array-Element:
`"<freq><radioCode>|<volume 0..10>|<stereoMode 0/1/2>|<radioClassname>"`, kommagetrennt in `[...]`.
`stereoMode`: `0=stereo, 1=leftOnly, 2=rightOnly`. Kein SW/LR verfügbar → `["No_SW_Radio"]` bzw.
`["No_LR_Radio"]`.

### `TANGENT` / `TANGENT_LR` (Async) — PTT-Status

```
TANGENT[_LR] \t PRESSED|RELEASED \t <freq+radioCode> \t <rangeMeters> \t <subtype> [\t <radioClassname>, nur bei PRESSED]
```

`subtype` ∈ `digital` (SW), `digital_lr` (LR), `airborne`, `dd` (Diver), `phone`, `directSpeech`
(Direktsprache — wird **nicht** als eigener Effekt-Typ erkannt, da Originalcode nach String-**Länge**
unterscheidet und `"directSpeech"` mit Länge 12 auf `invalid` fällt → Direktsprache bekommt effektiv
keinen der Funkeffekte, nur die reine 3D-Distanzkette). Nur im Multiplayer gesendet (`isMultiplayer`).

### `SPEAKERS` (Async) — externe Funklautsprecher (Fahrzeuge, abgelegte Radios)

```
SPEAKERS \t speaker(0x0B)speaker(0x0B)... ~
speaker = radio_id(0x0A)freq|freq|...(0x0A)nickname(0x0A)[x,y,z]|[](0x0A)volume(0x0A)vehicle[(0x0A)waveZ]
```

Leerer `tokens[1]` → Speaker-Liste löschen (keine Fehlermeldung nötig).

### `SETCFG` (Async) — Plugin-Konfiguration

```
SETCFG \t key \t value \t STRING|BOOL|SCALAR ~
```

20 gültige Keys (aus `fnc_sendPluginConfig.sqf`, gesendet als Reaktion auf `NEEDCFG`):
`full_duplex, addon_version, serious_channelName, serious_channelPassword, intercomVolume,
intercomEnabled, pluginTimeout, headsetLowered, spectatorNotHearEnemies,
spectatorCanHearFriendlies, tangentReleaseDelay, moveWhileTabbedOut, intercomDucking,
minimumPluginVersion, objectInterceptionStrength, voiceCone, allowDebugging,
noAutomoveSpectator, disableAutomaticMute, muteSpectators`. `minimumPluginVersion` kommt zuerst
und muss von der Extension einfach akzeptiert werden (kein Update-Zwang wie im Original nötig).

### `DFRAME` (Async) — Herzschlag & einziger Rückkanal

`DFRAME~` — Antwort normalerweise `"OK"`. Antwort `"NEEDCFG"` löst serverseitig (SQF)
`fnc_sendPluginConfig.sqf` aus, das alle 21 `SETCFG`-Calls feuert. Die Extension sollte beim ersten
Start (oder wenn sie ihre Konfiguration verloren hat, z. B. nach Neuverbindung des Voice-Clients)
einmalig `NEEDCFG` antworten, um die aktuelle Konfiguration vom Spiel zu bekommen.

### `KILLED`, `RELEASE_ALL_TANGENTS`, `MISSIONEND`, `RadioTwrAdd`/`RadioTwrDel`, `collectDebugInfo`

Alle Async, `"OK"` (bzw. bei `MISSIONEND` Leerstring) als Antwort. `KILLED`/`RELEASE_ALL_TANGENTS`
sollten intern das Sende-Override im Bridge-Protokoll setzen (`transmitOverride:false` bei KILLED,
Zurücksetzen bei RELEASE_ALL_TANGENTS). `RadioTwrAdd`/`RadioTwrDel` pflegen eine lokale Antennen-
Liste in der Extension (für spätere Reichweiten-/Loss-Berechnung, falls implementiert — siehe
`docs/dsp-audio-pipeline.md`, Antenna-Loss-Formel).

## Wichtigste Fallstricke (nicht vergessen)

1. `~` ist das gesamte Sync/Async-Unterscheidungsmerkmal — kein Header, kein Längenfeld.
2. `POS` **immer** `"OK"` antworten, sonst Hint-Popup beim Spieler.
3. `IS_SPEAKING_BULK` hat einen abschließenden Tab nach dem letzten Paar.
4. `MISSIONEND` antwortet mit Leerstring, nicht `"OK"`.
5. `"Not connected to TeamSpeak"` als Fehlertext beibehalten, falls SQF-Code (heute oder in
   Community-Addons) darauf matcht — im Zweifel exakten Originaltext verwenden.
