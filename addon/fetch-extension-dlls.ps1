<#
.SYNOPSIS
    Downloads the extension DLLs from the latest successful `addon` CI run into this folder.

.DESCRIPTION
    Shared by build-local.ps1 and publish-local.ps1. The extension DLLs need MSVC to build, which
    only CI has here; PBO building (hemtt build/release/publish) needs a P-Drive or a real Arma 3
    install, which only a local machine has (see README.md "Known HEMTT build findings"). So the
    two pieces are built on different machines and stitched together via this script.

.PREREQUISITES
    GitHub CLI (`gh`), authenticated (`gh auth login`). Run from inside a checkout of this repo.
#>

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

Write-Host "Finding latest successful 'addon' workflow run on main..."
$runId = gh run list --workflow=addon.yml --branch=main --status=success --limit=1 --json databaseId --jq ".[0].databaseId"
if (-not $runId) {
    throw "No successful 'addon' workflow run found. Push to main first so CI builds the extension DLLs."
}
Write-Host "Using run $runId"

$tmp = Join-Path $env:TEMP "tfrs-extension-dlls-$runId"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $tmp | Out-Null

gh run download $runId --name extension-x64   --dir $tmp
gh run download $runId --name extension-win32 --dir $tmp

Copy-Item (Join-Path $tmp "task_force_radio_pipe_x64.dll") . -Force
Copy-Item (Join-Path $tmp "task_force_radio_pipe.dll") . -Force
Write-Host "Copied extension DLLs from CI run $runId into $PSScriptRoot"
