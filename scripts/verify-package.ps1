#!/usr/bin/env pwsh
# Install the package into a private prefix and build the independent consumer
# against it. This is the downstream find_package proof.
[CmdletBinding()]
param(
  [string] $Config = "Release",
  [string] $BuildDir = "build",
  [string] $Prefix = "build/install",
  [string] $ConsumerDir = "build/downstream"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
  cmake --install $BuildDir --config $Config --prefix $Prefix
  if ($LASTEXITCODE -ne 0) { throw "install failed" }

  $resolved = (Resolve-Path $Prefix).Path
  cmake -S examples/downstream -B $ConsumerDir -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$resolved"
  if ($LASTEXITCODE -ne 0) { throw "downstream configure failed" }

  cmake --build $ConsumerDir --config $Config
  if ($LASTEXITCODE -ne 0) { throw "downstream build failed" }

  & "$ConsumerDir/$Config/downstream_consumer.exe"
  if ($LASTEXITCODE -ne 0) { throw "downstream consumer reported failure" }
  Write-Host "downstream find_package proof: OK"
} finally {
  Pop-Location
}
