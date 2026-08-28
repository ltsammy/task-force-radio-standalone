# Task Force Radio Standalone — Arma 3 addon

`addons/` is forked unchanged from the original TFAR (class names, function names, CfgPatches
identical) — see the repo root [`README.md`](../README.md) for why that's safe. The only new part
is `extensions/task_force_radio_pipe/` (see its own [`README.md`](extensions/task_force_radio_pipe/README.md)),
which replaces the TeamSpeak shared-memory bridge with a named pipe to the new voice client.

## Building

Two independent pieces need to be built, **on two different machines**, and combined:

1. **Extension DLLs** (C++, both architectures — Arma loads the matching one automatically) —
   built by **CI** (`.github/workflows/addon.yml`, needs only MSVC, which GitHub-hosted runners
   have). To build locally instead:
   ```bash
   cd extensions/task_force_radio_pipe
   cmake -S . -B build/x64   -A x64   && cmake --build build/x64   --config Release
   cmake -S . -B build/win32 -A Win32 && cmake --build build/win32 --config Release
   ```

2. **PBOs**, via [HEMTT](https://hemtt.dev/) (matches the `arma3_serverside` project's tooling) —
   built **locally**, on a machine with Arma 3 installed (needed to resolve one BI include — see
   "Known HEMTT build findings" below; GitHub-hosted runners don't have Arma 3, so this step can't
   run in CI). [`build-local.ps1`](build-local.ps1) pulls the CI-built DLLs and runs `hemtt
   release`/`build` with them in one step:
   ```powershell
   cd addon
   ./build-local.ps1          # signed, zipped into releases/
   ./build-local.ps1 -Dev     # fast, unsigned, for iteration
   ```
   Prerequisite: `gh` CLI authenticated (`gh auth login`), `hemtt` on PATH.

If you'd rather assemble the DLLs yourself (e.g. testing a local extension change before pushing),
copy `task_force_radio_pipe.dll`/`task_force_radio_pipe_x64.dll` into `addon/` (next to `mod.cpp`)
and run `hemtt build`/`release` directly — `build-local.ps1` only automates fetching them from CI.

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
run-it-on-your-own-machine command, not something a GitHub Actions runner can do — same reason PBO
building itself is local-only (see "Building" above). [`publish-local.ps1`](publish-local.ps1)
fetches the CI-built DLLs the same way `build-local.ps1` does, then runs `hemtt publish`, which
builds+signs+zips+uploads using them:

```powershell
cd addon
./publish-local.ps1
```

Prerequisites: `gh` CLI authenticated (`gh auth login`), `hemtt` on PATH, Steam running and logged
into the publishing account. First run creates the Workshop item and writes `publishedid` into
`meta.cpp` — commit that so later runs update the same item instead of creating new ones.

## Known HEMTT build findings

`addons/` builds under HEMTT after fixing syntax HEMTT is stricter about than the legacy tooling
(addonbuilder/pboProject) — all purely syntactic, verified against the rapified output, no
behavior change:

- ~994 unquoted array/expression values (`controls[]={background,...}` → `{"background",...}`,
  and GUI positioning math like `x = 0.85 * safezoneW;` → `x = "0.85 * safezoneW";` — both
  documented HEMTT idioms for values older tools auto-quoted silently).
- 2 `true`/`false` barewords converted to `1`/`0` instead of quoted, to keep their rapified type as
  Number rather than String.
- A parent-class case mismatch (`class Controls: controls` → `: Controls`) in
  `addons/static_radios/CfgVehicles.hpp` — Arma is case-insensitive here, HEMTT isn't.
- `VERSION_CONFIG` (`addons/core/script_mod.hpp`) overridden locally so the legacy `version`
  scalar gets a real single number (`BUILD`) instead of the unquoted 4-part
  `MAJOR.MINOR.PATCHLVL.BUILD` expression CBA's default macro produces — `MINOR` is `-1` in
  `script_version.hpp`, so that expression was never a valid single Arma number to begin with;
  `versionStr`/`versionAr[]` keep the full, unabbreviated version info unchanged.
- `PATHTOF(...)` → `QPATHTOF(...)` in one `CfgSounds.hpp` entry (CBA's own quoted variant).
- `REQUIRED_VERSION` bumped from `1.72` to `2.02` in `script_mod.hpp` — `addons/core` uses
  `createHashMap`, which needs 2.02; the declared minimum was just stale (every real Arma 3
  install is already well past this).
- A missing debug-trace argument (`TRACE_2(_unit,_distance)` → `TRACE_2("revealInArea",_unit,_distance)`
  in `fnc_revealInArea.sqf`) — `TRACE_2` compiles to nothing outside debug builds, zero release impact.
- **Renamed 8 preprocessor-only fragment files from `.sqf` to `.hpp`** (the 7 `XEH_PREP.sqf` files
  plus `functions/flexiUI/flexiInit.sqf`, all `#include`d textually into a file that already has
  macro context, never meant to be compiled standalone) — matches the convention ACE3 and other
  HEMTT-built addons use for exactly this situation. HEMTT's SQF analyzer independently parses
  every `.sqf` file it finds as if it were complete, standalone SQF; with the macros they reference
  unresolved outside their intended `#include` context, that parse fails. `.hpp` signals
  "preprocessor fragment, don't lint standalone" the same way it already does for config fragments.
- `addons/core/functions/fnc_initKeybinds.sqf` included BI's own
  `\a3\editor_f\Data\Scripts\dikCodes.h` for 8 `DIK_*` keycode constants — that absolute engine
  path needs a P-Drive/Arma 3 install to resolve at build time. Replaced with a local
  `dikCodes_local.h` containing just those 8 constants (standard, unchanging DirectInput scancode
  values, not Arma-specific content).
- `[lints.sqf] format_args = "Warning"` in `project.toml`: 4 `format [...]` calls (2 in
  `fnc_addWirelessIntercomMenu.sqf`, 2 in `fnc_connect.sqf`) pass an argument their format string
  has no `%1` for. Harmless at runtime (SQF's `format` ignores unused args), but the extra
  argument (a channel ID, a headgear name) looks like it may have been intended to appear in the
  message/action-ID — deliberately **not changed**, since fixing that would be a content/behavior
  decision outside a build-compatibility pass, not just a syntax one.

**One remaining item, and the whole reason PBO building lives locally instead of in CI:**
`addons/static_radios/functions/fnc_zeusAttributes.sqf` `#include`s BI's own
`\a3\ui_f_curator\UI\Displays\RscDisplayAttributes.sqf` (the base Zeus attribute-display UI class
it extends) — unlike `dikCodes.h`, this is a large, complex BI-authored UI class body, not a small
constant table, so it wasn't vendored. Per HEMTT's
[P-Drive documentation](https://hemtt.dev/configuration/p-drive/), this resolves automatically on
a machine with an actual Arma 3 installation (falls back to extracting the file from the game
install if no P-Drive is mounted) — which is exactly why `build-local.ps1`/`publish-local.ps1`
exist: run on a machine with Arma 3 installed, this isn't an issue at all. GitHub-hosted runners
have no Arma 3 install, so `.github/workflows/addon.yml` only builds the extension DLLs and never
attempts a PBO build. Making PBO building CI-possible would need either a P-Drive-equivalent CI
setup (e.g. `arma-actions/arma3-tools`, which needs access to BI's Arma 3 Tools depot) or someone
confirming it's safe to vendor a minimal stand-in for this one BI file — not done here.
