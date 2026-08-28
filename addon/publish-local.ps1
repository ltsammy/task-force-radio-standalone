<#
.SYNOPSIS
    Pulls the latest CI-built extension DLLs and publishes the addon to the Steam Workshop.

.DESCRIPTION
    hemtt publish needs a locally running, logged-in Steam client (see README.md "Steam Workshop")
    so it can't run in GitHub Actions — same reason PBO building itself is local-only (see
    build-local.ps1). This fetches the CI-built extension DLLs, then runs `hemtt publish` locally,
    which builds+signs+zips+uploads using them.

.PREREQUISITES
    - GitHub CLI (`gh`), authenticated (`gh auth login`)
    - `hemtt` on PATH
    - Arma 3 installed (for the P-Drive fallback HEMTT uses to resolve BI includes)
    - Steam running and logged into the account that owns/publishes this Workshop item
    - Run from inside a checkout of this repo

.NOTES
    First run: hemtt will prompt to create a new Workshop item and writes the resulting
    `publishedid` into meta.cpp — commit that afterward so future runs update the same item
    instead of creating a new one.
#>

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

& "$PSScriptRoot\fetch-extension-dlls.ps1"

Write-Host "Running hemtt publish (needs Steam running and logged in)..."

# See build-local.ps1 for why: hemtt's own non-fatal stderr lint output can otherwise be turned
# into a terminating error under $ErrorActionPreference = "Stop", aborting mid-publish.
$previousEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& hemtt publish
$hemttExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousEap

if ($hemttExitCode -ne 0) {
    throw "hemtt publish failed with exit code $hemttExitCode"
}
