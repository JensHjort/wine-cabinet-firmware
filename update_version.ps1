$hash = git -C $PSScriptRoot rev-parse --short HEAD 2>$null
if (-not $hash) { $hash = "unknown" }
$content = "#pragma once`n#define FIRMWARE_VERSION `"$hash`"`n"
Set-Content -Path "$PSScriptRoot\version.h" -Value $content -Encoding utf8
Write-Host "version.h updated: $hash"
