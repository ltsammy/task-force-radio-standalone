# Task Force Radio Standalone

Ein TeamSpeak-3-unabhängiges Task Force Radio (TFAR) für Arma 3: eigener Voice-Client, eigener
Voice-Server, und ein an das neue System angebundenes Arma-3-Addon — bei vollständiger
Kompatibilität zu Addons/Missionen, die auf dem originalen TFAR aufbauen.

## Komponenten

| Ordner | Was |
|---|---|
| [`addon/`](addon/) | Arma-3-Mod. `addons/` ist 1:1 aus dem Original-TFAR geforkt (Klassennamen, Funktionsnamen, CfgPatches unverändert), nur `extensions/task_force_radio_pipe/` (die native Extension-DLL) ist neu geschrieben und spricht statt mit TeamSpeak mit dem neuen Voice-Client. |
| [`voice-client/`](voice-client/) | Windows-Desktop-Client (C#/WPF): IP/Port/Passwort verbinden, Push-to-Talk/Sprachaktivierung/Dauersenden, Mikro-/Lautsprecher-Mute mit Tastenbindung, Geräte-/Lautstärkeauswahl, 3D-Audio + Funkverzerrung nach TFAR-Vorbild. |
| [`voice-server/`](voice-server/) | Reiner UDP-Relay-Server (C#/.NET, Native AOT), als schlankes Docker-Image (~20 MB) deploybar, ausgelegt auf 200-300 gleichzeitige Verbindungen bei minimaler Serverlast. |

## Warum das funktioniert, ohne den ganzen Mod neu zu schreiben

TFAR trennt sauber zwischen der Funklogik (SQF, ca. 90 % des Codes — Frequenzen, Reichweiten,
Verschlüsselung, Antennen) und dem reinen Audio-Transport (bisher: TeamSpeak 3). Beide Seiten
reden nur über ein simples Text-Protokoll (`callExtension`) miteinander. Diese Grenze bleibt exakt
erhalten — nur was *hinter* der Extension-DLL passiert, ist komplett neu:

```
Arma/SQF  ──(unverändertes callExtension-Protokoll)──▶  Extension-DLL (neu)
                                                              │
                                                    (neues, eigenes Pipe-Protokoll)
                                                              ▼
                                                    Voice-Client (neu) ──UDP──▶ Voice-Server (neu)
```

Details: [`docs/protocol-extension-legacy.md`](docs/protocol-extension-legacy.md) (die
Kompatibilitätsgrenze), [`docs/protocol-ipc-bridge.md`](docs/protocol-ipc-bridge.md) (Extension ↔
Voice-Client), [`docs/protocol-network.md`](docs/protocol-network.md) (Voice-Client ↔ Voice-Server),
[`docs/dsp-audio-pipeline.md`](docs/dsp-audio-pipeline.md) (3D-Audio/Funkeffekte im Client).

## Status

- Voice-Server: fertig, end-to-end getestet (Verbindung, Docker-Build).
- Voice-Client: Kernfunktionen (Netzwerk, Audio-Pipeline, Hotkeys, Bridge, UI) implementiert und
  kompiliert; noch nicht gegen die echte Extension getestet.
- Extension-DLL (`addon/extensions/task_force_radio_pipe/`): in Arbeit.
- CI (`.github/workflows/`): Server + Client eingerichtet; Addon-Build folgt, sobald die Extension
  steht.

## Lizenz

`addon/` steht weiterhin unter der originalen Arma Public License Share Alike (siehe
`addon/LICENSE.md`) — unverändert aus dem Original übernommen. `voice-client/` und `voice-server/`
sind neuer, eigenständiger Code unter MIT-Lizenz (siehe jeweilige `LICENSE`-Datei) — bei Bedarf vor
Veröffentlichung anpassen.
