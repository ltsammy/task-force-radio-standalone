# TFRS Voice Server

Reiner UDP-Relay-Server für den Task-Force-Radio-Standalone-Voice-Client. Er entkodiert/mischt keine
Audiodaten und berechnet kein 3D-Audio — er verteilt nur eingehende Opus-Pakete an alle anderen
verbundenen Clients, genau wie es der TeamSpeak-3-Server für TFAR bisher getan hat. Die gesamte
Positions-/Lautstärkeberechnung (3D-Audio, Funkverzerrung, Distanzdämpfung) passiert client-seitig,
gesteuert vom Arma-3-Addon. Dadurch ist der Server sehr leicht: keine Audio-DSP, kein Mixing,
kaum CPU-Last selbst bei 200-300 gleichzeitigen Verbindungen.

Protokoll-Details: [`../docs/protocol-network.md`](../docs/protocol-network.md).

## Lokal starten

```bash
cd src/Tfrs.VoiceServer
dotnet run -- --port 9987 --password changeme --max-clients 300
```

Konfiguration alternativ über Umgebungsvariablen (das nutzt das Docker-Image):

| Variable | Default | Bedeutung |
|---|---|---|
| `TFRS_PORT` | `9987` | UDP-Port |
| `TFRS_PASSWORD` | *(leer)* | Server-Passwort. Leer = offener Server (nicht empfohlen). |
| `TFRS_MAX_CLIENTS` | `300` | Maximale gleichzeitige Verbindungen |
| `TFRS_TIMEOUT_SECONDS` | `20` | Nach dieser Inaktivität (kein Ping/Voice-Paket) wird ein Client als getrennt behandelt |

## Docker

```bash
docker build -t tfrs-voice-server .
docker run --rm -p 9987:9987/udp -e TFRS_PASSWORD=changeme tfrs-voice-server
```

Oder mit Compose: `docker compose up --build`.

## Deploy auf Coolify

1. Neue Ressource → "Docker Compose" oder "Dockerfile" → dieses Repo, Build-Kontext `voice-server/`.
2. Port-Mapping als **UDP** setzen (nicht TCP!) — `9987/udp`.
3. `TFRS_PASSWORD` als Secret/Umgebungsvariable setzen.
4. Kein HTTP-Healthcheck möglich (reines UDP) — Coolify auf Container-Status-Healthcheck stellen
   oder Healthcheck ganz deaktivieren.

## Sizing

Der Server hält pro Client nur einen kleinen State-Eintrag (Endpoint, UID, Name, Timestamps) und
leitet Pakete zustandslos weiter — Speicherverbrauch und CPU-Last skalieren linear mit der Anzahl
gleichzeitig *sprechender* Clients, nicht mit der Gesamtzahl verbundener Clients. 200-300 Clients
mit üblichem Funkverkehr (wenige gleichzeitige Sprecher) laufen problemlos mit minimalen Docker-
Ressourcen (z. B. 0.5 vCPU / 128 MB reichen erfahrungsgemäß deutlich).
