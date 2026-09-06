<#
    Fast build/flash for either firmware.

        .\build.ps1                 # build + flash the LVGL firmware
        .\build.ps1 -Gfx            # build + flash the Arduino_GFX firmware
        .\build.ps1 -NoUpload       # compile only
        .\build.ps1 -Clean          # discard the cache and rebuild from scratch

    Two things make this much faster than a bare `arduino-cli compile`:

      -j 0            compile with all CPU cores instead of the default
      --build-path    a fixed, per-sketch object directory, so LVGL and
                      ESP32_Display_Panel are not recompiled every run

    The cache is keyed per sketch. It is invalidated automatically when a source
    file changes, but NOT reliably when a header outside the sketch changes -
    editing libraries/lv_conf.h, for instance. Use -Clean after doing that.
#>
[CmdletBinding()]
param(
    [switch]$Gfx,
    [switch]$NoUpload,
    [switch]$Clean,
    [string]$Port = "COM3"
)

$ErrorActionPreference = 'Stop'

$cli = 'C:\Program Files\Arduino CLI\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'

$name = if ($Gfx) { 'claude_usage_widget' } else { 'claude_widget_lvgl' }
$sketch = Join-Path $PSScriptRoot "firmware\$name"
$buildPath = Join-Path $env:LOCALAPPDATA "claude-widget-build\$name"

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

# The Arduino build only compiles what is inside the sketch directory, so the
# canonical UI sources are copied in here. ui/ and assets/ are the originals; the
# copies are gitignored and overwritten every build. Edit the originals, or the
# next build silently reverts your change.
if (-not $Gfx) {
    $sync = @(
        @{ From = Join-Path $PSScriptRoot 'ui\ui.c';             To = 'ui.c' }
        @{ From = Join-Path $PSScriptRoot 'ui\ui.h';             To = 'ui.h' }
        @{ From = Join-Path $PSScriptRoot 'assets\claude_logo.c'; To = 'claude_logo.c' }
        @{ From = Join-Path $PSScriptRoot 'assets\claude_logo.h'; To = 'claude_logo.h' }
    )
    foreach ($f in $sync) {
        if (-not (Test-Path $f.From)) { throw "missing source: $($f.From)" }
        Copy-Item $f.From (Join-Path $sketch $f.To) -Force
    }
    Write-Host "Synced ui/ and assets/ into the sketch" -ForegroundColor DarkGray
}

$args = @('compile', '--fqbn', $fqbn, '-j', '0', '--build-path', $buildPath,
          '--build-property', 'compiler.c.extra_flags=-DUI_HAVE_LOGO_IMG',
          '--build-property', 'compiler.cpp.extra_flags=-DUI_HAVE_LOGO_IMG')
if ($Clean) { $args += '--clean' }
$args += $sketch

Write-Host "Compiling $name (cache: $buildPath)" -ForegroundColor Cyan
$sw = [Diagnostics.Stopwatch]::StartNew()
& $cli @args
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }
$sw.Stop()
Write-Host ("Compiled in {0:n1}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green

if ($NoUpload) { return }

Write-Host "Uploading to $Port" -ForegroundColor Cyan
& $cli upload --fqbn $fqbn -p $Port --input-dir $buildPath $sketch
if ($LASTEXITCODE -ne 0) { throw 'Upload failed.' }
Write-Host 'Flashed.' -ForegroundColor Green
