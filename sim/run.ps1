<#
.SYNOPSIS
    Build and run the headless LVGL simulator from Windows, via WSL.

.DESCRIPTION
    There is no gcc/make on the Windows side, so the actual compile happens
    inside WSL Ubuntu against /mnt/c/... paths. This script is just a wrapper:
    it translates its own directory to a WSL path, runs make, and then either
    regenerates every screenshot (the default) or runs one ad-hoc scenario.

.PARAMETER SimArgs
    Passed straight through to ./sim instead of running the `shots` target,
    e.g.  .\run.ps1 -SimArgs '--page home --theme light --out shots/x.png'
    (Not named -Args: that collides with PowerShell's automatic $Args.)

.PARAMETER Distro
    WSL distribution name. Defaults to Ubuntu.

.PARAMETER Jobs
    Parallel make jobs. Defaults to 8.

.PARAMETER Clean
    Wipe build/ before compiling.

.EXAMPLE
    .\run.ps1
    .\run.ps1 -Clean
    .\run.ps1 -SimArgs '--page usage --theme dark --state limit --out shots/limit.png'
#>

[CmdletBinding()]
param(
    [string]$SimArgs = '',
    [string]$Distro  = 'Ubuntu',
    [int]   $Jobs    = 8,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$simDir = $PSScriptRoot

# C:\foo\bar  ->  /mnt/c/foo/bar
$wslDir = $simDir -replace '\\', '/'
if ($wslDir -match '^([A-Za-z]):(.*)$') {
    $wslDir = '/mnt/' + $Matches[1].ToLower() + $Matches[2]
}
else {
    throw "Could not translate '$simDir' to a WSL path."
}

# The Arduino libraries (lvgl/ + lv_conf.h) sit on the Windows side, and WSL's
# $HOME is the Linux home, so the Makefile cannot guess this - pass it in.
$libs = Join-Path $env:USERPROFILE 'Documents\Arduino\libraries'
if (-not (Test-Path (Join-Path $libs 'lvgl\lvgl.h'))) {
    throw "LVGL not found under '$libs'. Install it in the Arduino IDE, or edit this path."
}
$wslLibs = $libs -replace '\\', '/'
if ($wslLibs -match '^([A-Za-z]):(.*)$') { $wslLibs = '/mnt/' + $Matches[1].ToLower() + $Matches[2] }

$steps = @()
if ($Clean) { $steps += 'make clean' }
$steps += "make -j$Jobs ARDUINO_LIBS='$wslLibs'"
if ($SimArgs) { $steps += "./sim $SimArgs" } else { $steps += 'make shots' }

$script = "set -e; cd '$wslDir'; " + ($steps -join '; ')

Write-Host "WSL($Distro): $script" -ForegroundColor DarkGray
& wsl.exe -d $Distro -- bash -lc $script
$code = $LASTEXITCODE

if ($code -ne 0) {
    Write-Host "sim: FAILED (exit $code)" -ForegroundColor Red
    exit $code
}

$shots = Join-Path $simDir 'shots'
if (Test-Path $shots) {
    Write-Host ''
    Write-Host "Screenshots in $shots" -ForegroundColor Green
    Get-ChildItem $shots -Filter *.png |
        Sort-Object Name |
        Format-Table Name, @{ N = 'KB'; E = { [math]::Round($_.Length / 1KB) } }, LastWriteTime -AutoSize
}
