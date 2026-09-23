#!/usr/bin/env pwsh
# Run the full test suite.
#
# The AddressSanitizer runtime ships with the MSVC toolchain and is not on PATH
# by default, so this script adds it before running an instrumented build.
#
# Usage: pwsh scripts/test.ps1 [-Config Debug] [-BuildDir build] [-Asan]
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
  if ($Asan) {
    $programFilesX86 = (Get-Item "Env:ProgramFiles(x86)" -ErrorAction SilentlyContinue).Value
    if ($programFilesX86) {
      $vcRoot = Join-Path $programFilesX86 "Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC"
      if (Test-Path $vcRoot) {
        $runtime = Get-ChildItem $vcRoot -Recurse -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
          Select-Object -First 1
        if ($runtime) {
          $env:PATH = $runtime.DirectoryName + ";" + $env:PATH
          Write-Host "asan runtime: $($runtime.DirectoryName)"
        } else {
          Write-Warning "AddressSanitizer runtime not found; instrumented tests may fail to start"
        }
      }
    }
  }
  ctest --test-dir $BuildDir -C $Config --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "tests failed" }
} finally {
  Pop-Location
}
