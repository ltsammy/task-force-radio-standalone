# Task Force Radio Standalone

A TeamSpeak-3-independent Task Force Radio (TFAR) for Arma 3 — one Steam Workshop addon, no
separate app to install, while staying fully compatible with addons/missions that build on top of
the original TFAR.

## Components

| Folder | What |
|---|---|
| [`addon/`](addon/) | Arma 3 mod, published as a single Steam Workshop item. `addons/` is forked 1:1 from the original TFAR (class names, function names, CfgPatches unchanged). `extensions/task_force_radio_pipe/` (the native extension DLL) is newly written: mic capture, Opus, UDP networking, playback mixing, and the radio DSP effect chain all run inside the DLL, in the same process as the audibility solver. |
| [`voice-server/`](voice-server/) | Pure UDP relay server (C#/.NET, Native AOT), deployable as a lean Docker image (~20MB), built for 200-300 concurrent connections with minimal server load. Every mission needs one running instance that all players' clients connect to — see [Hosting a server](#hosting-a-server) below. |

## Why this works without rewriting the whole mod

TFAR cleanly separates the radio logic (SQF, ~90% of the code — frequencies, ranges, encryption,
antennas) from the pure audio transport (previously: TeamSpeak 3). Both sides only talk to each
other through a simple text protocol (`callExtension`). That boundary stays exactly intact — only
what happens *behind* the extension DLL is entirely new:

```
Arma/SQF  ──(unchanged callExtension protocol)──▶  extension DLL (native, in-process)
                                                              │
                                                     (UDP, same protocol
                                                      voice-server already spoke)
                                                              ▼
                                                        voice-server
```

Details: [`docs/protocol-extension-legacy.md`](docs/protocol-extension-legacy.md) (the SQF ↔
extension compatibility boundary), [`docs/protocol-network.md`](docs/protocol-network.md)
(extension ↔ voice-server, the wire protocol), [`docs/dsp-audio-pipeline.md`](docs/dsp-audio-pipeline.md)
(3D audio/radio effects — filter formulas, panning, gain staging).

## Hosting a server

Every mission needs: the addon (client-side, via Steam Workshop) and one running `voice-server`
instance (server-side) that all players' addon settings point at. See
[`docs/server-hosting.md`](docs/server-hosting.md) for the full guide — Workshop install, opening
the right ports, deploying `voice-server` (Docker/Coolify), and the in-game settings players need
to configure.

## Status

Native voice path (`addon/extensions/task_force_radio_pipe/src/Voice/`): implemented and
live-verified end-to-end with real players (connection, distance-based audibility, panning, all
radio effect types, PTT/mic-mute/speaker-mute, radio start/stop beep cues). Voice server: done,
in production use. Addon build: [HEMTT](https://hemtt.dev/) (PBO signing, Steam Workshop publish).
See [`addon/README.md`](addon/README.md) for build/signing/Workshop notes and
[`addon/extensions/task_force_radio_pipe/README.md`](addon/extensions/task_force_radio_pipe/README.md)
for the extension's own structure and build instructions.

## License

`addon/` remains under the original Arma Public License Share Alike (see `addon/LICENSE.md`) —
carried over unchanged from the original. `voice-server/` is new, standalone code under the MIT
license (see its `LICENSE` file).
