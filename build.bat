@echo off
setlocal enabledelayedexpansion

set KARCH=x86_64
set IMAGE_NAME=eucalypt-%KARCH%
set ISO_ROOT=iso_root
set LIMINE_DIR=limine

if "%1"=="" goto usage
if "%1"=="build" goto build
if "%1"=="kernel" goto build_kernel
if "%1"=="iso" goto build_iso
if "%1"=="clean" goto clean
goto usage

:build_kernel
echo Building eucalyptOS kernel...
cd kernel
make
cd ..
if "%1"=="kernel" goto end
goto :eof

:build_iso
echo Building ISO image...

if not exist "%LIMINE_DIR%" (
    echo Cloning Limine bootloader...
    git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
)

if exist "%ISO_ROOT%" rmdir /s /q "%ISO_ROOT%"
mkdir "%ISO_ROOT%"

if not exist "kernel\kernel" (
    echo ERROR: Kernel binary not found at kernel\kernel
    exit /b 1
)

echo Copying kernel...
copy /y kernel\kernel "%ISO_ROOT%\"

mkdir "%ISO_ROOT%\boot"
mkdir "%ISO_ROOT%\EFI\BOOT"

echo Copying Limine boot files...
if exist "%LIMINE_DIR%\limine-bios.sys" (
    copy /y "%LIMINE_DIR%\limine-bios.sys" "%ISO_ROOT%\boot\"
)

if not exist "%LIMINE_DIR%\limine-bios-cd.bin" (
    echo ERROR: limine-bios-cd.bin not found!
    exit /b 1
)
copy /y "%LIMINE_DIR%\limine-bios-cd.bin" "%ISO_ROOT%\boot\"

if exist "%LIMINE_DIR%\limine-uefi-cd.bin" (
    copy /y "%LIMINE_DIR%\limine-uefi-cd.bin" "%ISO_ROOT%\boot\"
)

if exist "%LIMINE_DIR%\BOOTX64.EFI" (
    copy /y "%LIMINE_DIR%\BOOTX64.EFI" "%ISO_ROOT%\EFI\BOOT\"
)

echo Creating limine.conf...
(
echo timeout: 0
echo.
echo /eucalyptOS
echo     protocol: limine
echo     kernel_path: boot^(^):/kernel
) > "%ISO_ROOT%\boot\limine.conf"

echo Creating ISO with xorriso...
where xorriso >nul 2>&1
if errorlevel 1 (
    echo ERROR: xorriso not found!
    echo Install it or use WSL for ISO creation
    exit /b 1
)

xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label "%ISO_ROOT%" -o "%IMAGE_NAME%.iso"

if exist "%LIMINE_DIR%\limine.exe" (
    echo Installing Limine bootloader to ISO...
    "%LIMINE_DIR%\limine.exe" bios-install "%IMAGE_NAME%.iso"
) else if exist "%LIMINE_DIR%\limine-deploy.exe" (
    "%LIMINE_DIR%\limine-deploy.exe" "%IMAGE_NAME%.iso"
)

if not exist "%IMAGE_NAME%.iso" (
    echo ERROR: ISO file was not created!
    exit /b 1
)

echo.
echo *** ISO created successfully: %IMAGE_NAME%.iso ***
echo.
echo To run in VirtualBox:
echo   1. Create a new VM with type "Other" and version "Other/Unknown (64-bit)"
echo   2. Attach %IMAGE_NAME%.iso as a CD/DVD
echo   3. Start the VM
echo.
goto end

:build
call :build_kernel
call :build_iso
goto end

:clean
echo Cleaning build artifacts...
cd kernel
make clean
cd ..
if exist "%ISO_ROOT%" rmdir /s /q "%ISO_ROOT%"
if exist "%IMAGE_NAME%.iso" del /q "%IMAGE_NAME%.iso"
echo Clean complete
goto end

:usage
echo Usage: %0 {build^|kernel^|iso^|clean}
echo   build   - Build kernel and ISO
echo   kernel  - Build kernel only
echo   iso     - Build ISO only (requires kernel)
echo   clean   - Clean build artifacts
exit /b 1

:end
endlocal