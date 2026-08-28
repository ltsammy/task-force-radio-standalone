# Audio-DSP-Pipeline (Portierung aus dem TS3-Plugin)

Das Original berechnet 3D-Audio **komplett selbst** — TeamSpeaks eigene 3D-API wird aktiv
neutralisiert (`systemset3DListenerAttributes`/`channelset3DAttributes` bekommen immer `(0,0,0)`,
`onCustom3dRolloffCalculationClientEvent` setzt Rolloff hart auf `1.0`). Das heißt: der neue
Voice-Client braucht **keine** 3D-Audio-API vom Betriebssystem/einer Engine — er bekommt rohe
Mono-Samples pro Sprecher und wendet exakt diese Kette selbst an, gesteuert durch die vom
Arma-Addon berechneten Werte (Distanz, Funktyp, Fahrzeug-Isolation, Antennen-Loss etc.), die über
das Legacy-Extension-Protokoll hereinkommen.

Alles rechnet in 48 kHz, 16-bit, wird aber intern in `Tfrs.VoiceClient` als `float` `[-1,1]`
verarbeitet (Opus arbeitet ohnehin mit `float`/`short`-PCM bei 48 kHz mono).

## 1. Entfernungsdämpfung — `VolumeAttenuation`

```csharp
static float VolumeAttenuation(float distance, bool shouldPlayerHear, float maxAudible, float multiplier = 1.0f)
{
    if (distance <= 1.0f) return 1.0f;
    float maxDistance = shouldPlayerHear ? maxAudible * multiplier : 5.0f; // CANT_SPEAK_DISTANCE
    float gain = MathF.Pow(10.0f, (distance / (maxDistance * 2f) * -60.0f) / 20.0f);
    return gain < 0.001f ? 0.0f : MathF.Min(1.0f, gain);
}
```

−30 dB bei `distance == maxDistance`, −60 dB bei doppelter Distanz, harter Cutoff bei −60 dB.

**Effektive Distanz für Funk** (inkl. Terrain-Okklusion):
```
effectiveDistance = raw + terrainInterception*coef + terrainInterception*coef*(raw/2000)
effectiveDistance *= receivingDistanceMultiplicator
```
Für Direktsprache stattdessen: `distance = raw + 2 * objectInterception` (2 m Zuschlag je Objekt
in der Sichtlinie).

Diese Werte (`terrainInterception`, `receivingDistanceMultiplicator`, `objectInterception`,
`voiceVolume`) liefert das Arma-Addon bereits fertig berechnet über `POS`/`FREQ` — die Extension
reicht sie 1:1 im `units`-Snapshot des Bridge-Protokolls als `gain` weiter (siehe unten, Abschnitt 6:
Aufgabenteilung Extension/Client).

## 2. Panning (ILD) — vereinfachte Variante statt X3DAudio/HRTF

Das Original nutzt für Direktsprache volles X3DAudio (COM, Windows-only) und für Speaker-Radios
eine simple Cosinus-Formel. **Bewusste Vereinfachung für den neuen Client:** nur die simple Formel,
für alle Quellen — kein Clunk/KEMAR-HRTF-Convolution-Port (unverhältnismäßiger Aufwand für den
Zugewinn gegenüber Stereo-Panning bei den meisten Nutzern, die ohnehin kein Binaural-Setup haben).

```csharp
// dir = Azimut relativ zur Blickrichtung, Radiant, 0 = vorne, +.. im Uhrzeigersinn (siehe az im
// Bridge-Protokoll)
static (float left, float right) Pan(float dirRadians)
{
    float cos = MathF.Cos(dirRadians);
    float gainLeft  = -0.37525f * cos + 0.625f; // -21.5° in Radiant
    float gainRight =  0.37525f * cos + 0.625f; // +21.5° in Radiant
    return (gainLeft, gainRight);
}
```

Erweiterungspunkt: Sollte später echtes HRTF gewünscht sein, lässt sich `Pan` durch eine
Convolution-Engine ersetzen, ohne den Rest der Kette anzufassen (siehe `Audio/Dsp/IPanningModel.cs`).

## 3. Funkverzerrung (`RadioEffect`) — Foldback → Delay → Ringmod → HP → LP

Kette pro Frame (in dieser Reihenfolge):

```csharp
// 1) Foldback-Verzerrung, Threshold aus mittlerem Pegel
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

// 2) x30 Preamp, dann 50ms-Delay (reiner Ringpuffer, 100% wet — d.h. Delay-Line-Wert wird
//    zurückgegeben, nicht mit dem Original gemischt), dann Ringmodulation
for i: buffer[i] = RingModulation(Delay(buffer[i] * 30f), errorLevel);

// Delay: Ringbuffer, DELAY_SAMPLES = SAMPLE_RATE / 20 = 2400 (50ms bei 48kHz)
float Delay(float input) { line[pos] = input; pos = (pos + 1) % 2400; return line[pos]; }

// Ringmod: 90Hz-Sägezahn-Phase treibt eine sin-Rampe
float RingModulation(float input, float mix)
{
    float modulated = input * MathF.Sin(phase * MathF.PI / 2f);
    phase += 90.0f / 48000f;
    if (phase > 1.0f) phase = 0f;
    return input * (1f - mix) + modulated * mix;
}

// 3) Highpass, dann Lowpass (RBJ-Biquads, siehe Abschnitt 4)
```

`errorLevel` = `min(antennaLoss, effectiveDistance / senderRange)`, per Funkverbindung. Kennlinie
(bewusst NICHT linear — Stufen bei 0.0/0.1/0.2/… mit Interpolationsfaktor `(errorLevel - stufe/10)`,
der **nicht** auf `[0,1)` sondern `[0,0.1)` normalisiert ist — das ist ein Kuriosum im Original, muss
aber exakt so übernommen werden, sonst klingt es hörbar anders):

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

## 4. Filter-Parameter je Funktyp

RBJ-Biquad-Formeln (exakt, keine externe Lib nötig):

```csharp
// LowPass
double w0 = 2 * Math.PI * cutoffHz / sampleRate;
double cs = Math.Cos(w0), sn = Math.Sin(w0), AL = sn / (2 * q);
double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
double a0 = 1 + AL, a1 = -2 * cs, a2 = 1 - AL;
// HighPass: b0=(1+cs)/2, b1=-(1+cs), b2=(1+cs)/2 — a0/a1/a2 identisch zu LowPass
// Danach alle b/a durch a0 teilen (Standard-Biquad-Normalisierung).
```

| Funktyp | Highpass | Lowpass |
|---|---|---|
| SW-Funk (`digital`, "Personal") | 900 Hz, Q 0.85 | 3000 Hz, Q 2.0 |
| LR-Funk + Intercom (`digital_lr`) | 520 Hz, Q 0.97 | 1300 Hz, Q 1.0 |
| Airborne (`airborne`) | 1000 Hz, Q 1.0 | 4000 Hz, Q 1.0 |

Zusätzliche Butterworth-Filter (Standard-Cascaded-Biquad-Design über bilineare Transformation,
keine RBJ-Formel — Ordnung wie angegeben):

| Zweck | Typ | Parameter |
|---|---|---|
| Diver-Funk (`dd`) | Bandpass Ordnung 2 | Mitte 1000 Hz, Breite 400 Hz |
| Telefon (`phone`) | Bandpass Ordnung 2 | Mitte 1850 Hz, Breite 1550 Hz |
| Speaker (Funk am Boden) | Bandpass Ordnung 1 | Mitte 2000 Hz, Breite 1000 Hz |
| "Kann nicht sprechen" / untergetaucht | Lowpass Ordnung 4 | 100 Hz |
| Fahrzeug-Isolation | Lowpass Ordnung 2 | `20000 * (1 - loss) / 4` Hz |
| Objekt-Okklusion | Lowpass Ordnung 2 | `2000 - objCount*400` Hz (objCount ≤ 5) |

## 5. Diver-Funk-Sonderfall (`dd`)

Kein Foldback/Ringmod — stattdessen zufälliges Nullsetzen einzelner Samples:
```csharp
if (random.NextDouble() < errorLevel) buffer[i] = 0f;
// danach Bandpass (1000Hz/400Hz, s.o.), dann *30
```
`errorLevel` hier **ungestuft** (kein `CalcErrorLevel`), direkt:
```
underwaterRange = 70 + 230 * (1 - wavesLevel)   // wavesLevel 0..1 vom Addon
errorLevel = min(
    (underwaterDist * (range/underwaterRange) + (normalDist - underwaterDist)) / range,
    antennaLoss)
```

## 6. Aufgabenteilung Extension ↔ Client

Damit der Client selbst **keine** Arma-spezifische Geometrie/Physik kennen muss (Terrain, Fahrzeuge,
Antennen — das bleibt Domäne des Addons/der Extension), gilt folgende Arbeitsteilung:

- **Extension (C++, im Arma-Prozess):** kennt alle SQF-Rohdaten (Distanzen, Terrain-Interception,
  Fahrzeug-Isolation, Antennen-Loss, Funktyp/Subtype, `errorLevel`-Grundwert `distance/range`).
  Berechnet daraus **pro hörbarer Quelle** einen fertigen `gain` (0..1, bereits inkl.
  Entfernungsdämpfung UND `errorLevel` für die Funkverzerrung) und einen `az` (Azimut) und schickt
  das im `units`-Snapshot des Bridge-Protokolls. Zusätzlich einen `effect`-Typ pro Quelle (SW/LR/
  Airborne/DD/Phone/Speaker/DirectSpeech/Intercom), damit der Client die richtige Filterkette wählt.
  *(Ergänzung zum bisherigen `units`-Schema in `protocol-ipc-bridge.md`: dort um ein optionales Feld
  `"fx"` erweitern, sobald die Extension gebaut wird.)*
- **Client (C#):** wendet nur noch die generische, Arma-unabhängige Signalverarbeitung an
  (Foldback/Delay/Ringmod, RBJ/Butterworth-Filter, Panning, Mixing, Kompressor) — exakt das, was
  in diesem Dokument steht. Kein Geometrie-/Physik-Code im Client.

Diese Trennung ist bewusst identisch zur Originalarchitektur (SQF/Extension kennt die Spielwelt,
"TS3-Seite" kennt nur Audio) und hält den Client testbar ohne Arma-Abhängigkeit.

## 7. Gain-Staging & Konstanten

- Mono-Downmix vor Effekt: `mono = sum(channels)/channelCount`, normiert `/32766`.
- Gain je Funktyp: `volumeLevel * 0.35`, `volumeLevel = ((radioVolume0to10 + 1) / 10) ^ 4`.
  Bei gesenktem Headset zusätzlich `* 0.1`.
- Mono-Panning-Modi (`leftOnly`/`rightOnly`, aus `stereoMode` im `FREQ`-Frequenzeintrag): Gain `* 1.5`.
- `CANT_SPEAK_GAIN = 14`, `SPEAKER_GAIN = 4`, `RADIO_GAIN_LR = 5`, `RADIO_GAIN_DD = 15`.
- Nach der Kette: Clamp `[-1,1]`, zurück auf 16-bit-Skala.
- Additive Mischung aller gleichzeitig aktiven Quellen (mit Clamp), abschließend `* globalVolume`.
- Kompressor auf dem Endsignal: `SimpleComp`-Style, 48 kHz, Threshold 80, Release 300 ms, Attack 1 ms,
  Ratio 0.1 — Standard-Feed-Forward-Kompressor, exakte Portierung optional (Client kann vorerst mit
  einem einfachen Soft-Limiter starten, um Clipping bei vielen gleichzeitigen Quellen zu vermeiden;
  echter Kompressor ist ein Nice-to-have, kein Korrektheits-Blocker).

## 8. Bewusste Abweichungen vom Original

- Kein KEMAR-HRTF-Convolution-ITD (Clunk) — durch einfaches Cosinus-Panning ersetzt (Abschnitt 2).
- Kein X3DAudio COM — Distanzdämpfung/Winkel werden mit den hier dokumentierten Formeln direkt
  berechnet statt über die X3DAudio-Distanzkurve/Cone-Emitter-Simulation.
- Antennen-Loss/Fahrzeug-Isolation/Objekt-Okklusion: Formeln sind dokumentiert (siehe
  Recherche-Rohdaten in dieser Datei-Historie), werden aber in der Extension berechnet und fließen
  nur noch als fertiger `gain`-Faktor beim Client an — nicht Teil der Client-DSP-Pipeline.
