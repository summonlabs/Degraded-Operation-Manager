# Degraded Operation Manager - configure, build and test helper.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# The script locates the MSVC toolchain through vswhere, imports the developer
# environment, and then drives CMake with Ninja. No timeout wrapper is applied
# to the test run: a hanging test is a defect and must be visible.

[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string]$Config = 'Release',

  [string]$BuildDir = '',

  [ValidateSet('none', 'asan', 'ubsan')]
  [string]$Sanitizer = 'none',

  [switch]$Analyze,
  [switch]$RunTests,
  [switch]$SkipTests,
  [switch]$SkipRuntime,
  [switch]$SkipTools,
  [switch]$SkipExamples,
  [switch]$Install,
  [string]$Prefix = '',
  [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Import-MsvcEnvironment {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found; install the Visual Studio Build Tools with C++ support.'
  }
  $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installation) {
    throw 'No Visual Studio installation with the C++ toolset was found.'
  }
  $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat was not found under $installation."
  }
  $quote = [char]34
  $command = 'call ' + $quote + $vcvars + $quote + ' >nul 2>&1 && set'
  $envDump = cmd.exe /c $command
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
      Set-Item -Path ('env:' + $Matches[1]) -Value $Matches[2]
    }
  }
  Write-Host "MSVC environment imported from $installation"
}

# Native tools write progress and diagnostics to stderr, which PowerShell would
# otherwise treat as a terminating error under $ErrorActionPreference = 'Stop'.
# Only the exit code decides whether a step failed.
function Invoke-Native([string]$description, [scriptblock]$body) {
  Write-Host "== $description"
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $body
  } finally {
    $ErrorActionPreference = $previous
  }
  if ($LASTEXITCODE -ne 0) {
    throw "$description failed with exit code $LASTEXITCODE"
  }
}

Import-MsvcEnvironment

if (-not $BuildDir) {
  $suffix = $Config.ToLowerInvariant()
  if ($Sanitizer -ne 'none') { $suffix = $suffix + '-' + $Sanitizer }
  $BuildDir = Join-Path $root ('build\' + $suffix)
}

$configureArgs = @(
  '-S', $root,
  '-B', $BuildDir,
  '-G', 'Ninja',
  ('-DCMAKE_BUILD_TYPE=' + $Config)
)
if ($SkipRuntime) { $configureArgs += '-DDOM_BUILD_RUNTIME=OFF' }
if ($SkipTests) { $configureArgs += '-DDOM_BUILD_TESTS=OFF' }
if ($SkipTools) { $configureArgs += '-DDOM_BUILD_TOOLS=OFF' }
if ($SkipExamples) { $configureArgs += '-DDOM_BUILD_EXAMPLES=OFF' }
if ($Sanitizer -eq 'asan') { $configureArgs += '-DDOM_ENABLE_ASAN=ON' }
if ($Sanitizer -eq 'ubsan') { $configureArgs += '-DDOM_ENABLE_UBSAN=ON' }
if ($Analyze) { $configureArgs += '-DDOM_ENABLE_ANALYZE=ON' }

$buildArgs = @('--build', $BuildDir)
if ($Jobs -gt 0) { $buildArgs += @('--parallel', "$Jobs") }

Invoke-Native ('configure: cmake ' + ($configureArgs -join ' ')) { cmake @configureArgs }
Invoke-Native "build $Config" { cmake @buildArgs }
if ($RunTests) {
  Invoke-Native 'run the test suite' { ctest --test-dir $BuildDir --output-on-failure }
}
if ($Install) {
  if (-not $Prefix) { $Prefix = Join-Path $root ('install\' + $Config.ToLowerInvariant()) }
  Invoke-Native "install to $Prefix" { cmake --install $BuildDir --prefix $Prefix }
  Write-Host "Installed to $Prefix"
}

Write-Host "Build directory: $BuildDir"
