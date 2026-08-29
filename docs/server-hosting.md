# Hosting a server

Everything a mission needs: the addon on both server and clients (via Steam Workshop), and one
running `voice-server` instance that every player's addon connects to.

## 1. Install the addon + CBA_A3

[CBA_A3](https://steamcommunity.com/sharedfiles/filedetails/?id=450814997) is a required
dependency, Steam Workshop only.

For the addon itself, the server has two options:

- **SteamCMD** (standard Arma 3 server workflow): subscribe the account the server runs under, or
  `+workshop_download_item 107410 <this item's id>`.
- **This release's attached zip**: a complete, pre-built, pre-signed `@tfar/` folder (PBOs +
  both extension DLLs + signing key) — extract it straight into your server's mod directory, no
  Steam account needed on the server at all. Rebuilt and re-attached whenever a new version ships,
  so it always matches the current Workshop version.

Clients still need to subscribe to the Workshop item as normal (this is about the *server's* copy).

Launch parameter on both client and server: `-mod=@CBA_A3;@tfar` (or `@task_force_radio` if using
the Workshop copy's folder name) plus whatever else the mission needs.

## 2. Deploy `voice-server`

This is the one piece that needs its own always-on process, separate from the Arma server itself.
Full instructions, config table, and Coolify-specific steps: [`../voice-server/README.md`](../voice-server/README.md).
Quick version:

```bash
docker run -d --restart unless-stopped -p 9987:9987/udp \
  -e TFRS_PASSWORD=<pick one> \
  ghcr.io/<this-repo>-voice-server:latest
```

Or `docker compose up -d` from `voice-server/` using its `docker-compose.yml` +
`.env.example` (copy to `.env` and fill in `TFRS_PASSWORD`).

## 3. Open the right ports

| Port | Protocol | For |
|---|---|---|
| `2302` | UDP | Arma 3 game port (required) |
| `2303` | UDP | Steam query port (only needed for the in-game server browser; skip it if players will direct-connect) |
| `9987` (or whatever you set `TFRS_PORT` to) | UDP | `voice-server` |

Forward these on your router to the machine running the server, and allow them through Windows
Firewall / your Linux firewall if applicable. If the game server and `voice-server` run on
different machines, each needs its own forwarding.

## 4. Configure the voice server address — do this via userconfig, not in-game

The in-game Voice Server Address/Port/Password settings (Configure Addons → TFAR, or Game Options →
Server if you're logged in as admin) are [CBA_Settings](https://github.com/CBATeam/CBA_A3/wiki/Settings-Framework),
and CBA_Settings supports pre-setting values from a file so players (and you) never have to type
them in manually. Create `@task_force_radio/userconfig/tfar_voice.hpp` next to the addon on the
**server**:

```cpp
class CBA_Settings {
    class TFAR_Voice_ServerHost { value = "your.server.ip.or.hostname"; };
    class TFAR_Voice_ServerPort { value = "9987"; };
    class TFAR_Voice_ServerPassword { value = "<same password as TFRS_PASSWORD above>"; };
};
```

These three are global settings (`isGlobal=1` — the server's value is authoritative and syncs to
every connected client), so this only needs to exist on the server, not on each player's client.
Per-client settings (mic/speaker volume, VAD threshold, transmit mode) can go in the same file or
be left for each player to set for themselves — they default to sensible values either way.

## 5. Troubleshooting

- **"Not connected to TeamSpeak" hint in-game / nobody can hear anything**: almost always means
  Voice Server Address is empty or wrong — check step 4. `%APPDATA%\Tfrs\Extension\extension.log`
  on the client logs exactly what it's waiting for or why a connection attempt failed.
- **Connects locally but not for others**: usually a port-forwarding or firewall gap — re-check
  step 3, specifically that `voice-server`'s UDP port (not just Arma's) is forwarded.
- **Testing solo with two Arma clients on one PC/Steam account**: the dedicated server will kick
  the second connection ("player with same SteamID already in game") — that's a hardcoded engine
  safeguard, not something `server.cfg` can disable for real multiplayer, but a local-only test
  server can set `kickDuplicate = 0;` to allow it. `voice-server` separately keys sessions by UID
  and will drop the first session's *voice* connection the moment the second one connects (by
  design — this is what lets a real client reconnect cleanly after a network drop), so this only
  gets you two connected Arma characters, not two working voice connections. A genuine second test
  needs a second Steam identity (another account, or a friend).
