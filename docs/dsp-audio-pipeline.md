# Audio DSP pipeline (ported from the TS3 plugin)

The original computes 3D audio **entirely itself** — TeamSpeak's own 3D API is actively
neutralized (`systemset3DListenerAttributes`/`channelset3DAttributes` always get `(0,0,0)`,
`onCustom3dRolloffCalculationClientEvent` hard-sets rolloff to `1.0`). Meaning: the new voice
client needs **no** 3D audio API from the OS/an engine — it receives raw mono samples per speaker
and applies exactly this chain itself, driven by values computed by the Arma addon (distance,
radio type, vehicle isolation, antenna loss, etc.) that arrive over the legacy extension protocol.

Everything computes at 48kHz, 16-bit, but is processed internally in `Tfrs.VoiceClient` as `float`
`[-1,1]` (Opus works with `float`/`short` PCM at 48kHz mono anyway).

## 1. Distance attenuation — `VolumeAttenuation`

```csharp
static float VolumeAttenuation(float distance, bool shouldPlayerHear, float maxAudible, float multiplier = 1.0f)
{
    if (distance <= 1.0f) return 1.0f;
    float maxDistance = shouldPlayerHear ? maxAudible * multiplier : 5.0f; // CANT_SPEAK_DISTANCE
    float gain = MathF.Pow(10.0f, (distance / (maxDistance * 2f) * -60.0f) / 20.0f);
    return gain < 0.001f ? 0.0f : MathF.Min(1.0f, gain);
}
```

−30dB at `distance == maxDistance`, −60dB at double the distance, hard cutoff at −60dB.

**Effective distance for radio** (incl. terrain occlusion):
```
effectiveDistance = raw + terrainInterception*coef + terrainInterception*coef*(raw/2000)
effectiveDistance *= receivingDistanceMultiplicator
```
For direct speech, instead: `distance = raw + 2 * objectInterception` (2m penalty per object in
the line of sight).

These values (`terrainInterception`, `receivingDistanceMultiplicator`, `objectInterception`,
`voiceVolume`) are delivered by the Arma addon already fully computed via `POS`/`FREQ` — the
extension passes them through 1:1 as `gain` in the bridge protocol's `units` snapshot (see below,
section 6: division of labor between extension and client).

## 2. Panning (ILD) — simplified stand-in for X3DAudio/HRTF

The original uses full X3DAudio (COM, Windows-only) for direct speech, and a simple cosine formula
for speaker radios. **Deliberate simplification for the new client:** only the simple formula, for
all sources — no Clunk/KEMAR HRTF convolution port (disproportionate effort for the gain over
stereo panning for most users, who don't have a binaural setup anyway).

```csharp
// az = azimuth relative to facing direction, radians, 0 = front, positive = clockwise/right
// (matches the "az" field in the bridge protocol). Note: sin(), not cos() — the original angle
// this formula was ported from is measured from the right axis, not from "front"; using cos()
// directly against an az where 0 = front pans dead center audio hard to one side. Caught while
// cross-checking the extension's az convention against this formula — see
// addon/extensions/task_force_radio_pipe/README.md.
static (float left, float right) Pan(float azRadians)
{
    float sin = MathF.Sin(azRadians);
    float gainLeft  = -0.37525f * sin + 0.625f; // -21.5° in radians
    float gainRight =  0.37525f * sin + 0.625f; // +21.5° in radians
    return (gainLeft, gainRight);
}
```

Extension point: if real HRTF is wanted later, `Pan` can be swapped for a convolution engine
without touching the rest of the chain (see `Audio/Dsp/IPanningModel.cs`).

## 3. Radio distortion (`RadioEffect`) — foldback → delay → ringmod → HP → LP

Chain per frame (in this order):

```csharp
// 1) Foldback distortion, threshold derived from the average level
float avg = buffer.Select(MathF.Abs).Average();
float threshold = 0.3f * (1f - errorLevel) * (avg / 0.005f);
for i: buffer[i] = Foldback(buffer[i], threshold);

static float Foldback(float input, float threshold)
{
    if (threshold < 0.00001f) return 0f;
    if (input > threshold || input < -threshold)
        input = MathF.Abs(MathF.Abs(Mod(input - threshold, threshold * 4f)) - threshold * 2f) - threshold;
    return input;
}

// 2) x30 preamp, then a 50ms delay (plain ring buffer, 100% wet — i.e. the delay-line value is
//    returned, not mixed with the original), then ring modulation
for i: buffer[i] = RingModulation(Delay(buffer[i] * 30f), errorLevel);

// Delay: ring buffer, DELAY_SAMPLES = SAMPLE_RATE / 20 = 2400 (50ms at 48kHz)
float Delay(float input) { line[pos] = input; pos = (pos + 1) % 2400; return line[pos]; }

// Ringmod: a 90Hz sawtooth phase drives a sin ramp
float RingModulation(float input, float mix)
{
    float modulated = input * MathF.Sin(phase * MathF.PI / 2f);
    phase += 90.0f / 48000f;
    if (phase > 1.0f) phase = 0f;
    return input * (1f - mix) + modulated * mix;
}

// 3) Highpass, then lowpass (RBJ biquads, see section 4)
```

`errorLevel` = `min(antennaLoss, effectiveDistance / senderRange)`, per radio link. Response curve
(deliberately NOT linear — steps at 0.0/0.1/0.2/… with an interpolation factor
`(errorLevel - step/10)` that is **not** normalized to `[0,1)` but to `[0,0.1)` — this is a quirk
in the original, but must be carried over exactly, otherwise it audibly sounds different):

```csharp
static readonly float[] ErrorLevels = {
    0f, 0.150000006f, 0.300000012f, 0.600000024f, 0.899999976f, 0.950000048f,
    0.960000038f, 0.970000029f, 0.980000019f, 0.995000005f, 0.997799993f,
    0.998799993f, 0.99999f
};
static float CalcErrorLevel(float errorLevel)
{
    int part = Math.Clamp((int)(errorLevel * 10f), 0, ErrorLevels.Length - 2);
    float from = ErrorLevels[part], to = ErrorLevels[part + 1];
    return from + (to - from) * (errorLevel - part / 10f);
}
```

## 4. Filter parameters per radio type

RBJ biquad formulas (exact, no external library needed):

```csharp
// LowPass
double w0 = 2 * Math.PI * cutoffHz / sampleRate;
double cs = Math.Cos(w0), sn = Math.Sin(w0), AL = sn / (2 * q);
double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
double a0 = 1 + AL, a1 = -2 * cs, a2 = 1 - AL;
// HighPass: b0=(1+cs)/2, b1=-(1+cs), b2=(1+cs)/2 — a0/a1/a2 identical to LowPass
// Then divide all b/a by a0 (standard biquad normalization).
```

| Radio type | Highpass | Lowpass |
|---|---|---|
| SW radio (`digital`, "Personal") | 900Hz, Q 0.85 | 3000Hz, Q 2.0 |
| LR radio + intercom (`digital_lr`) | 520Hz, Q 0.97 | 1300Hz, Q 1.0 |
| Airborne (`airborne`) | 1000Hz, Q 1.0 | 4000Hz, Q 1.0 |

Additional Butterworth filters (standard cascaded-biquad design via bilinear transform, not an RBJ
formula — order as noted):

| Purpose | Type | Parameters |
|---|---|---|
| Diver radio (`dd`) | Bandpass order 2 | center 1000Hz, width 400Hz |
| Phone (`phone`) | Bandpass order 2 | center 1850Hz, width 1550Hz |
| Speaker (ground radio) | Bandpass order 1 | center 2000Hz, width 1000Hz |
| "Can't speak" / underwater | Lowpass order 4 | 100Hz |
| Vehicle isolation | Lowpass order 2 | `20000 * (1 - loss) / 4` Hz |
| Object occlusion | Lowpass order 2 | `2000 - objCount*400` Hz (objCount ≤ 5) |

## 5. Diver radio special case (`dd`)

No foldback/ringmod — instead, random zeroing of individual samples:
```csharp
if (random.NextDouble() < errorLevel) buffer[i] = 0f;
// then bandpass (1000Hz/400Hz, above), then *30
```
`errorLevel` here is **unstepped** (no `CalcErrorLevel`), computed directly:
```
underwaterRange = 70 + 230 * (1 - wavesLevel)   // wavesLevel 0..1 from the addon
errorLevel = min(
    (underwaterDist * (range/underwaterRange) + (normalDist - underwaterDist)) / range,
    antennaLoss)
```

## 6. Division of labor: extension ↔ client

So the client itself needs **no** Arma-specific geometry/physics knowledge (terrain, vehicles,
antennas — that stays the addon's/extension's domain), the work is split as follows:

- **Extension (C++, inside the Arma process):** knows all the raw SQF data (distances, terrain
  interception, vehicle isolation, antenna loss, radio type/subtype, the `errorLevel` base value
  `distance/range`). Computes, **per audible source**, a finished `gain` (0..1, already including
  distance attenuation AND the `errorLevel` for radio distortion) and an `az` (azimuth), and sends
  that in the bridge protocol's `units` snapshot. Also an `effect` type per source (SW/LR/
  Airborne/DD/Phone/Speaker/DirectSpeech/Intercom) so the client picks the right filter chain.
  *(This is reflected in `protocol-ipc-bridge.md`'s `units` schema via the `"fx"`/`"err"` fields.)*
- **Client (C#):** applies only the generic, Arma-independent signal processing (foldback/delay/
  ringmod, RBJ/Butterworth filters, panning, mixing, compressor) — exactly what's documented here.
  No geometry/physics code in the client.

This split is deliberately identical to the original architecture (SQF/extension knows the game
world, the "TS3 side" only knows audio) and keeps the client testable without an Arma dependency.

## 7. Gain staging & constants

- Mono downmix before the effect: `mono = sum(channels)/channelCount`, normalized `/32766`.
- Gain per radio type: `volumeLevel * 0.35`, `volumeLevel = ((radioVolume0to10 + 1) / 10) ^ 4`.
  With the headset lowered, additionally `* 0.1`.
- Mono panning modes (`leftOnly`/`rightOnly`, from `stereoMode` in the `FREQ` frequency entry):
  gain `* 1.5`.
- `CANT_SPEAK_GAIN = 14`, `SPEAKER_GAIN = 4`, `RADIO_GAIN_LR = 5`, `RADIO_GAIN_DD = 15`.
- After the chain: clamp `[-1,1]`, back to 16-bit scale.
- Additive mixing of all simultaneously active sources (with clamping), finally `* globalVolume`.
- Compressor on the final signal: `SimpleComp`-style, 48kHz, threshold 80, release 300ms, attack
  1ms, ratio 0.1 — standard feed-forward compressor, exact port is optional (the client can start
  with a simple soft limiter to avoid clipping when many sources are active at once; a real
  compressor is a nice-to-have, not a correctness blocker).

## 8. Deliberate deviations from the original

- No KEMAR HRTF convolution ITD (Clunk) — replaced by simple cosine/sine panning (section 2).
- No X3DAudio COM — distance attenuation/angle are computed directly with the formulas documented
  here instead of via X3DAudio's distance-curve/cone-emitter simulation.
- Antenna loss/vehicle isolation/object occlusion: formulas are documented (see the research this
  file was built from), but are computed in the extension and only reach the client as a finished
  `gain` factor — not part of the client's DSP pipeline.
