# Task Force Radio Standalone — Arma 3 addon

`addons/` is forked unchanged from the original TFAR (class names, function names, CfgPatches
identical) — see the repo root [`README.md`](../README.md) for why that's safe. The only new part
is `extensions/task_force_radio_pipe/` (see its own [`README.md`](extensions/task_force_radio_pipe/README.md)),
which replaces the TeamSpeak shared-memory bridge with a named pipe to the new voice client.

## Building

Two independent pieces need to be built and combined:

1. **Extension DLLs** (C++, both architectures — Arma loads the matching one automatically):
   ```bash
   cd extensions/task_force_radio_pipe
   cmake -S . -B build/x64   -A x64   && cmake --build build/x64   --config Release
   cmake -S . -B build/win32 -A Win32 && cmake --build build/win32 --config Release
   ```
   Copy the resulting `task_force_radio_pipe.dll` and `task_force_radio_pipe_x64.dll` into
   `addon/` (the project root, next to `mod.cpp`) — `.hemtt/project.toml` picks them up from there.

2. **PBOs**, via [HEMTT](https://hemtt.dev/) (matches the `arma3_serverside` project's tooling):
   ```bash
   hemtt build      # fast, unsigned — for local iteration
   hemtt release     # signed, zipped into releases/ — for actual distribution
   ```

CI (`.github/workflows/addon.yml`) does both automatically: builds both extension
architectures, drops them into the project root, then runs `hemtt build` on pull requests
(validation only) or `hemtt release` on pushes to `main` (signed, zipped artifact).

## Signing

`hemtt release` signs every PBO and ships a matching `.bikey` in the release's `keys/` folder —
without that, dedicated servers with signature checking enabled will reject the addon.

**Current setup uses HEMTT's default behavior: a fresh signing key is generated for every
release build.** That's a deliberate simplification, not an oversight — HEMTT's mechanism for
reusing one stable key across releases (`hemtt keys generate`) produces a password-encrypted key
file that's explicitly meant for **local, interactive use only**; HEMTT refuses to use it in a CI
environment at all. Since every release zip bundles its own matching PBOs + `.bikey` together,
server admins who replace the whole `@mod` folder on update (the normal way to update any Arma
mod) never notice the key changing. This only matters if you need one long-lived, provably-stable
publisher identity key across versions — if that's needed later, someone with a real terminal
needs to run `hemtt keys generate` once, store the resulting `.hemttprivatekey` + password
somewhere safe, and the CI workflow would need to be changed to consume it (not currently wired
up).

## Steam Workshop

HEMTT has a built-in `hemtt publish` command — but it authenticates through the Steamworks client
API, which **requires a running, logged-in local Steam client**. That makes it a one-person,
run-it-on-your-own-machine command, not something a GitHub Actions runner can do.

Publishing is therefore split across machines: CI builds the extension DLLs (needs MSVC), and
[`publish-local.ps1`](publish-local.ps1) pulls those from the latest successful `addon` workflow
run and then runs `hemtt publish` locally, which builds+signs+zips+uploads using them:

```powershell
cd addon
./publish-local.ps1
```

Prerequisites: `gh` CLI authenticated (`gh auth login`), `hemtt` on PATH, Steam running and logged
into the publishing account. First run creates the Workshop item and writes `publishedid` into
`meta.cpp` — commit that so later runs update the same item instead of creating new ones.
