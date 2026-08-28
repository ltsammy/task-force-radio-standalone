# Avoiding antivirus/SmartScreen false positives

Decision (see project notes): no code-signing certificate for now (costs money, needs a
registered organization) — instead, best practices that structurally reduce the risk of a false
positive, plus a prepared hook to add a certificate later.

## What we specifically avoid

- **No packing/obfuscation.** No ConfuserEx, no Themida, no custom packer — exactly the pattern
  that heuristic AV engines flag most aggressively ("packed, self-extracting code" is a core
  signal for practically every heuristic engine).
- **No single-file bundle** for the client build (`PublishSingleFile=false` in
  `.github/workflows/voice-client.yml`). A self-contained but "ordinary" folder with many `.dll`
  files reads as a normal .NET app to AV engines; a single self-extracting exe (even though
  technically harmless) structurally resembles a dropper.
- **No low-level keyboard hooks** (`SetWindowsHookEx(WH_KEYBOARD_LL, …)`). Push-to-talk instead
  uses targeted polling of a single configured key via `GetAsyncKeyState`
  (`Hotkeys/PttKeyPoller.cs`) — works just as reliably without focus, but doesn't read every
  keystroke in the system. Mute toggles use the standard Win32 API `RegisterHotKey`
  (`Hotkeys/GlobalHotkeyManager.cs`) — the same API practically every app with global shortcuts
  uses.
- **No process injection, no DLL side-loading, no reading/writing into foreign processes.** The
  only cross-process IPC is a named pipe to our own Arma extension
  (`docs/protocol-ipc-bridge.md`) — standard Windows IPC, no memory patching.
- **Well-known, open-source dependencies** (NAudio, Concentus) instead of custom native audio
  interop DLLs. Fewer unknown binaries in the shipped folder that would need to build up their own
  reputation from scratch.

## What we actively do about it

- **Public, auditable build.** The entire build runs through GitHub Actions from this (public)
  repo — anyone can check the source against the shipped binary. This is also the basis for later
  false-positive reports to AV vendors (Microsoft Defender, among others, via
  https://www.microsoft.com/en-us/wdsi/filesubmission) — a traceable build path ("binary X from
  commit Y in repo Z") speeds up that process.
- **Stable file/assembly name** across releases (`TfrsVoiceClient.exe`) — frequent name/hash
  changes between versions make it harder for reputation systems (SmartScreen relies partly on
  download frequency per file hash) to build trust.
- **Reserved slot for code signing.** `.github/workflows/voice-client.yml` deliberately has no
  signing step yet, but is structured so a `signtool sign …` step can be added right after
  `dotnet publish` once a certificate exists (secrets: certificate/PFX file + password as GitHub
  secrets). Signed builds that accumulate downloads over time are by far the most effective lever
  against SmartScreen warnings — the measures listed here only reduce the baseline risk, they
  don't fully replace a certificate.

## What this does NOT solve

Even with all of the above, a new, unsigned .exe from a previously unknown publisher will very
likely show a SmartScreen "Unknown publisher" warning on the first downloads ("More info" → "Run
anyway" is then required). That typically fades on its own as download counts for the same signed
hash increase — it can't be avoided entirely without a certificate.
