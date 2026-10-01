# Degraded Operation Manager - packaging and downstream closure.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Stages:
#   install   configure, build, test and install into a clean prefix;
#   consumer  build and run an independent out-of-tree consumer against that
#             prefix with find_package, outside the repository;
#   fresh     clone the configured remote into a disposable directory, run the
#             whole closure there (Release and Debug) and delete the clone.
# Scratch directories live outside the repository and are removed afterwards.

[CmdletBinding()]
param(
  [ValidateSet('all', 'install', 'consumer', 'fresh')]
  [string]$Stage = 'all',

  [ValidateSet('Release', 'Debug')]
  [string]$Config = 'Release',

  [string]$Prefix = '',
  [string]$Scratch = '',
  # The fresh stage clones the configured remote URL by default, so it proves
  # the artefact that was actually pushed rather than a local directory.
  [string]$Remote = '',
  [string]$Ref = 'main'
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
  $quote = [char]34
  $command = 'call ' + $quote + (Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat') + $quote + ' >nul 2>&1 && set'
  foreach ($line in (cmd.exe /c $command)) {
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

function New-ScratchDirectory([string]$label) {
  $base = if ($Scratch) { $Scratch } else { [System.IO.Path]::GetTempPath() }
  $path = Join-Path $base ('dom-' + $label + '-' + $PID + '-' + (Get-Random))
  New-Item -ItemType Directory -Path $path -Force | Out-Null
  return $path
}

function Build-And-Test([string]$repository, [string]$buildDirectory, [string]$configuration) {
  Invoke-Native "configure $configuration in $buildDirectory" {
    cmake -S $repository -B $buildDirectory -G Ninja ('-DCMAKE_BUILD_TYPE=' + $configuration)
  }
  Invoke-Native "build $configuration" { cmake --build $buildDirectory }
  Invoke-Native "test $configuration" { ctest --test-dir $buildDirectory --output-on-failure }
}

function Install-Prefix([string]$buildDirectory, [string]$installPrefix) {
  if (Test-Path -LiteralPath $installPrefix) {
    Remove-Item -LiteralPath $installPrefix -Recurse -Force
  }
  Invoke-Native "install to $installPrefix" {
    cmake --install $buildDirectory --prefix $installPrefix
  }
}

function Build-And-Run-Consumer([string]$repository, [string]$installPrefix, [string]$configuration) {
  $consumerBuild = New-ScratchDirectory 'consumer-build'
  $store = New-ScratchDirectory 'consumer-store'
  try {
    Invoke-Native 'configure the downstream consumer' {
      cmake -S (Join-Path $repository 'examples\consumer') -B $consumerBuild -G Ninja ('-DCMAKE_BUILD_TYPE=' + $configuration) ('-DCMAKE_PREFIX_PATH=' + $installPrefix)
    }
    Invoke-Native 'build the downstream consumer' { cmake --build $consumerBuild }
    Invoke-Native 'run the downstream consumer against the installed package' {
      & (Join-Path $consumerBuild 'dom_consumer.exe') (Join-Path $store 'store')
    }
  } finally {
    Remove-Item -LiteralPath $consumerBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $store -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Import-MsvcEnvironment

if (-not $Prefix) {
  $Prefix = Join-Path $root ('install\' + $Config.ToLowerInvariant())
}

if ($Stage -eq 'install' -or $Stage -eq 'all') {
  $buildDirectory = Join-Path $root ('build\' + $Config.ToLowerInvariant())
  Build-And-Test $root $buildDirectory $Config
  Install-Prefix $buildDirectory $Prefix
}

if ($Stage -eq 'consumer' -or $Stage -eq 'all') {
  if (-not (Test-Path -LiteralPath (Join-Path $Prefix 'lib\cmake\DegradedOperationManager'))) {
    throw "no installed package under $Prefix; run -Stage install first."
  }
  Build-And-Run-Consumer $root $Prefix $Config
}

if ($Stage -eq 'fresh') {
  if (-not $Remote) {
    $Remote = (& git -C $root remote get-url origin)
    if ($LASTEXITCODE -ne 0 -or -not $Remote) {
      throw 'no origin remote is configured; pass -Remote explicitly.'
    }
  }
  $clone = New-ScratchDirectory 'fresh-clone'
  try {
    Invoke-Native "clone $Remote $Ref into $clone" {
      git clone --branch $Ref $Remote (Join-Path $clone 'source')
    }
    $source = Join-Path $clone 'source'
    # The clone drives itself with its own committed scripts: configure, build,
    # test, install and run the downstream consumer, in both configurations.
    foreach ($configuration in @('Release', 'Debug')) {
      $freshPrefix = Join-Path $clone ('prefix-' + $configuration.ToLowerInvariant())
      Invoke-Native "closure inside the fresh clone ($configuration)" {
        & (Join-Path $source 'scripts\closure.ps1') -Stage all -Config $configuration -Prefix $freshPrefix
      }
    }
  } finally {
    Remove-Item -LiteralPath $clone -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host "closure stage '$Stage' completed"
