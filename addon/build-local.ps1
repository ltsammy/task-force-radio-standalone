<#
.SYNOPSIS
    Pulls the latest CI-built extension DLLs and builds the addon's PBOs locally.

.DESCRIPTION
    PBO building (hemtt build/release) needs a P-Drive or a real Arma 3 installation to resolve
    one BI include (Zeus's RscDisplayAttributes.sqf — see README.md "Known HEMTT build findings"),
    which GitHub-hosted runners don't have. So PBOs are built here, locally, on a machine that has
    Arma 3 installed, using extension DLLs fetched from the latest CI run (CI has MSVC, which this
    machine might not).

.PARAMETER Dev
    Fast, unsigned build (`hemtt build`) for local iteration — output stays in .hemttout/build,
    nothing is zipped. Default is a full signed release (`hemtt release`), zipped into releases/.

.PREREQUISITES
    - GitHub CLI (`gh`), authenticated (`gh auth login`)
    - `hemtt` on PATH
    - Arma 3 installed (for the P-Drive fallback HEMTT uses to resolve BI includes)
    - Run from inside a checkout of this repo
#>

param(
    [switch]$Dev
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

& "$PSScriptRoot\fetch-extension-dlls.ps1"

if ($Dev) {
    Write-Host "Running hemtt build (fast, unsigned)..."
    hemtt build
} else {
    Write-Host "Running hemtt release (signed, zipped into releases/)..."
    hemtt release
}
