# `task_force_radio_pipe` — Arma 3 extension (TFRS)

Newly written extension DLL for Task Force Radio Standalone. It replaces the original shared-memory
bridge to the TeamSpeak 3 plugin with a named pipe to the `Tfrs.VoiceClient` process.

* **Upward (SQF):** byte-compatible with [`docs/protocol-extension-legacy.md`](../../../docs/protocol-extension-legacy.md).
  `addon/addons/` is unchanged and was not touched.
* **Downward (voice client):** [`docs/protocol-ipc-bridge.md`](../../../docs/protocol-ipc-bridge.md),
  named pipe `\\.\pipe\TFRS_VoiceBridge`, line-delimited JSON.
* **Division of labor:** [`docs/dsp-audio-pipeline.md`](../../../docs/dsp-audio-pipeline.md) section 6 —
  this DLL supplies `gain`/`az`/`fx`/`err`, it never touches PCM samples.

---

## Building

```bash
# 64-bit (task_force_radio_pipe_x64.dll)
cmake -S . -B build/x64   -A x64
cmake --build build/x64   --config Release

# 32-bit (task_force_radio_pipe.dll)
cmake -S . -B build/win32 -A Win32
cmake --build build/win32 --config Release
```

Output: `build/<arch>/Release/task_force_radio_pipe[_x64].dll`. Both DLLs belong in
`@task_force_radio/` (Arma automatically loads the matching architecture).

Requirements: CMake ≥ 3.15, MSVC (Visual Studio 2019/2022). No external dependencies, no
`FetchContent`, no vendored code — the JSON parser/writer is hand-written (`src/Json.*`) and
scoped only to the flat bridge schema.

The CRT is linked **statically** (`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`) so players without
the VC++ redistributable can still load the DLL.

---

## Files

| File | Contents |
|---|---|
| `src/Extension.cpp` | `RVExtension` export, `DllMain`, snapshot thread (~15Hz) |
| `src/CommandProcessor.*` | parser/dispatcher for all `callExtension` commands |
| `src/State.*` | client registry, local state, antennas, speakers, **audibility solver** |
| `src/PipeClient.*` | named-pipe client with reconnect, line framing, send coalescing |
| `src/Json.*` | minimal JSON parser/writer (no external library) |
| `src/Util.*` | vector/angle math, Arma parsing, `volumeAttenuation` |

---

## What's implemented

**Legacy protocol (complete):**

* Sync/async determined solely by the trailing `~`.
* `POS` (14 tokens) → registry incl. velocity extrapolation, always replies `OK`.
* `IS_SPEAKING` (2 characters) and `IS_SPEAKING_BULK` (pair + tab per name, **including the
  trailing tab**).
* `TS_INFO` with `SERVER`, `SERVERUID`, `CHANNEL`, `CHANNELID` (constant `"0"`), `PING`→`PONG`,
  `VERSION`; unknown sub-command → `FAIL`.
* `FREQ` (11 tokens, `alive` compared exactly against `"true"`), frequency parsing incl.
  `volume|stereoMode|classname`.
* `TANGENT` / `TANGENT_LR` / `TANGENT_DD`, `RELEASE_ALL_TANGENTS`, `KILLED` → `local` message
  (`transmitOverride` `true`/`null`/`false`).
* `SPEAKERS` (0x0B/0x0A/`|` delimiters), an empty payload clears the list.
* `SETCFG` (20 valid keys, invalid ones are **silently** ignored — no `MessageBox` like in the
  original, which would block Arma's main thread).
* `DFRAME` → `OK`, or `NEEDCFG` on first start and after every pipe reconnect (self-healing:
  re-requested every 3s until the first `SETCFG` arrives).
* `MISSIONEND` → empty string + full state reset.
* `RadioTwrAdd` / `RadioTwrDel` (50-item batches are just handled as they come, antenna list with
  update-in-place for a known net ID).
* `TRACK` and `collectDebugInfo` → no-op returning `OK`.
* Pipe not connected → `"Not connected to TeamSpeak"` (wording deliberately unchanged,
  `fnc_sendPlayerInfo.sqf` matches against it exactly). Unknown sync command → `"UNKNOWN COMMAND"`.

**Bridge:**

* Full `units` snapshot roughly every 66ms, with coalescing (a stalled client can't build up a
  backlog).
* `local` on change, automatically resent after a reconnect.
* Receives `status` (feeds `TS_INFO PING`) and `roster` (name→UID mapping).
* Reconnects every 500ms while the voice client isn't running yet or is restarting.

**Geometry/audio control values (from `dsp-audio-pipeline.md`):**

* `volumeAttenuation` exactly per section 1.
* Effective distance `raw + ti*coef + ti*coef*(raw/2000)`, then `* receivingDistanceMultiplicator`.
* Direct speech: `dist = raw + 2*objectInterception`, range gate `<= speakVolume + 15`, vehicle
  isolation (`clamp(iso_me + iso_him, 0, 0.99)`, `* pow(1-loss, 1.2)`), `CANT_SPEAK_GAIN = 14`
  when "can't speak".
* Radio: `gain = volumeLevel * 0.35` with `volumeLevel = ((vol+1)/10)^4`, `* 0.1` when
  `headsetLowered`; phone: `volumeLevel * 10`.
* Speaker: `speakerRange = vol < 20 ? (vol/10)*TF_speakerDistance : vol*1.9`, plus vehicle
  isolation.
* Intercom: `intercomVolume`, `* 0.1` when `headsetLowered`, ducking on an incoming LR
  transmission (`volume > 2`).
* `err = min(antennaLoss, effectiveDistance / senderRange)`; diver (`dd`) uses
  `underwaterRange = 70 + 230*(1-waves)` and the unstepped formula from section 5.
* Antenna loss = a port of `AntennaManager::findConnection` (`lossTo(to) + lossFrom(from, range)`,
  only connections with total loss < 1 count, sentinel `7.0` = no antenna).
* `az` = azimuth relative to facing direction, `0` = front, positive = clockwise, `-π..π`.

---

## Important for the voice client (please read)

### 1. The `az` convention vs. `Pan()` in `dsp-audio-pipeline.md`

`protocol-ipc-bridge.md` defines `az` as **0 = front, positive = right**. But the `Pan()` formula
in `dsp-audio-pipeline.md` section 2 uses `cos(dir)` — in the original, `dir` was the angle
**from the right axis** (`atan2(y,x) + viewAngle`), not from the facing direction. With `cos` and
the `az` convention, "front" would end up hard right.

The extension delivers `az` exactly as defined in the **bridge protocol** (that's the
authoritative source for this field). The client must therefore use `sin` instead of `cos`:

```csharp
float s = MathF.Sin(az);
float gainLeft  = -0.37525f * s + 0.625f;
float gainRight =  0.37525f * s + 0.625f;
```

(Equivalent: `cos(az - π/2)`.)

*(Already fixed client-side in `Audio/Dsp/Panning.cs` and `docs/dsp-audio-pipeline.md` §2.)*

### 2. Optional back-channel messages the extension can consume

The documented bridge only knows `status` and `roster`. That means the extension **cannot** know
*who is currently transmitting on which frequency* — in the original, that information arrived via
TeamSpeak plugin-to-plugin commands between clients; that channel no longer exists. The extension
therefore additionally accepts (all optional, degrades cleanly when absent):

```json
{"t":"tx","u":[{"uid":"765...","name":"Foo","freq":"51.5X","range":1800,"sub":"digital"}]}
```

* `name` = Arma nickname (preferred) or `uid` (resolved via the most recently received `roster`).
* `sub` ∈ `digital`, `digital_lr`, `airborne`, `dd`, `phone` — same as `subtype` in the `TANGENT`
  command.
* Full snapshot, like `units`: whoever is missing is no longer transmitting. Each entry also
  expires after 1.5s.
* The transmitting client already knows these values itself (it gets them via `TANGENT` from its
  own extension) and only needs to relay them through the voice server.

```json
{"t":"roster","clients":[{"uid":"765...","name":"Foo","talking":true}]}
{"t":"talking","names":["Foo","Bar"]}
```

`talking` (a field on the `roster` entry, or its own message) is needed for `IS_SPEAKING` — that
drives the in-game lip animation and `TFAR_isSpeaking`. Without this info the extension always
reports "not speaking" for remote players.

`status` may additionally include `serverName`, `serverUid`, and `channelName`; those are then
passed through for `TS_INFO SERVER/SERVERUID/CHANNEL` (placeholders otherwise).

**This is a real gap, not just a nice-to-have:** without some client-to-client relay of `tx` (see
the network-protocol extension proposed in `docs/protocol-network.md`'s changelog / the voice
client's implementation status), radio audio only ever works through the fallback heuristic below,
never with correct per-frequency routing.

### 3. Fallback heuristic until `tx` is implemented

As long as a `tx` message has **never** arrived, every known remote player outside direct-speech
range gets an `fx:"sw"` row using the volume of the first subscribed SW frequency and
`err = dist / 3000`. This makes radio audible at all for a first bring-up. As soon as the first
`tx` message arrives, the heuristic permanently disables itself and routing switches to the
correct frequency/range/subtype logic.

---

## Deliberate simplifications / open items

| Topic | Status |
|---|---|
| **One row per UID** | The `units` schema is UID-keyed, so exactly **one** source is reported per speaker: whichever has the highest `gain` (direct / radio / speaker / intercom). The original mixed multiple paths additively. Only audible if you hear someone simultaneously via direct speech *and* radio. |
| **Remote speak volume** | Only *our own* `TF_speak_volume_meters` arrives via `FREQ`; it's assumed to be every player's speak volume (in practice usually a server-wide setting). The original got this value per-sender via TS broadcasts. |
| **`IS_SPEAKING` bit 0** | Correct for remote players only if the client supplies `talking` (see above). Always correct for the local player (tangent + `status.transmitting`). |
| **`IS_SPEAKING` bit 1** | Local player: `true` if at least one non-`direct` source was audible in the last snapshot. Remote: `true` if we're currently hearing *that* player over radio. |
| **"Can't speak" / underwater** | No dedicated `fx` type for this. The `CANT_SPEAK_GAIN = 14` factor is folded into `gain`; the 4th-order 100Hz lowpass from `dsp-audio-pipeline.md` section 4 is therefore missing. Add a dedicated `fx` type if this is audibly noticeable. |
| **Object occlusion** | `objectInterception` is folded into `gain` as a 2m distance penalty per object (as in the original). The additional occlusion lowpass (`2000 - objCount*400`Hz) is missing since there's no field for it. |
| **Vehicle isolation** | Implemented as a gain factor (`pow(1-loss, 1.2)`). The corresponding lowpass (`20000*(1-loss)/4`Hz) is missing for the same reason. |
| **Serious mode / auto-mute** | Not ported. The original muted dead/alive players via the TeamSpeak API; here that would be `muted` in the `units` schema. Currently always `muted:false`. `spectatorNotHearEnemies`, `spectatorCanHearFriendlies`, and `muteSpectators` **are** implemented. |
| **`stereoMode`** | Parsed and stored from `FREQ` but not (yet) transmitted — there's no field for it in the `units` schema. Add `"pan":0/1/2` if needed. |
| **Half-duplex** | Implemented via `full_duplex` + the `TANGENT` radio classname: the frequency of the radio currently being used to transmit is not received. |
| **Trailing `~`** | Stripped before parsing. The original passed it through, which meant `isTrue()` on the *last* `POS` field (`isEnemyToPlayer`) never evaluated `true` — fixed here. |
| **`RVExtensionVersion`** | The original (`old/extensions/task_force_radio_pipe/task_force_radio_pipe.cpp`) exports **only** `RVExtension`, no `RVExtensionVersion`/`RVExtensionArgs`. Carried over 1:1. Anyone who wants the version visible in the RPT log can add the export additively — nothing on the SQF side depends on it. |

---

## Untested — what to watch for on the first CI build

**No C++ compiler was available in this working environment**; the code was neither compiled nor
run. In rough order of how likely something is to break:

1. **32-bit export name.** The export is declared exactly as in the original
   (`extern "C" __declspec(dllexport) void __stdcall RVExtension(...)`, no `.def` file). On x86,
   MSVC decorates `__stdcall` to `_RVExtension@12`. This worked in production in the original, so
   it should be fine. If the 32-bit DLL still shows up as "not found"/"invalid" in the RPT log,
   either add a `task_force_radio_pipe.def` with `EXPORTS / RVExtension`, **or** add to
   `Extension.cpp`:
   ```cpp
   #if defined(_MSC_VER) && !defined(_WIN64)
   #pragma comment(linker, "/EXPORT:RVExtension=_RVExtension@12")
   #endif
   ```
   Verify directly with `dumpbin /exports task_force_radio_pipe.dll`.
2. **`CMAKE_MSVC_RUNTIME_LIBRARY`** needs CMP0091=NEW. `cmake_minimum_required(3.15)` sets that
   automatically — but if this file gets included by a wrapper with an older policy,
   `-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` needs to be added to the command line.
3. **`/W4` warnings.** `/WX` is deliberately **not** set, so a CI build won't fail on warnings.
   Expected candidates: `size_t`↔`DWORD` conversions in `PipeClient.cpp`, `int`↔`float` in
   `State.cpp`.
4. **`strncpy_s` / `_TRUNCATE`** (`Extension.cpp`) are MSVC-specific — a MinGW/clang build would
   need them replaced with `snprintf`. The build is deliberately MSVC-only.
5. **Byte literals.** The delimiters `0x0A`, `0x0B`, `0x10` are written as `'\x0A'`/`'\x0B'`/
   `'\x10'` in the code (`State.cpp`), not as literal characters in the source file — code-page
   independent. `/utf-8` is set regardless.
6. **Threading sanity at runtime.** `DllMain` starts **no** threads (loader lock); the first
   `callExtension` call does that via `std::call_once`. On unload, only a flag is set — no
   joining. All global objects are deliberately leaked heap objects so no static destructors run
   under the loader lock.

### Quick test without Arma

A minimal host is enough to exercise the parser and pipe (the DLL only needs `RVExtension`):

```cpp
// LoadLibraryW(L"task_force_radio_pipe_x64.dll"); GetProcAddress(h, "RVExtension");
char out[10240];
fn(out, sizeof(out), "TS_INFO\tPING");            // expects "PONG" (voice client must be running)
fn(out, sizeof(out), "POS\tFoo\t[0,0,0]\t[0,1,0]\ttrue\ttrue\ttrue\t0\tno\t0\t1\t0\tfalse\tfalse~");
                                                   // expects exactly "OK"
fn(out, sizeof(out), "IS_SPEAKING_BULK\tFoo\tBar"); // expects "00\t00\t"
fn(out, sizeof(out), "DFRAME~");                   // expects "NEEDCFG" the first time
fn(out, sizeof(out), "MISSIONEND~");               // expects ""
```

Without a running `Tfrs.VoiceClient`, **every** call returns `"Not connected to TeamSpeak"` — that
is the correct behavior, identical to the original.
