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

function New-ScratchDirectory([string]$label) {
  $base = if ($Scratch) { $Scratch } else { [System.IO.Path]::GetTempPath() }
  $path = Join-Path $base ('dom-' + $label + '-' + $PID + '-' + (Get-Random))
  New-Item -ItemType Directory -Path $path -Force | Out-Null
  return $path
}

function Build-And-Test([string]$repository, [string]$buildDirectory, [string]$configuration) {
  Write-Host "== configure $configuration in $buildDirectory"
  & cmake -S $repository -B $buildDirectory -G Ninja ('-DCMAKE_BUILD_TYPE=' + $configuration)
  if ($LASTEXITCODE -ne 0) { throw "configure failed with exit code $LASTEXITCODE" }
  Write-Host "== build $configuration"
  & cmake --build $buildDirectory
  if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
  Write-Host "== test $configuration"
  & ctest --test-dir $buildDirectory --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "tests failed with exit code $LASTEXITCODE" }
}

function Install-Prefix([string]$buildDirectory, [string]$installPrefix) {
  if (Test-Path -LiteralPath $installPrefix) {
    Remove-Item -LiteralPath $installPrefix -Recurse -Force
  }
  Write-Host "== install to $installPrefix"
  & cmake --install $buildDirectory --prefix $installPrefix
  if ($LASTEXITCODE -ne 0) { throw "install failed with exit code $LASTEXITCODE" }
}

function Build-And-Run-Consumer([string]$repository, [string]$installPrefix, [string]$configuration) {
  $consumerBuild = New-ScratchDirectory 'consumer-build'
  $store = New-ScratchDirectory 'consumer-store'
  try {
    Write-Host '== configure the downstream consumer'
    & cmake -S (Join-Path $repository 'examples\consumer') -B $consumerBuild -G Ninja ('-DCMAKE_BUILD_TYPE=' + $configuration) ('-DCMAKE_PREFIX_PATH=' + $installPrefix)
    if ($LASTEXITCODE -ne 0) { throw "consumer configure failed with exit code $LASTEXITCODE" }
    Write-Host '== build the downstream consumer'
    & cmake --build $consumerBuild
    if ($LASTEXITCODE -ne 0) { throw "consumer build failed with exit code $LASTEXITCODE" }
    Write-Host '== run the downstream consumer against the installed package'
    & (Join-Path $consumerBuild 'dom_consumer.exe') (Join-Path $store 'store')
    if ($LASTEXITCODE -ne 0) { throw "consumer run failed with exit code $LASTEXITCODE" }
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
    Write-Host "== clone $Remote $Ref into $clone"
    & git clone --branch $Ref $Remote (Join-Path $clone 'source')
    if ($LASTEXITCODE -ne 0) { throw "clone failed with exit code $LASTEXITCODE" }
    $source = Join-Path $clone 'source'
    foreach ($configuration in @('Release', 'Debug')) {
      $buildDirectory = Join-Path $clone ('build-' + $configuration.ToLowerInvariant())
      Build-And-Test $source $buildDirectory $configuration
      $freshPrefix = Join-Path $clone ('prefix-' + $configuration.ToLowerInvariant())
      Install-Prefix $buildDirectory $freshPrefix
      Build-And-Run-Consumer $source $freshPrefix $configuration
    }
  } finally {
    Remove-Item -LiteralPath $clone -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host "closure stage '$Stage' completed"
