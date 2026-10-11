@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM ===========================================================================
REM fMCG release packager (Windows)
REM
REM Builds fMCG.exe (if needed) and assembles a ready-to-zip release
REM folder in dist\fMCG-windows\ containing:
REM
REM   fMCG.exe              the application -- self-contained: libarchive and
REM                             its codecs, HarfBuzz and x264 are linked statically
REM   LICENSE                   fMCG's own GPL-3.0 licence
REM   THIRD_PARTY_LICENSES.txt  required notices for the bundled libraries
REM   README.md                 quick-start / build-from-source instructions
REM
REM Usage:  release.bat            -> dist\fMCG-windows\
REM         release.bat MyName     -> dist\fMCG-MyName-windows\
REM
REM Zip it yourself, or let the script do it when PowerShell is available
REM (it always is on Windows 10+).
REM ===========================================================================

set "NAME=fMCG"
if not "%~1"=="" set "NAME=fMCG-%~1"
set "DIST=dist\%NAME%-windows"

REM --- 1. build ---------------------------------------------------------------
call "%~dp0build.bat"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: build failed; cannot package.
    exit /b 1
)

REM --- 2. assemble ------------------------------------------------------------
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" 2>nul
copy /y fMCG.exe             "%DIST%\" >nul
copy /y LICENSE                  "%DIST%\" >nul
copy /y THIRD_PARTY_LICENSES.txt "%DIST%\" >nul
copy /y README.md                "%DIST%\" >nul

REM --- 3. sanity: everything the exe needs is present -------------------------
for %%f in (fMCG.exe LICENSE THIRD_PARTY_LICENSES.txt README.md) do (
    if not exist "%DIST%\%%f" (
        echo ERROR: %DIST%\%%f missing; packaging incomplete.
        exit /b 1
    )
)
REM The exe must import Windows system DLLs only (nothing to ship next to it).
where objdump >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    set "FOREIGN="
    for /f "tokens=3" %%d in ('objdump -p "%DIST%\fMCG.exe" ^| findstr /C:"DLL Name"') do (
        echo bcrypt.dll gdi32.dll kernel32.dll msvcrt.dll opengl32.dll shell32.dll user32.dll advapi32.dll ole32.dll comdlg32.dll ws2_32.dll | findstr /I /C:"%%d" >nul || set "FOREIGN=!FOREIGN! %%d"
    )
    if defined FOREIGN (
        echo ERROR: fMCG.exe imports non-system DLLs:!FOREIGN!
        echo        The Windows release is meant to be a single self-contained exe.
        exit /b 1
    )
    echo [ok] fMCG.exe imports Windows system DLLs only.
)

REM --- 4. optional zip --------------------------------------------------------
set "ZIP=%DIST%.zip"
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST:\=\\%\*' -DestinationPath '%ZIP%'"
if exist "%ZIP%" (
    echo.
    echo Windows release ready:
    echo   folder: %DIST%
    echo   zip:    %ZIP%
) else (
    echo.
    echo Windows release folder ready: %DIST% ^(zip failed; compress it manually^).
)

REM --- 5. Linux build through WSL ----------------------------------------------
echo.
echo Building the Linux release through WSL...
where wsl >nul 2>&1
if %ERRORLEVEL% NEQ 0 goto :no_wsl
wsl -e true >nul 2>&1
if %ERRORLEVEL% NEQ 0 goto :no_wsl
set "WSLSRC="
for /f "usebackq delims=" %%p in (`wsl -e wslpath -a "%CD%"`) do set "WSLSRC=%%p"
if not defined WSLSRC goto :no_wsl
wsl -e bash "%WSLSRC%/release_linux.sh" "%WSLSRC%" "%NAME%"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: the Linux release failed ^(see above^). The Windows release is complete.
    exit /b 1
)
echo.
echo Linux release ready:
echo   folder: dist\%NAME%-linux
echo   tar:    dist\%NAME%-linux.tar.gz
exit /b 0

:no_wsl
echo ERROR: WSL with an installed Linux distribution was not found, so the Linux
echo        binary was not built. Install it with "wsl --install" ^(then the packages
echo        listed in README.md^) and re-run. The Windows release is complete.
exit /b 1
