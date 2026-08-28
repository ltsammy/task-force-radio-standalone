# `task_force_radio_pipe` — Arma-3-Extension (TFRS)

Neu geschriebene Extension-DLL für Task Force Radio Standalone. Sie ersetzt die ursprüngliche
Shared-Memory-Brücke zum TeamSpeak-3-Plugin durch eine Named Pipe zum `Tfrs.VoiceClient`-Prozess.

* **Nach oben (SQF):** byte-kompatibel zu [`docs/protocol-extension-legacy.md`](../../../docs/protocol-extension-legacy.md).
  `addon/addons/` ist unverändert und wurde nicht angefasst.
* **Nach unten (Voice-Client):** [`docs/protocol-ipc-bridge.md`](../../../docs/protocol-ipc-bridge.md),
  Named Pipe `\\.\pipe\TFRS_VoiceBridge`, zeilenweises JSON.
* **Rechenaufteilung:** [`docs/dsp-audio-pipeline.md`](../../../docs/dsp-audio-pipeline.md) Abschnitt 6 —
  diese DLL liefert `gain`/`az`/`fx`/`err`, sie fasst **keine** PCM-Samples an.

---

## Bauen

```bash
# 64 Bit (task_force_radio_pipe_x64.dll)
cmake -S . -B build/x64   -A x64
cmake --build build/x64   --config Release

# 32 Bit (task_force_radio_pipe.dll)
cmake -S . -B build/win32 -A Win32
cmake --build build/win32 --config Release
```

Ergebnis: `build/<arch>/Release/task_force_radio_pipe[_x64].dll`. Beide DLLs gehören nach
`@task_force_radio/` (Arma lädt automatisch die passende Architektur).

Anforderungen: CMake ≥ 3.15, MSVC (Visual Studio 2019/2022). Keine externen Abhängigkeiten,
kein `FetchContent`, kein vendored Code — der JSON-Parser/-Writer ist handgeschrieben
(`src/Json.*`) und nur auf das flache Bridge-Schema zugeschnitten.

Die CRT wird **statisch** gelinkt (`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`), damit Spieler
ohne VC++-Redistributable die DLL laden können.

---

## Dateien

| Datei | Inhalt |
|---|---|
| `src/Extension.cpp` | `RVExtension`-Export, `DllMain`, Snapshot-Thread (~15 Hz) |
| `src/CommandProcessor.*` | Parser/Dispatcher für alle `callExtension`-Kommandos |
| `src/State.*` | Client-Registry, lokaler Zustand, Antennen, Speaker, **Hörbarkeits-Solver** |
| `src/PipeClient.*` | Named-Pipe-Client mit Reconnect, Zeilen-Framing, Sende-Coalescing |
| `src/Json.*` | Minimaler JSON-Parser/-Writer (keine externe Lib) |
| `src/Util.*` | Vektor-/Winkelmathematik, Arma-Parsing, `volumeAttenuation` |

---

## Was ist implementiert

**Legacy-Protokoll (vollständig):**

* Sync/Async ausschließlich über das abschließende `~`.
* `POS` (14 Tokens) → Registry inkl. Velocity-Extrapolation, antwortet immer `OK`.
* `IS_SPEAKING` (2 Zeichen) und `IS_SPEAKING_BULK` (Paar + Tab pro Name, **inkl. abschließendem Tab**).
* `TS_INFO` mit `SERVER`, `SERVERUID`, `CHANNEL`, `CHANNELID` (konstant `"0"`), `PING`→`PONG`, `VERSION`;
  unbekanntes Sub-Kommando → `FAIL`.
* `FREQ` (11 Tokens, `alive` exakt gegen `"true"`), Frequenz-Parsing inkl. `volume|stereoMode|classname`.
* `TANGENT` / `TANGENT_LR` / `TANGENT_DD`, `RELEASE_ALL_TANGENTS`, `KILLED` → `local`-Nachricht
  (`transmitOverride` `true`/`null`/`false`).
* `SPEAKERS` (0x0B/0x0A/`|`-Delimiter), leerer Payload löscht die Liste.
* `SETCFG` (20 gültige Keys, ungültige werden **still** ignoriert — kein `MessageBox` wie im Original,
  das würde Armas Hauptthread blockieren).
* `DFRAME` → `OK`, bzw. `NEEDCFG` beim ersten Start und nach jedem Pipe-Reconnect (self-healing:
  wird alle 3 s erneut angefordert, bis das erste `SETCFG` eintrifft).
* `MISSIONEND` → Leerstring + kompletter State-Reset.
* `RadioTwrAdd` / `RadioTwrDel` (50er-Batches werden einfach mitverarbeitet, Antennen-Liste mit
  Update-in-place bei bekannter NetID).
* `TRACK` und `collectDebugInfo` → No-Op mit `OK`.
* Pipe nicht verbunden → `"Not connected to TeamSpeak"` (Wortlaut bewusst unverändert, `fnc_sendPlayerInfo.sqf`
  matcht exakt darauf). Unbekanntes Sync-Kommando → `"UNKNOWN COMMAND"`.

**Bridge:**

* `units`-Vollsnapshot alle ~66 ms, coalescing (ein stehengebliebener Client kann keinen Backlog erzeugen).
* `local` bei Änderung, wird nach einem Reconnect automatisch neu gesendet.
* Empfang von `status` (füttert `TS_INFO PING`) und `roster` (Name→UID-Mapping).
* Reconnect alle 500 ms, wenn der Voice-Client noch nicht läuft oder neu startet.

**Geometrie/Audio-Steuerwerte (aus `dsp-audio-pipeline.md`):**

* `volumeAttenuation` exakt nach Abschnitt 1.
* Effektive Distanz `raw + ti*coef + ti*coef*(raw/2000)`, dann `* receivingDistanceMultiplicator`.
* Direktsprache: `dist = raw + 2*objectInterception`, Reichweiten-Gate `<= speakVolume + 15`,
  Fahrzeug-Isolation (`clamp(iso_me + iso_him, 0, 0.99)`, `* pow(1-loss, 1.2)`),
  `CANT_SPEAK_GAIN = 14` bei „kann nicht sprechen“.
* Funk: `gain = volumeLevel * 0.35` mit `volumeLevel = ((vol+1)/10)^4`, `* 0.1` bei `headsetLowered`;
  Telefon `volumeLevel * 10`.
* Speaker: `speakerRange = vol < 20 ? (vol/10)*TF_speakerDistance : vol*1.9`, plus Fahrzeug-Isolation.
* Intercom: `intercomVolume`, `* 0.1` bei `headsetLowered`, Ducking bei eingehendem LR (`volume > 2`).
* `err = min(antennaLoss, effectiveDistance / senderRange)`, Diver (`dd`) mit
  `underwaterRange = 70 + 230*(1-waves)` und der ungestuften Formel aus Abschnitt 5.
* Antennen-Loss = Port von `AntennaManager::findConnection` (`lossTo(to) + lossFrom(from, range)`,
  nur Verbindungen mit Gesamt-Loss < 1 zählen, Sentinel `7.0` = keine Antenne).
* `az` = Azimut relativ zur Blickrichtung, `0` = vorne, positiv = im Uhrzeigersinn, `-π..π`.

---

## Wichtig für den Voice-Client (bitte lesen)

### 1. `az`-Konvention vs. `Pan()` in `dsp-audio-pipeline.md`

`protocol-ipc-bridge.md` definiert `az` als **0 = vorne, positiv = rechts**. Die `Pan()`-Formel in
`dsp-audio-pipeline.md` Abschnitt 2 benutzt aber `cos(dir)` — im Original war `dir` der Winkel
**ab der Rechts-Achse** (`atan2(y,x) + viewAngle`), nicht ab der Blickrichtung. Mit `cos` und der
`az`-Konvention läge „vorne“ hart rechts.

Die Extension liefert `az` exakt so wie im **Bridge-Protokoll** definiert (das ist die maßgebliche
Quelle für dieses Feld). Der Client muss deshalb `sin` statt `cos` verwenden:

```csharp
float s = MathF.Sin(az);
float gainLeft  = -0.37525f * s + 0.625f;
float gainRight =  0.37525f * s + 0.625f;
```

(Äquivalent: `cos(az - π/2)`.)

### 2. Optionale Rückkanal-Nachrichten, die die Extension auswerten kann

Die dokumentierte Bridge kennt nur `status` und `roster`. Damit kann die Extension **nicht** wissen,
*wer gerade auf welcher Frequenz sendet* — im Original kam diese Info über TeamSpeak-Plugin-Commands
zwischen den Clients, dieser Kanal existiert jetzt nicht mehr. Die Extension akzeptiert deshalb
zusätzlich (alles optional, wird bei Abwesenheit sauber degradiert):

```json
{"t":"tx","u":[{"uid":"765...","name":"Foo","freq":"51.5X","range":1800,"sub":"digital"}]}
```

* `name` = Arma-Nickname (bevorzugt) oder `uid` (wird über das zuletzt empfangene `roster` aufgelöst).
* `sub` ∈ `digital`, `digital_lr`, `airborne`, `dd`, `phone` — identisch zum `subtype` im
  `TANGENT`-Kommando.
* Vollsnapshot, wie `units`: wer fehlt, sendet nicht mehr. Zusätzlich läuft jeder Eintrag nach 1,5 s ab.
* Der Sender-Client kennt diese Werte bereits (er bekommt sie über `TANGENT` von seiner eigenen
  Extension) und muss sie nur über den Voice-Server weiterreichen.

```json
{"t":"roster","clients":[{"uid":"765...","name":"Foo","talking":true}]}
{"t":"talking","names":["Foo","Bar"]}
```

`talking` (Feld im `roster`-Eintrag oder eigene Nachricht) wird für `IS_SPEAKING` gebraucht — das
steuert im Spiel die Lippenanimation und `TFAR_isSpeaking`. Ohne diese Info meldet die Extension für
Remote-Spieler immer „spricht nicht“.

`status` darf zusätzlich `serverName`, `serverUid` und `channelName` enthalten; die werden dann für
`TS_INFO SERVER/SERVERUID/CHANNEL` durchgereicht (sonst Platzhalter).

### 3. Fallback-Heuristik bis `tx` implementiert ist

Solange **noch nie** eine `tx`-Nachricht eintraf, bekommt jeder bekannte Remote-Spieler außerhalb der
Direktsprech-Reichweite eine `fx:"sw"`-Zeile mit dem Volume der ersten abonnierten SW-Frequenz und
`err = dist / 3000`. Das macht Funk beim ersten Bring-up überhaupt hörbar. Sobald die erste
`tx`-Nachricht ankommt, schaltet sich die Heuristik dauerhaft ab und es wird korrekt nach Frequenz,
Reichweite und Subtype geroutet.

---

## Bewusste Vereinfachungen / offene Punkte

| Thema | Zustand |
|---|---|
| **Eine Zeile pro UID** | Das `units`-Schema ist UID-keyed, also wird pro Sprecher genau **eine** Quelle gemeldet: die mit dem höchsten `gain` (Direkt / Funk / Speaker / Intercom). Das Original mischte mehrere Pfade additiv. Hörbar wird das nur, wenn man jemanden gleichzeitig direkt *und* über Funk hört. |
| **Sprechlautstärke Remote** | Nur die *eigene* `TF_speak_volume_meters` kommt über `FREQ` an; sie wird als Sprechlautstärke aller Spieler angenommen (in der Praxis eine servergleiche Einstellung). Das Original bekam den Wert pro Sender über TS-Broadcasts. |
| **`IS_SPEAKING` Bit 0** | Für Remote-Spieler nur korrekt, wenn der Client `talking` liefert (siehe oben). Für den lokalen Spieler immer korrekt (Tangente + `status.transmitting`). |
| **`IS_SPEAKING` Bit 1** | Lokaler Spieler: `true`, wenn im letzten Snapshot mindestens eine Nicht-`direct`-Quelle hörbar war. Remote: `true`, wenn wir *diesen* Spieler gerade über Funk hören. |
| **„Kann nicht sprechen“ / untergetaucht** | Es gibt keinen eigenen `fx`-Typ dafür. Der Gain-Faktor `CANT_SPEAK_GAIN = 14` ist in `gain` eingerechnet, der 100-Hz-Lowpass 4. Ordnung aus `dsp-audio-pipeline.md` Abschnitt 4 fehlt dadurch. Falls hörbar störend: eigenen `fx`-Typ ergänzen. |
| **Objekt-Okklusion** | `objectInterception` fließt als 2-m-Distanzzuschlag pro Objekt in `gain` ein (wie im Original). Der zusätzliche Okklusions-Lowpass (`2000 - objCount*400 Hz`) fehlt, weil kein Feld dafür existiert. |
| **Fahrzeug-Isolation** | Als Gain-Faktor implementiert (`pow(1-loss, 1.2)`). Der zugehörige Lowpass (`20000*(1-loss)/4 Hz`) fehlt aus demselben Grund. |
| **Serious Mode / Auto-Mute** | Nicht portiert. Das Original mutete Tote/Lebende über die TeamSpeak-API; hier gäbe es dafür `muted` im `units`-Schema. Aktuell immer `muted:false`. `spectatorNotHearEnemies`, `spectatorCanHearFriendlies` und `muteSpectators` **sind** umgesetzt. |
| **`stereoMode`** | Wird aus `FREQ` geparst und gespeichert, aber (noch) nicht übertragen — im `units`-Schema gibt es kein Feld dafür. Bei Bedarf `"pan":0/1/2` ergänzen. |
| **Halbduplex** | Umgesetzt über `full_duplex` + `TANGENT`-Radio-Classname: die Frequenz des Geräts, mit dem gerade gesendet wird, wird nicht empfangen. |
| **Trailing `~`** | Wird vor dem Parsen abgeschnitten. Das Original reichte es durch, wodurch `isTrue()` auf dem *letzten* `POS`-Feld (`isEnemyToPlayer`) nie `true` ergab — hier ist das repariert. |
| **`RVExtensionVersion`** | Das Original (`old/extensions/task_force_radio_pipe/task_force_radio_pipe.cpp`) exportiert **nur** `RVExtension`, kein `RVExtensionVersion`/`RVExtensionArgs`. Das wurde 1:1 übernommen. Wer die Version im RPT sehen will, kann den Export additiv ergänzen — SQF-seitig hängt nichts daran. |

---

## Ungetestet — worauf beim ersten CI-Build zu achten ist

In dieser Arbeitsumgebung war **kein C++-Compiler verfügbar**; der Code wurde nicht kompiliert und
nicht ausgeführt. Reihenfolge der Wahrscheinlichkeit, dass etwas hakt:

1. **32-Bit-Export-Name.** Der Export ist exakt wie im Original deklariert
   (`extern "C" __declspec(dllexport) void __stdcall RVExtension(...)`, ohne `.def`-Datei). Auf x86
   dekoriert MSVC `__stdcall` zu `_RVExtension@12`. Das lief im Original produktiv, sollte also passen.
   Falls die 32-Bit-DLL im RPT dennoch als „not found“/„invalid“ auftaucht, entweder eine
   `task_force_radio_pipe.def` mit `EXPORTS / RVExtension` hinzufügen **oder** in `Extension.cpp`
   ergänzen:
   ```cpp
   #if defined(_MSC_VER) && !defined(_WIN64)
   #pragma comment(linker, "/EXPORT:RVExtension=_RVExtension@12")
   #endif
   ```
   Mit `dumpbin /exports task_force_radio_pipe.dll` lässt sich das direkt prüfen.
2. **`CMAKE_MSVC_RUNTIME_LIBRARY`** braucht CMP0091=NEW. Das setzt `cmake_minimum_required(3.15)`
   automatisch — wird die Datei aber von einem Wrapper mit älterer Policy eingebunden, muss
   `-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` mit auf die Kommandozeile.
3. **`/W4`-Warnungen.** Es ist bewusst **kein** `/WX` gesetzt, ein CI-Build bricht also nicht an
   Warnungen ab. Erwartbare Kandidaten: `size_t`↔`DWORD`-Konvertierungen in `PipeClient.cpp`,
   `int`↔`float` in `State.cpp`.
4. **`strncpy_s` / `_TRUNCATE`** (`Extension.cpp`) sind MSVC-spezifisch — bei einem eventuellen
   MinGW/clang-Build müssten sie durch `snprintf` ersetzt werden. Der Build ist bewusst MSVC-only.
5. **Byte-Literale.** Die Delimiter `0x0A`, `0x0B`, `0x10` stehen als `'\x0A'`/`'\x0B'`/`'\x10'` im
   Code (`State.cpp`), nicht als Zeichen in der Quelldatei — Code-Page-unabhängig. `/utf-8` ist
   trotzdem gesetzt.
6. **Threading-Sanity zur Laufzeit.** `DllMain` startet **keine** Threads (Loader-Lock); der erste
   `callExtension`-Aufruf tut das per `std::call_once`. Beim Entladen wird nur ein Flag gesetzt und
   nicht gejoint. Alle globalen Objekte sind bewusst geleakte Heap-Objekte, damit keine statischen
   Destruktoren unter dem Loader-Lock laufen.

### Schnelltest ohne Arma

Ein minimaler Host reicht, um Parser und Pipe zu prüfen (die DLL braucht nur `RVExtension`):

```cpp
// LoadLibraryW(L"task_force_radio_pipe_x64.dll"); GetProcAddress(h, "RVExtension");
char out[10240];
fn(out, sizeof(out), "TS_INFO\tPING");            // erwartet "PONG" (Voice-Client muss laufen)
fn(out, sizeof(out), "POS\tFoo\t[0,0,0]\t[0,1,0]\ttrue\ttrue\ttrue\t0\tno\t0\t1\t0\tfalse\tfalse~");
                                                   // erwartet exakt "OK"
fn(out, sizeof(out), "IS_SPEAKING_BULK\tFoo\tBar"); // erwartet "00\t00\t"
fn(out, sizeof(out), "DFRAME~");                   // erwartet "NEEDCFG" beim ersten Mal
fn(out, sizeof(out), "MISSIONEND~");               // erwartet ""
```

Ohne laufenden `Tfrs.VoiceClient` liefert **jeder** Aufruf `"Not connected to TeamSpeak"` — das ist
das korrekte Verhalten, identisch zum Original.
