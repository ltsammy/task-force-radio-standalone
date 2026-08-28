<#
.SYNOPSIS
    Pulls the latest CI-built extension DLLs and publishes the addon to the Steam Workshop.

.DESCRIPTION
    hemtt publish needs a locally running, logged-in Steam client (see README.md "Steam Workshop")
    so it can't run in GitHub Actions. This script instead does the two parts split across
    machines: it downloads the extension DLLs (built by CI, which has MSVC) from the latest
    successful `addon` workflow run on GitHub, drops them into this folder, then runs
    `hemtt publish` locally so it builds+signs+zips+uploads using those DLLs.

.PREREQUISITES
    - GitHub CLI (`gh`), authenticated (`gh auth login`)
    - `hemtt` on PATH
    - Steam running and logged into the account that owns/publishes this Workshop item
    - Run from inside a checkout of this repo (gh infers the repo from the git remote)

.NOTES
    First run: hemtt will prompt to create a new Workshop item and writes the resulting
    `publishedid` into meta.cpp — commit that afterward so future runs update the same item
    instead of creating a new one.
#>

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

Write-Host "Finding latest successful 'addon' workflow run on main..."
$runId = gh run list --workflow=addon.yml --branch=main --status=success --limit=1 --json databaseId --jq ".[0].databaseId"
if (-not $runId) {
    throw "No successful 'addon' workflow run found. Push to main first so CI builds the extension DLLs."
}
Write-Host "Using run $runId"

$tmp = Join-Path $env:TEMP "tfrs-publish-$runId"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $tmp | Out-Null

gh run download $runId --name extension-x64   --dir $tmp
gh run download $runId --name extension-win32 --dir $tmp

Copy-Item (Join-Path $tmp "task_force_radio_pipe_x64.dll") . -Force
Copy-Item (Join-Path $tmp "task_force_radio_pipe.dll") . -Force
Write-Host "Copied extension DLLs from CI run $runId into $PSScriptRoot"

Write-Host "Running hemtt publish (needs Steam running and logged in)..."
hemtt publish
