<#
    Build and flash the Claude usage widget.

    Put the board in download mode first: hold BOOT, tap RESET, release BOOT.

        .\flash.ps1              # auto-detect the port, compile, upload, then watch serial
        .\flash.ps1 -Port COM7   # pin the port
        .\flash.ps1 -NoMonitor   # upload and exit
        .\flash.ps1 -CompileOnly # just check that it builds
#>
[CmdletBinding()]
param(
    [string]$Port,
    [switch]$NoMonitor,
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'

$cli = 'C:\Program Files\Arduino CLI\arduino-cli.exe'
$sketch = Join-Path $PSScriptRoot 'firmware\claude_usage_widget'
$fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'

if (-not (Test-Path $cli)) { throw "arduino-cli not found at $cli" }

function Find-BoardPort {
    # An ESP32-S3 in ROM download mode shows up under Espressif's VID 303A; a board
    # running CDC firmware does too. USB-serial bridges (CP210x, CH34x, FTDI) are
    # accepted as a fallback for boards wired that way.
    $devices = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'COM(\d+)' -and $_.PNPDeviceID -match 'VID_303A|VID_10C4|VID_1A86|VID_0403' }
    foreach ($d in $devices) {
        if ($d.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    }
    return $null
}

if (-not $CompileOnly -and -not $Port) {
    $Port = Find-BoardPort
    if (-not $Port) {
        Write-Host ''
        Write-Warning 'No ESP32 board found on USB.'
        Write-Host '  1. Hold BOOT, tap RESET, release BOOT — reset alone is not enough.'
        Write-Host '  2. If still nothing, try a different USB-C cable. Charge-only cables'
        Write-Host '     are the most common cause; they power the board but carry no data.'
        Write-Host ''
        Write-Host 'Ports currently present:'
        Get-CimInstance Win32_PnPEntity |
            Where-Object { $_.Name -match 'COM\d+' } |
            ForEach-Object { "  $($_.Name)" }
        throw 'No board detected.'
    }
    Write-Host "Board on $Port" -ForegroundColor Green
}

Write-Host 'Compiling...' -ForegroundColor Cyan
& $cli compile --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }

if ($CompileOnly) {
    Write-Host 'Compile OK.' -ForegroundColor Green
    return
}

Write-Host "Uploading to $Port..." -ForegroundColor Cyan
& $cli upload --fqbn $fqbn -p $Port $sketch
if ($LASTEXITCODE -ne 0) { throw 'Upload failed.' }

Write-Host 'Flashed.' -ForegroundColor Green

if (-not $NoMonitor) {
    # After a native-USB upload the board re-enumerates, sometimes on a new port.
    Start-Sleep -Seconds 3
    $monitorPort = Find-BoardPort
    if (-not $monitorPort) { $monitorPort = $Port }
    Write-Host "Serial monitor on $monitorPort (Ctrl+C to exit)" -ForegroundColor Cyan
    & $cli monitor -p $monitorPort --config baudrate=115200
}
