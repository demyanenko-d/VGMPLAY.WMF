@echo off
REM Create disk image with VGM Player plugin for TSConfig/WC
REM Image size: 512MB = 524288 KB

echo Creating TSConfig disk image with VGM Player...
echo.

REM Check if plugin was built
if not exist "vgmplay.wmf" (
    echo Error: vgmplay.wmf not found!
    echo Please run build.bat first.
    exit /b 1
)

REM Step 1: Copy plugin to DiskRef\WC directory
echo Step 1: Copying plugin to DiskRef\WC...
copy /Y vgmplay.wmf DiskRef\WC\VGMPLAY.WMF
if errorlevel 1 (
    echo Error: Failed to copy plugin
    exit /b 1
)
echo   Done: vgmplay.wmf copied to DiskRef\WC\

REM Step 2: Create disk image
echo.
echo Step 2: Creating 512MB disk image...
echo   Using robimg.exe to create tsconf.img
del tsconf.img 2>nul

tools\robimg.exe -p="tsconf.img" -s=524288 -C="DiskRef"

if errorlevel 1 (
    echo Error: Failed to create disk image
    exit /b 1
)

echo.
echo ========================================
echo Disk image created successfully!
echo ========================================
echo.
echo Output file: tsconf.img (512MB)
echo Contains: DiskRef contents + VGM Player plugin
echo.
echo To use:
echo 1. Mount tsconf.img in your TSConfig emulator
echo 2. Boot Wild Commander
echo 3. Navigate to WC directory
echo 4. Find VGMPLAY.WMF and press Enter to load
echo 5. Select a .VGM file to play
echo.

