@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM ===========================================================================
REM fMCG dependency bootstrap (Windows)
REM
REM Run this once on a fresh clone. It downloads pinned copies of the three
REM third-party dependencies into vendor\ so build.bat can compile offline
REM afterwards:
REM
REM   Dear ImGui v1.92.9b        git submodule (vendor/imgui) -- fetched below
REM   GLFW 3.4 (mingw-w64 bin)   https://github.com/glfw/glfw
REM   libarchive 3.8.9 (mingw)   https://github.com/li-ruijie/libarchive
REM
REM Downloads use curl.exe (bundled with Windows 10 1803+) with a PowerShell
REM fallback; extraction uses the built-in Expand-Archive. Nothing is installed
REM system-wide -- everything stays inside vendor\ and build_cache\.
REM ===========================================================================

echo === fMCG dependency bootstrap ===
echo.

REM --- 1. toolchain ----------------------------------------------------------
where g++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: g++ not found in PATH.
    echo        Install w64devkit ^(https://github.com/skeeto/w64devkit/releases^)
    echo        or any MinGW-w64 toolchain and put g++ on PATH.
    exit /b 1
)
echo [ok] g++ found.

REM --- 2. download helper ----------------------------------------------------
set "DL="
curl.exe --version >nul 2>&1 && set "DL=curl"
if not defined DL (
    powershell -NoProfile -Command "$true" >nul 2>&1 && set "DL=ps"
)
if not defined DL (
    echo ERROR: neither curl.exe nor PowerShell is available for downloads.
    exit /b 1
)
set "CACHE=build_cache"
if not exist "%CACHE%" mkdir "%CACHE%"

REM --- 3. fetch the imgui submodule -------------------------------------------
if exist vendor\imgui\imgui.h (
    echo [ok] imgui submodule already present.
) else (
    where git >nul 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo ERROR: git not found in PATH. Install Git for Windows
        echo        ^(https://git-scm.com/download/win^) and re-run.
        exit /b 1
    )
    echo [..] fetching imgui submodule...
    git submodule update --init --depth 1 vendor/imgui
    if !ERRORLEVEL! NEQ 0 (
        echo ERROR: git submodule update failed. Check your network connection.
        exit /b 1
    )
    echo [ok] imgui fetched into vendor\imgui.
)

REM --- 4. download the remaining dependencies (skipped when already present) ---
call :get_dep glfw       https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip glfw-3.4.bin.WIN64.zip vendor\GLFW\GLFW\glfw3.h
if !ERRORLEVEL! NEQ 0 exit /b 1
call :get_dep libarchive https://github.com/li-ruijie/libarchive/releases/download/v3.8.9/libarchive-v3.8.9-windows-mingw-x64-static.zip libarchive-3.8.9-mingw-x64.zip vendor\libarchive\lib\libarchive.dll.a
if !ERRORLEVEL! NEQ 0 exit /b 1

echo.
echo Dependencies ready. Building...
echo.
call build.bat
exit /b %ERRORLEVEL%

REM ===========================================================================
REM :get_dep name url zipname marker-file
REM ===========================================================================
:get_dep
if exist "%~4" (
    echo [ok] %~1 already present.
    exit /b 0
)
echo [..] downloading %~1 ...
if "%DL%"=="curl" (
    curl.exe -L --fail -o "%CACHE%\%~3" "%~2"
) else (
    powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri '%~2' -OutFile '%CACHE%\%~3'"
)
if !ERRORLEVEL! NEQ 0 goto :dl_fail
echo [..] extracting %~1 ...
powershell -NoProfile -Command "Expand-Archive -Force '%CACHE%\%~3' '%CACHE%\%~1'"
if !ERRORLEVEL! NEQ 0 goto :dl_fail
call :stage_%~1
if !ERRORLEVEL! NEQ 0 goto :dl_fail
echo [ok] %~1 installed to vendor\%~1
exit /b 0

:dl_fail
echo ERROR: failed to download or prepare %~1.
echo        Delete build_cache\ and retry, or fetch %~2 manually.
exit /b 1

REM --- per-dependency staging: layout the extracted files under vendor\ ------

:stage_glfw
if not exist vendor\GLFW\GLFW mkdir vendor\GLFW\GLFW
REM Official binaries: include\GLFW\*.h + lib-mingw-w64\libglfw3.a (static)
robocopy "%CACHE%\glfw\glfw-3.4.bin.WIN64\include\GLFW" vendor\GLFW\GLFW /E >nul
if errorlevel 8 exit /b 1
robocopy "%CACHE%\glfw\glfw-3.4.bin.WIN64\lib-mingw-w64" vendor\GLFW /E >nul
if errorlevel 8 exit /b 1
exit /b 0

:stage_libarchive
if not exist vendor\libarchive\lib  mkdir vendor\libarchive\lib
if not exist vendor\libarchive\bin  mkdir vendor\libarchive\bin
REM Self-contained DLL + its import library; the .a in this package does NOT
REM link statically (see vendor\README.txt), so we ship the DLL.
copy /y "%CACHE%\libarchive\include\archive.h"        vendor\libarchive\ >nul
copy /y "%CACHE%\libarchive\include\archive_entry.h"  vendor\libarchive\ >nul
copy /y "%CACHE%\libarchive\lib\libarchive.dll.a"     vendor\libarchive\lib\ >nul
copy /y "%CACHE%\libarchive\bin\libarchive.dll"       vendor\libarchive\bin\ >nul
exit /b 0
