<#
.SYNOPSIS
  Build multiple ISR frequency / command budget variants for A/B testing.
  Each variant produces a separate tsconf_*.img disk image.

.DESCRIPTION
  Modifies isr.h, isr.s, vgm.c, build.bat with variant parameters,
  runs build.bat, then copies the output image.  Restores originals afterward.

.NOTES
  C compilation is SLOW (~60 sec).  Do NOT interrupt (Ctrl-C).
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root   = Split-Path -Parent $MyInvocation.MyCommand.Path   # src_sdcc
$projRoot = Split-Path -Parent $root                         # OplPlug

# ── Variant definitions ──────────────────────────────────────────────
# Each variant:  Name, Budget, Entries, Step, ISR_FREQ, Shift, Mask, TicksPerFrame, DataLoc
$variants = @(
  # --- User-requested 6 ---
  @{ Name='v1_b8';          Budget=8;  Entries=56;  Step=1280; Freq=2734; Shift=4; Mask='0x0Fu'; TPF=56;  DataLoc='0xB820' }
  @{ Name='v2_b32';         Budget=32; Entries=56;  Step=1280; Freq=2734; Shift=4; Mask='0x0Fu'; TPF=56;  DataLoc='0xB820' }
  @{ Name='v3_b32_hf';      Budget=32; Entries=28;  Step=2560; Freq=1367; Shift=5; Mask='0x1Fu'; TPF=28;  DataLoc='0xB820' }
  @{ Name='v4_b16_hf';      Budget=16; Entries=28;  Step=2560; Freq=1367; Shift=5; Mask='0x1Fu'; TPF=28;  DataLoc='0xB820' }
  @{ Name='v5_b8_df';       Budget=8;  Entries=112; Step=640;  Freq=5468; Shift=3; Mask='0x07u'; TPF=112; DataLoc='0xB940' }
  @{ Name='v6_b16_df';      Budget=16; Entries=112; Step=640;  Freq=5468; Shift=3; Mask='0x07u'; TPF=112; DataLoc='0xB940' }
  # --- Bonus variants ---
  @{ Name='v7_b24';         Budget=24; Entries=56;  Step=1280; Freq=2734; Shift=4; Mask='0x0Fu'; TPF=56;  DataLoc='0xB820' }
  @{ Name='v8_b64';         Budget=64; Entries=56;  Step=1280; Freq=2734; Shift=4; Mask='0x0Fu'; TPF=56;  DataLoc='0xB820' }
  @{ Name='v9_b32_df';      Budget=32; Entries=112; Step=640;  Freq=5468; Shift=3; Mask='0x07u'; TPF=112; DataLoc='0xB940' }
)

# ── Save originals ──────────────────────────────────────────────────
$files = @{
  'isr.h'    = "$root\inc\isr.h"
  'isr.s'    = "$root\asm\isr.s"
  'vgm.c'    = "$root\lib\vgm.c"
  'build.bat'= "$root\build.bat"
}
$backups = @{}
foreach ($k in $files.Keys) {
  $backups[$k] = Get-Content $files[$k] -Raw
}

function Restore-All {
  foreach ($k in $files.Keys) {
    Set-Content $files[$k] -Value $backups[$k] -NoNewline
  }
}

# ── Output directory ─────────────────────────────────────────────────
$outDir = "$projRoot\variants"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# ── Build loop ──────────────────────────────────────────────────────
$total = $variants.Count
$idx = 0

foreach ($v in $variants) {
  $idx++
  $name = $v.Name
  Write-Host "`n========== [$idx/$total] Building $name ==========" -ForegroundColor Cyan
  Write-Host "  Budget=$($v.Budget)  Freq=$($v.Freq)Hz  Entries=$($v.Entries)  Shift=$($v.Shift)  TPF=$($v.TPF)  DataLoc=$($v.DataLoc)"

  # --- Restore clean state ---
  Restore-All

  # --- Patch isr.h ---
  $h = $backups['isr.h']
  $h = $h -replace '(?m)^#define ISR_FREQ\s+\d+',       "#define ISR_FREQ              $($v.Freq)"
  $h = $h -replace '(?m)^#define ISR_TICKS_PER_FRAME\s+\d+', "#define ISR_TICKS_PER_FRAME   $($v.TPF)"
  $h = $h -replace '(?m)^#define VGM_SAMPLE_SHIFT\s+\d+',    "#define VGM_SAMPLE_SHIFT      $($v.Shift)"
  $h = $h -replace '(?m)^#define VGM_SAMPLE_MASK\s+0x[0-9A-Fa-f]+u', "#define VGM_SAMPLE_MASK       $($v.Mask)"
  Set-Content $files['isr.h'] -Value $h -NoNewline

  # --- Patch isr.s (ISR_FREQ = value) ---
  $s = $backups['isr.s']
  $s = $s -replace '(?m)^ISR_FREQ = \d+', "ISR_FREQ = $($v.Freq)"
  Set-Content $files['isr.s'] -Value $s -NoNewline

  # --- Patch vgm.c (VGM_FILL_CMD_BUDGET) ---
  $vc = $backups['vgm.c']
  $vc = $vc -replace '(?m)^#define VGM_FILL_CMD_BUDGET\s+\d+u', "#define VGM_FILL_CMD_BUDGET $($v.Budget)u"
  Set-Content $files['vgm.c'] -Value $vc -NoNewline

  # --- Patch build.bat (--data-loc and gen_pos_table.js args) ---
  $bat = $backups['build.bat']
  $bat = $bat -replace '--data-loc\s+0x[0-9A-Fa-f]+', "--data-loc $($v.DataLoc)"
  $bat = $bat -replace 'node scripts\\gen_pos_table\.js\b', "node scripts\gen_pos_table.js --entries $($v.Entries) --step $($v.Step)"
  Set-Content $files['build.bat'] -Value $bat -NoNewline

  # --- Clean .rel files to force recompile ---
  Remove-Item "$root\build\*.rel" -ErrorAction SilentlyContinue

  # --- Run build ---
  Write-Host "  Building... (this takes ~60 sec)" -ForegroundColor Yellow
  $buildOut = cmd /c "cd /d $root && build.bat 2>&1"
  $buildOk = $buildOut | Where-Object { $_ -match 'BUILD OK' }
  $buildFail = $buildOut | Where-Object { $_ -match 'FAIL|ERROR|overflow' }

  if ($buildFail -or -not $buildOk) {
    Write-Host "  *** BUILD FAILED for $name ***" -ForegroundColor Red
    $buildOut | Where-Object { $_ -match 'FAIL|error|warning|_CODE|_DATA|Last byte|overflow' } | ForEach-Object { Write-Host "    $_" }
    continue
  }

  # --- Show map ---
  $buildOut | Where-Object { $_ -match 'Section|_CODE|_DATA|_GSINIT|_HOME|_INITIAL|Last byte|Code:|Written.*WMF' } | ForEach-Object { Write-Host "  $_" }

  # --- Copy output image ---
  $srcImg = "$projRoot\tsconf.img"
  $dstImg = "$outDir\tsconf_$name.img"
  if (Test-Path $srcImg) {
    Copy-Item $srcImg $dstImg -Force
    Write-Host "  -> $dstImg" -ForegroundColor Green
  } else {
    Write-Host "  WARNING: tsconf.img not found, copying WMF only" -ForegroundColor Yellow
  }
  # Also copy WMF
  $srcWmf = "$projRoot\DiskRef\WC\VGMPLAY.WMF"
  $dstWmf = "$outDir\VGMPLAY_$name.WMF"
  if (Test-Path $srcWmf) {
    Copy-Item $srcWmf $dstWmf -Force
  }
}

# ── Restore originals ───────────────────────────────────────────────
Write-Host "`n========== Restoring original sources ==========" -ForegroundColor Cyan
Restore-All
# Rebuild with originals to leave workspace clean
Remove-Item "$root\build\*.rel" -ErrorAction SilentlyContinue
Write-Host "  Rebuilding baseline..." -ForegroundColor Yellow
$null = cmd /c "cd /d $root && build.bat 2>&1"
Write-Host "  Done." -ForegroundColor Green

# ── Summary ──────────────────────────────────────────────────────────
Write-Host "`n========== VARIANT SUMMARY ==========" -ForegroundColor Cyan
Write-Host ("{0,-18} {1,7} {2,6} {3,8} {4,6}" -f 'Name','Budget','Freq','Entries','Shift')
Write-Host ("{0,-18} {1,7} {2,6} {3,8} {4,6}" -f '----','------','----','-------','-----')
foreach ($v in $variants) {
  Write-Host ("{0,-18} {1,7} {2,6} {3,8} {4,6}" -f $v.Name, $v.Budget, $v.Freq, $v.Entries, $v.Shift)
}
Write-Host "`nImages in: $outDir"
Write-Host ""

# Legend
Write-Host "Legend:" -ForegroundColor DarkGray
Write-Host "  hf = half freq (1367 Hz, /2)" -ForegroundColor DarkGray
Write-Host "  df = double freq (5468 Hz, x2)" -ForegroundColor DarkGray
Write-Host "  bN = budget N commands per ISR tick" -ForegroundColor DarkGray
