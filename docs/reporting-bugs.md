# Reporting a bug

Voice problems are almost impossible to diagnose from a description alone — "I couldn't hear
anyone" has about a dozen different causes that look identical from the inside. The addon writes a
log that tells them apart. **Attaching it turns a guess into a five-minute fix.**

## Where the log is

```
%APPDATA%\Tfrs\Extension\extension.log
```

Written in full: `C:\Users\<YOUR NAME>\AppData\Roaming\Tfrs\Extension\extension.log`

Fastest way there: press **Win + R**, paste this, press Enter:

```
%APPDATA%\Tfrs\Extension
```

`AppData` is hidden in Explorer by default, which is why pasting the path beats clicking through.

## Please don't just attach the whole file

The log is only ever appended to — it is never rotated or cleared, so after a few weeks it can be
tens of megabytes covering months of sessions. Nobody is going to find your incident in there.

Every line starts with a timestamp in **your local time**:

```
[2026-09-08 20:44:36] playback: source added, sessionId=42 (active sources now 7)
```

So either **tell us roughly when it happened** ("around 20:45, right after we mounted up"), or grab
just the tail. Open PowerShell and paste:

```powershell
Get-Content "$env:APPDATA\Tfrs\Extension\extension.log" -Tail 500 | Set-Clipboard
```

That copies the last 500 lines to your clipboard, ready to paste into a bug report. If the problem
happened a while ago and you have kept playing since, send the whole file instead and tell us the
time — better too much than the wrong slice.

## What else to include

| | Why it matters |
|---|---|
| **Your in-game name** | The log identifies people by name and Steam ID; we need to know which one is you |
| **When it happened** (local time) | Lets us line your log up against the server's |
| **Who else was affected** | "Only me" and "all six of us at once" have completely different causes |
| **What you were doing** | On foot / in a vehicle / which radio / speaker mode on or off |

**If several people were affected, we need a log from each of them.** The file is written by your
own game, on your own PC — it only ever describes what *your* client saw. For "X can't hear Y", the
logs from X and Y together tell us far more than either one alone.

## Lines worth knowing

You do not need to interpret any of this — send the log and we will. But if you are curious, these
are the ones that usually matter:

| Line | Meaning |
|---|---|
| `connected to voice server, sessionId=…` | You are connected. Everything before this is startup. |
| `waiting: voice server not configured` | No server address set — a settings problem, not a bug |
| `handshake: … got zero reply` | Your packets left, nothing came back — network path or firewall |
| `connect rejected, reason=1` | Wrong voice server password |
| `connection lost: no datagram received for 15s` | The connection died; a reconnect follows |
| `capture: no usable microphone` | No mic could be opened at all |
| `capture: started, native format …` | Mic is live, with the format it is running at |
| `playback: started, device format …` | Headset/speakers are live |
| `transmit-gate: … -> shouldTransmit=1` | Your mic opened. `0` means it stayed closed, and the line says why |
| `radio-tx: START freq=…` | Your client announced a radio transmission |
| `playback: source added, sessionId=…` | You learned about another player's voice stream |

## A note on privacy

The log contains player names, Steam IDs and the voice server's address and port. It does **not**
contain audio, chat, passwords, or anything about your PC beyond the audio device format. If your
group treats Steam IDs as sensitive, send it in a DM rather than a public channel.
