# Vermeidung von Antivirus-/SmartScreen-Fehlalarmen

Entscheidung (siehe Projektnotizen): vorerst **kein Code-Signing-Zertifikat** (kostet Geld, braucht
eine registrierte Organisation) — stattdessen Best Practices, die das Risiko eines False-Positives
strukturell reduzieren, plus eine vorbereitete Stelle, um später ein Zertifikat einzuhängen.

## Was wir konkret vermeiden

- **Kein Packing/Obfuskierung.** Kein ConfuserEx, kein Themida, kein Custom-Packer — genau das
  Muster, das heuristische AV-Engines am aggressivsten flaggen ("gepackter, sich selbst
  entpackender Code" ist ein Kernsignal für praktisch jede Heuristik-Engine).
- **Kein Single-File-Bundle** für den Client-Build (`PublishSingleFile=false` in
  `.github/workflows/voice-client.yml`). Ein self-contained, aber "normaler" Ordner mit vielen
  `.dll`-Dateien sieht für AV-Engines wie eine gewöhnliche .NET-App aus; ein einzelnes,
  selbst-entpackendes Exe (auch wenn technisch harmlos) ähnelt strukturell einem Dropper.
- **Keine Low-Level-Keyboard-Hooks** (`SetWindowsHookEx(WH_KEYBOARD_LL, …)`). Push-to-Talk nutzt
  stattdessen gezieltes Polling einer einzelnen konfigurierten Taste über `GetAsyncKeyState`
  (`Hotkeys/PttKeyPoller.cs`) — funktioniert genauso zuverlässig auch ohne Fokus, liest aber nicht
  jeden Tastendruck im System mit. Mute-Toggles nutzen die Standard-Win32-API `RegisterHotKey`
  (`Hotkeys/GlobalHotkeyManager.cs`) — dieselbe API, die praktisch jede App mit globalen Shortcuts
  verwendet.
- **Keine Prozess-Injection, kein DLL-Side-Loading, kein Lesen/Schreiben in fremde Prozesse.** Die
  einzige plattformübergreifende IPC ist eine benannte Pipe zur eigenen Arma-Extension
  (`docs/protocol-ipc-bridge.md`) — Standard-Windows-IPC, kein Memory-Patching.
- **Bekannte, quelloffene Abhängigkeiten** (NAudio, Concentus) statt eigener nativer
  Audio-Interop-DLLs. Weniger unbekannte Binärdateien im Auslieferungsordner, die eine
  Reputationsprüfung neu aufbauen müssten.

## Was wir aktiv dafür tun

- **Öffentlicher, nachvollziehbarer Build.** Der komplette Build läuft über GitHub Actions aus
  diesem (öffentlichen) Repo — jeder kann den Quellcode gegen das ausgelieferte Binary prüfen. Das
  ist auch die Grundlage für spätere False-Positive-Meldungen bei AV-Herstellern (Microsoft
  Defender, u. a. über https://www.microsoft.com/en-us/wdsi/filesubmission), da ein nachvollziehbarer
  Build-Pfad ("Binary X aus Commit Y in Repo Z") das Verfahren beschleunigt.
- **Stabiler Dateiname/Assembly-Name** über Releases hinweg (`TfrsVoiceClient.exe`) — häufige
  Namens-/Hash-Änderungen zwischen Versionen erschweren es Reputationssystemen (SmartScreen
  arbeitet u. a. mit Download-Häufigkeit pro Datei-Hash), Vertrauen aufzubauen.
- **Reserve für Code-Signing.** `.github/workflows/voice-client.yml` hat bewusst noch keinen
  Signing-Schritt, ist aber so aufgebaut, dass ein `signtool sign …`-Step nach dem `dotnet publish`
  einfach ergänzt werden kann, sobald ein Zertifikat vorhanden ist (Secrets: Zertifikatsdatei/PFX +
  Passwort als GitHub-Secret). Signierte + im Zeitverlauf zunehmend heruntergeladene Builds sind der
  mit Abstand wirksamste Hebel gegen SmartScreen-Warnungen — die hier beschriebenen Maßnahmen
  reduzieren nur das Grundrisiko, ersetzen ein Zertifikat aber nicht vollständig.

## Was das NICHT löst

Auch mit allen oben genannten Maßnahmen wird eine neue, unsignierte .exe von einem bislang
unbekannten Publisher bei den ersten Downloads mit hoher Wahrscheinlichkeit eine
SmartScreen-"Unbekannter Herausgeber"-Warnung zeigen ("Weitere Informationen" → "Trotzdem
ausführen" ist dann nötig). Das legt sich typischerweise mit steigender Download-Zahl desselben
signierten Hashes von selbst — ganz vermeiden lässt es sich ohne Zertifikat nicht.
