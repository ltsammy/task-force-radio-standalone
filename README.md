# Task Force Radio Standalone

A TeamSpeak-3-independent Task Force Radio (TFAR) for Arma 3: its own voice client, its own voice
server, and an Arma 3 addon wired up to the new system — while staying fully compatible with
addons/missions that build on top of the original TFAR.

## Components

| Folder | What |
|---|---|
| [`addon/`](addon/) | Arma 3 mod. `addons/` is forked 1:1 from the original TFAR (class names, function names, CfgPatches unchanged) — only `extensions/task_force_radio_pipe/` (the native extension DLL) is newly written and talks to the new voice client instead of TeamSpeak. |
| [`voice-client/`](voice-client/) | Windows desktop client (C#/WPF): connect via IP/port/password, push-to-talk/voice-activation/always-on, mic/speaker mute with key bindings, device/volume selection, 3D audio + radio distortion modeled on TFAR. |
| [`voice-server/`](voice-server/) | Pure UDP relay server (C#/.NET, Native AOT), deployable as a lean Docker image (~20MB), built for 200-300 concurrent connections with minimal server load. |

## Why this works without rewriting the whole mod

TFAR cleanly separates the radio logic (SQF, ~90% of the code — frequencies, ranges, encryption,
antennas) from the pure audio transport (previously: TeamSpeak 3). Both sides only talk to each
other through a simple text protocol (`callExtension`). That boundary stays exactly intact — only
what happens *behind* the extension DLL is entirely new:

```
Arma/SQF  ──(unchanged callExtension protocol)──▶  extension DLL (new)
                                                              │
                                                  (new, custom pipe protocol)
                                                              ▼
                                                  voice client (new) ──UDP──▶ voice server (new)
```

Details: [`docs/protocol-extension-legacy.md`](docs/protocol-extension-legacy.md) (the
compatibility boundary), [`docs/protocol-ipc-bridge.md`](docs/protocol-ipc-bridge.md) (extension ↔
voice client), [`docs/protocol-network.md`](docs/protocol-network.md) (voice client ↔ voice
server), [`docs/dsp-audio-pipeline.md`](docs/dsp-audio-pipeline.md) (3D audio/radio effects in the
client).

## Status

- Voice server: done, tested end-to-end (connection, Docker build).
- Voice client: core functionality (networking, audio pipeline, hotkeys, bridge, UI) implemented
  and compiling; not yet tested against the real extension.
- Extension DLL (`addon/extensions/task_force_radio_pipe/`): implemented, not yet compiled/tested
  (no local C++ toolchain available — first real build happens in CI).
- Addon build: switched to [HEMTT](https://hemtt.dev/) (matches the `arma3_serverside` project's
  tooling, has built-in PBO signing). `addon/addons/` builds under it after fixing HEMTT's stricter
  syntax requirements (unquoted array values that older tools tolerated) — purely syntactic, no
  behavior change. See [`addon/README.md`](addon/README.md) for build/signing/Steam Workshop notes.
- CI (`.github/workflows/`): voice-server, voice-client, and addon (extension DLLs + HEMTT) all set
  up.

## License

`addon/` remains under the original Arma Public License Share Alike (see `addon/LICENSE.md`) —
carried over unchanged from the original. `voice-client/` and `voice-server/` are new, standalone
code under the MIT license (see the respective `LICENSE` file) — adjust before publishing if
needed.
