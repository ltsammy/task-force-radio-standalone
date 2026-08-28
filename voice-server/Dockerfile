# Native AOT build: the shipped image contains a self-contained native binary, no .NET
# runtime/CLR at all. Small image, near-instant cold start, low idle memory — comparable to a
# Go/Rust relay — while the source stays plain, debuggable C# shared in spirit with the client.
FROM mcr.microsoft.com/dotnet/sdk:8.0-alpine AS build
RUN apk add --no-cache clang build-base zlib-dev
WORKDIR /src
COPY src/Tfrs.VoiceServer/Tfrs.VoiceServer.csproj Tfrs.VoiceServer/
RUN dotnet restore Tfrs.VoiceServer/Tfrs.VoiceServer.csproj -r linux-musl-x64
COPY src/Tfrs.VoiceServer/ Tfrs.VoiceServer/
RUN dotnet publish Tfrs.VoiceServer/Tfrs.VoiceServer.csproj -c Release -r linux-musl-x64 -o /app

# runtime-deps, not runtime: no managed CLR needed for an AOT-compiled native executable.
FROM mcr.microsoft.com/dotnet/runtime-deps:8.0-alpine AS runtime
WORKDIR /app
RUN addgroup -S tfrs && adduser -S tfrs -G tfrs
COPY --from=build /app/tfrs-voice-server .
USER tfrs

# UDP-only service — no health probe port. Coolify/Docker health checks should
# use a raw UDP send/recv (see docs/protocol-network.md) or just container-status.
EXPOSE 9987/udp

ENV TFRS_PORT=9987 \
    TFRS_MAX_CLIENTS=300 \
    TFRS_TIMEOUT_SECONDS=20

ENTRYPOINT ["./tfrs-voice-server"]
