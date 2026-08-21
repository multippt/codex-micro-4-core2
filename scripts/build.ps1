[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("core2", "tab5", "all")]
    [string] $Target = "core2",

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]] $PlatformIOArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$localPio = Join-Path $projectRoot ".pio-core\penv\Scripts\pio.exe"
$pioCommand = Get-Command pio -ErrorAction SilentlyContinue

if ($null -ne $pioCommand) {
    $pioExecutable = $pioCommand.Source
} elseif (Test-Path -LiteralPath $localPio) {
    $pioExecutable = $localPio
} else {
    throw "PlatformIO Core was not found. Install 'pio' or make the project-local .pio-core runtime available."
}

$targets = switch ($Target) {
    "core2" { @(@{ Name = "core2"; Environment = "m5stack-core2" }) }
    "tab5"  { @(@{ Name = "tab5"; Environment = "m5stack-tab5" }) }
    "all"   {
        @(
            @{ Name = "core2"; Environment = "m5stack-core2" }
            @{ Name = "tab5"; Environment = "m5stack-tab5" }
        )
    }
}

$previousCoreDir = $env:PLATFORMIO_CORE_DIR
$previousPackagesDir = $env:PLATFORMIO_PACKAGES_DIR
$previousBuildDir = $env:PLATFORMIO_BUILD_DIR
$previousTempDir = $env:TEMP
$previousTmpDir = $env:TMP
try {
    $env:PLATFORMIO_CORE_DIR = Join-Path $projectRoot ".pio-core"
    $buildTempDir = Join-Path $projectRoot ".pio-tmp"
    New-Item -ItemType Directory -Force -Path $buildTempDir | Out-Null
    $env:TEMP = $buildTempDir
    $env:TMP = $buildTempDir
    foreach ($buildTarget in $targets) {
        $packagesDir = Join-Path $projectRoot ".pio-packages\$($buildTarget.Name)"
        $buildDir = Join-Path $projectRoot ".pio\build-$($buildTarget.Name)"
        $env:PLATFORMIO_PACKAGES_DIR = $packagesDir
        $env:PLATFORMIO_BUILD_DIR = $buildDir

        Write-Host "Building $($buildTarget.Environment) with isolated packages and build state"
        Write-Host "  Packages: $packagesDir"
        Write-Host "  Build:    $buildDir"
        & $pioExecutable run --project-dir $projectRoot -e $buildTarget.Environment @PlatformIOArgs
        if ($LASTEXITCODE -ne 0) {
            throw "PlatformIO failed for $($buildTarget.Environment) with exit code $LASTEXITCODE."
        }
    }
} finally {
    $env:PLATFORMIO_CORE_DIR = $previousCoreDir
    $env:PLATFORMIO_PACKAGES_DIR = $previousPackagesDir
    $env:PLATFORMIO_BUILD_DIR = $previousBuildDir
    $env:TEMP = $previousTempDir
    $env:TMP = $previousTmpDir
}
