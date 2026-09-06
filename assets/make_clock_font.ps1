<#
    Regenerate ui/font_clock_112.c - the big digits on the clock page.

    Montserrat Medium at 112 px, 4-bit anti-aliasing, glyphs "-./0123456789:" only,
    so the whole face is ~30 KB of flash. The TTF ships inside the LVGL library, so
    nothing is downloaded but the converter itself (lv_font_conv, via npx). Needs node.
    --no-compress because lv_conf.h has LV_USE_FONT_COMPRESSED 0.
#>
$ErrorActionPreference = 'Stop'
$ttf = Join-Path $env:USERPROFILE 'Documents\Arduino\libraries\lvgl\scripts\built_in_font\Montserrat-Medium.ttf'
if (-not (Test-Path $ttf)) { throw "Montserrat TTF not found at $ttf" }
$out = Join-Path $PSScriptRoot '..\ui\font_clock_112.c'
npx --yes lv_font_conv --font $ttf --size 112 --bpp 4 --format lvgl --no-compress `
    --lv-include lvgl.h --lv-font-name font_clock_112 -r 0x2D-0x3A -o $out
Write-Host "wrote $out"
