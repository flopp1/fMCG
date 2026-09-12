@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM ===========================================================================
REM fMCG release packager (Windows)
REM
REM Builds fMCG_gui.exe (if needed) and assembles a ready-to-zip release
REM folder in dist\fMCG-windows\ containing:
REM
REM   fMCG_gui.exe              the application
REM   libarchive.dll            required at runtime (BSD -- see licences file)
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
copy /y fMCG_gui.exe             "%DIST%\" >nul
copy /y libarchive.dll           "%DIST%\" >nul
copy /y LICENSE                  "%DIST%\" >nul
copy /y THIRD_PARTY_LICENSES.txt "%DIST%\" >nul
copy /y README.md                "%DIST%\" >nul

REM --- 3. sanity: everything the exe needs is present -------------------------
for %%f in (fMCG_gui.exe libarchive.dll LICENSE THIRD_PARTY_LICENSES.txt README.md) do (
    if not exist "%DIST%\%%f" (
        echo ERROR: %DIST%\%%f missing; packaging incomplete.
        exit /b 1
    )
)

REM --- 4. optional zip --------------------------------------------------------
set "ZIP=%DIST%.zip"
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST:\=\\%\*' -DestinationPath '%ZIP%'"
if exist "%ZIP%" (
    echo.
    echo Release ready:
    echo   folder: %DIST%
    echo   zip:    %ZIP%
) else (
    echo.
    echo Release folder ready: %DIST% ^(zip failed; compress it manually^).
)
exit /b 0
