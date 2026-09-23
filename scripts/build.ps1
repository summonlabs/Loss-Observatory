#!/usr/bin/env pwsh
# Configure and build Loss Observatory.
#
# Usage: pwsh scripts/build.ps1 [-Config Debug] [-BuildDir build] [-Asan]
[CmdletBinding()]
param(
  [string] $Config = "Debug",
  [string] $BuildDir = "build",
  [switch] $Asan
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
  $cmakeArgs = @("-S", ".", "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64")
  if ($Asan) { $cmakeArgs += "-DLO_ENABLE_ASAN=ON" }
  cmake @cmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  cmake --build $BuildDir --config $Config
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  Write-Host "build complete: $BuildDir ($Config)"
} finally {
  Pop-Location
}
