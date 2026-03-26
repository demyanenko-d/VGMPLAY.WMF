@echo off
chcp 65001 >nul
REM ===========================================================================
REM build.bat — Сборка VGM Player Plugin (SDCC/Z80)
REM
REM Инструменты:
REM   sdcc      — компилятор C (sdcccall(1), mz80, без frame pointer)
REM   sdas      — ассемблер SDCC (ASM файлы)
REM   sdld / sdcc --link-cmd-file — линковщик
REM   node      — JS-скрипты (gen_pos_table, ihx2wmf)
REM
REM Структура вывода: build/ (временные .rel) → vgmplay.wmf
REM ===========================================================================

setlocal

cd /d "%~dp0"

REM --- Папка для временных файлов ---
if not exist build mkdir build

REM --- Флаги SDCC ---
REM   -mz80         : целевая архитектура Z80
REM   --sdcccall 1  : соглашение вызова sdcccall(1) (регистровая передача)
REM   --no-std-crt0 : не линковать стандартный crt0 (наш в asm/crt0.s)
REM   --opt-code-size : оптимизация по размеру (важно для 8-бит)
REM   --no-peep-asm : не трогать __naked asm-блоки
set CFLAGS=-mz80 --sdcccall 1 --no-std-crt0 --opt-code-size -I inc --disable-warning 85

REM --- Флаги ассемблера (sdas) ---
set ASFLAGS=-plosff

echo [1/8] Генерация pos_table.s...
node scripts\gen_pos_table.js --entries 28 --step 2560
if errorlevel 1 ( echo FAIL gen_pos_table && goto :err )

echo [2/8] Генерация freq_tables.bin...
node scripts\gen_freq_tables.js --page-base 1 --binary build\freq_tables.bin --cheader inc\freq_lut_map.h
if errorlevel 1 ( echo FAIL gen_freq_tables && goto :err )

echo [3/8] Сборка inflate.asm (sjasmplus)...
..\tools\sjasm\sjasmplus.exe asm\inflate.asm >nul 2>&1
if errorlevel 1 ( echo FAIL inflate.asm && goto :err )
REM inflate.asm имеет SAVEBIN "build/inflate.bin" внутри

echo [4/8] Компиляция C файлов...
sdcc %CFLAGS% -c main.c         -o build\main.rel    >nul
if errorlevel 1 ( echo FAIL main.c && goto :err )

sdcc %CFLAGS% -c lib\vgm.c      -o build\vgm.rel     >nul
if errorlevel 1 ( echo FAIL vgm.c && goto :err )

sdcc %CFLAGS% -c lib\keys.c     -o build\keys.rel    >nul
if errorlevel 1 ( echo FAIL keys.c && goto :err )

REM txtlib is now hand-written assembly (asm\txtlib.s), assembled below

echo [5/8] Сборка ASM файлов...
sdasz80 %ASFLAGS% build\crt0.rel    asm\crt0.s      >nul
if errorlevel 1 ( echo FAIL crt0.s && goto :err )

sdasz80 %ASFLAGS% build\isr.rel     asm\isr.s       >nul
if errorlevel 1 ( echo FAIL isr.s && goto :err )

sdasz80 %ASFLAGS% build\pos_table.rel asm\pos_table.s >nul
if errorlevel 1 ( echo FAIL pos_table.s && goto :err )

call lib\wc_api\build_lib.bat >nul
if errorlevel 1 ( echo FAIL wc_api.lib && goto :err )

sdasz80 %ASFLAGS% build\opl3.rel    asm\opl3.s      >nul
if errorlevel 1 ( echo FAIL opl3.s && goto :err )

sdasz80 %ASFLAGS% build\txtlib.rel  asm\txtlib.s    >nul
if errorlevel 1 ( echo FAIL txtlib.s && goto :err )

sdasz80 %ASFLAGS% build\inflate_call.rel asm\inflate_call.s >nul
if errorlevel 1 ( echo FAIL inflate_call.s && goto :err )

echo [6/8] Линковка...
REM Порядок важен: crt0 первым (entry point), затем модули
REM Layout: CODE #8000–#B83F, DATA #B8A0–#BFFF
REM   Все секции (DATA + GSINIT + HOME + INITIALIZER) должны
REM   уместиться до #BFFF (Win2).  Если CODE вырастет →
REM   понизить --data-loc или оптимизировать код.
sdcc -mz80 --no-std-crt0 --out-fmt-ihx --code-loc 0x8000 --data-loc 0xB820 ^
    build\crt0.rel ^
    build\main.rel ^
    build\vgm.rel  ^
    build\keys.rel ^
    build\isr.rel  ^
    build\pos_table.rel ^
    build\wc_api.lib ^
    build\opl3.rel ^
    build\txtlib.rel ^
    build\inflate_call.rel ^
    -o build\vgmplay.ihx >nul
if errorlevel 1 ( echo FAIL link && goto :err )

REM --- Вывод карты памяти секций из map-файла ---
powershell -NoProfile -Command ^
  "$m=Get-Content build\vgmplay.map -Raw;" ^
  "function gs($p){if($m -match '([0-9a-f]{8})\s+'+$p){return $Matches[1]}return '00000000'};" ^
  "$secs=@('CODE','DATA','GSINIT','GSFINAL','INITIALIZED','HOME','INITIALIZER');" ^
  "Write-Host '';" ^
  "Write-Host '  Section        Start    End      Size';" ^
  "Write-Host '  -------------- -------- -------- ----------';" ^
  "$lastEnd=0;" ^
  "foreach($s in $secs){" ^
  "  $st=[Convert]::ToInt32((gs ('s__'+$s)),16);" ^
  "  $ln=[Convert]::ToInt32((gs ('l__'+$s)),16);" ^
  "  if($ln -eq 0){continue};" ^
  "  $en=$st+$ln-1;" ^
  "  if($en -gt $lastEnd){$lastEnd=$en};" ^
  "  Write-Host ('  _'+$s.PadRight(14)+' 0x'+$st.ToString('X4')+'   0x'+$en.ToString('X4')+'   '+$ln.ToString().PadLeft(5)+' bytes')" ^
  "};" ^
  "$free=0xBFFF-$lastEnd;" ^
  "Write-Host '  -------------- -------- -------- ----------';" ^
  "Write-Host ('  Last byte: 0x'+$lastEnd.ToString('X4')+'   Free before C000: '+$free+' bytes');" ^
  "if($lastEnd -ge 0xC000){Write-Host '  *** ERROR: sections overflow past 0xBFFF! ***' -ForegroundColor Red; exit 1}"
if errorlevel 1 goto :err

echo [7/8] Генерация WMF (multi-page)...
REM Склеить freq_tables.bin + inflate.bin в один extra blob
copy /b build\freq_tables.bin+build\inflate.bin build\extra_combined.bin >nul
if errorlevel 1 ( echo FAIL combine extra && goto :err )
node scripts\ihx2wmf.js build\vgmplay.ihx ..\DiskRef\WC\VGMPLAY.WMF --extra build\extra_combined.bin
if errorlevel 1 ( echo FAIL ihx2wmf && goto :err )
copy /Y ..\DiskRef\WC\VGMPLAY.WMF ..\VGMPLAY.WMF >nul

echo [8/8] Сборка образа диска...
if not exist "..\tools\robimg.exe" (
    echo   SKIP: tools\robimg.exe не найден
    goto :ok
)
del /Q ..\tsconf.img 2>nul
..\tools\robimg.exe -p="..\tsconf.img" -s=280000 -C="..\DiskRef" >nul 2>&1
if errorlevel 1 ( echo FAIL robimg && goto :err )
echo   tsconf.img готов

:ok
echo.
echo === BUILD OK ===
goto :end

:err
echo.
echo === BUILD FAILED ===
exit /b 1

:end
endlocal
