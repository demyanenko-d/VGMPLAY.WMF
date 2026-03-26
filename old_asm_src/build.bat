@echo off
REM Build script for VGM Player plugin
REM Requires sjasm assembler in tools/sjasm directory

echo Building VGM Player plugin...

REM Check if sjasm exists
if not exist "tools\sjasm\sjasmplus.exe" (
    echo Error: sjasmplus.exe not found in tools\sjasm\
    echo Please install sjasm assembler
    exit /b 1
)

REM Compile the plugin
"tools\sjasm\sjasmplus.exe" src\vgmplayer.asm

if errorlevel 1 (
    echo.
    echo Compilation failed!
    exit /b 1
)

echo.
echo Build successful!
echo Output: vgmplay.wmf
echo.

REM Create disk image
echo Creating disk image...
call create_disk.bat

exit /b 0
