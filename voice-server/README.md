# TFRS Voice Server

Pure UDP relay server for the Task Force Radio Standalone voice client. It doesn't decode/mix any
audio and doesn't compute 3D audio — it just forwards incoming Opus packets to every other
connected client, exactly like the TeamSpeak 3 server used to do for TFAR. All position/volume
computation (3D audio, radio distortion, distance attenuation) happens client-side, driven by the
Arma 3 addon. That keeps the server very light: no audio DSP, no mixing, barely any CPU load even
at 200-300 concurrent connections.

Protocol details: [`../docs/protocol-network.md`](../docs/protocol-network.md).

## Run locally

```bash
cd src/Tfrs.VoiceServer
dotnet run -- --port 9987 --password changeme --max-clients 300
```

Configuration can also come from environment variables (this is what the Docker image uses):

| Variable | Default | Meaning |
|---|---|---|
| `TFRS_PORT` | `9987` | UDP port |
| `TFRS_PASSWORD` | *(empty)* | Server password. Empty = open server (not recommended). |
| `TFRS_MAX_CLIENTS` | `300` | Maximum concurrent connections |
| `TFRS_TIMEOUT_SECONDS` | `20` | A client with no activity (no ping/voice packet) for this long is treated as disconnected |

## Docker

```bash
docker build -t tfrs-voice-server .
docker run --rm -p 9987:9987/udp -e TFRS_PASSWORD=changeme tfrs-voice-server
```

Or with Compose: `docker compose up --build`.

## Deploying on Coolify

1. New resource → "Docker Compose" or "Dockerfile" → this repo, build context `voice-server/`.
2. Set the port mapping as **UDP** (not TCP!) — `9987/udp`.
3. Set `TFRS_PASSWORD` as a secret/environment variable.
4. No HTTP health check is possible (pure UDP) — set Coolify to a container-status health check,
   or disable health checks entirely.

## Sizing

The server keeps only a small state entry per client (endpoint, UID, name, timestamps) and
forwards packets statelessly — memory usage and CPU load scale linearly with the number of
*currently talking* clients, not the total number connected. 200-300 clients with typical radio
traffic (few simultaneous speakers) run comfortably on minimal Docker resources (e.g. 0.5 vCPU /
128MB is generally plenty in practice).
