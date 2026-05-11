# Run this before building a release.
# Usage: .\update_version.ps1 v1.2.0
# If no argument given, uses the latest git tag.
param([string]$Tag)

if (-not $Tag) {
    $Tag = git -C $PSScriptRoot describe --tags --abbrev=0 2>$null
}
if (-not $Tag) { $Tag = "v0.0.0-dev" }

$content = "#pragma once`n#define FIRMWARE_VERSION `"$Tag`"`n"
Set-Content -Path "$PSScriptRoot\version.h" -Value $content -Encoding utf8

$json = "{\n  `"version`": `"$Tag`",\n  `"notes`": `"See release notes on GitHub`",\n  `"bin_url`": `"https://github.com/JensHjort/wine-cabinet-firmware/releases/download/$Tag/firmware.bin`"\n}`n"
Set-Content -Path "$PSScriptRoot\version.json" -Value $json -Encoding utf8

Write-Host "version.h and version.json updated to $Tag"
