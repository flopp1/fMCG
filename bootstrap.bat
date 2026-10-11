@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM ===========================================================================
REM fMCG dependency bootstrap (Windows)
REM
REM Run this once on a fresh clone. It downloads pinned copies of the
REM third-party dependencies into vendor\ so build.bat can compile offline
REM afterwards:
REM
REM   Dear ImGui v1.92.9b        git submodule (vendor/imgui) -- fetched below
REM   GLFW 3.4 (mingw-w64 bin)   https://github.com/glfw/glfw
REM   libarchive 3.8.9 (mingw)   https://github.com/li-ruijie/libarchive
REM   HarfBuzz 14.5.0 (source)   https://github.com/harfbuzz/harfbuzz (text shaping)
REM   x264 stable b35605ac       https://code.videolan.org/videolan/x264 (built here)
REM   NASM 3.02 (win64)          https://www.nasm.us (assembler for x264)
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

REM --- 5. in-built renderer: HarfBuzz (shaping), NASM + x264 (H.264 encoder) ---
REM Pinned versions, verified by SHA-256. x264 is built here as a static
REM library with w64devkit's sh + make (NASM enables its assembly paths).
set "X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55"
call :get_src harfbuzz https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz harfbuzz-14.5.0.tar.xz b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9 vendor\harfbuzz\src\harfbuzz.cc
if !ERRORLEVEL! NEQ 0 exit /b 1
if not exist vendor\x264\lib\libx264.a (
    call :get_src nasm https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip nasm-3.02-win64.zip 161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7 %CACHE%\nasm\nasm-3.02\nasm.exe
    if !ERRORLEVEL! NEQ 0 exit /b 1
)
call :get_src x264 https://code.videolan.org/videolan/x264/-/archive/%X264_COMMIT%/x264-%X264_COMMIT%.tar.bz2 x264-%X264_COMMIT%.tar.bz2 6eeb82934e69fd51e043bd8c5b0d152839638d1ce7aa4eea65a3fedcf83ff224 vendor\x264\lib\libx264.a
if !ERRORLEVEL! NEQ 0 exit /b 1

echo.
echo Dependencies ready. Building...
echo.
call "%~dp0build.bat"
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

REM ===========================================================================
REM :get_src name url file sha256 marker-file
REM   Download (once) into build_cache, verify the SHA-256, then stage it.
REM ===========================================================================
:get_src
if exist "%~5" (
    echo [ok] %~1 already present.
    exit /b 0
)
if not exist "%CACHE%\%~3" (
    echo [..] downloading %~1 ...
    if "%DL%"=="curl" (
        curl.exe -L --fail -o "%CACHE%\%~3" "%~2"
    ) else (
        powershell -NoProfile -Command "[Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri '%~2' -OutFile '%CACHE%\%~3'"
    )
    if !ERRORLEVEL! NEQ 0 goto :src_fail
)
set "GOT="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%CACHE%\%~3" SHA256') do if not defined GOT set "GOT=%%h"
set "GOT=!GOT: =!"
if /i not "!GOT!"=="%~4" (
    echo ERROR: %~3 has SHA-256 !GOT!, expected %~4.
    del "%CACHE%\%~3" >nul 2>&1
    goto :src_fail
)
echo [..] preparing %~1 ...
call :stage_src_%~1
if !ERRORLEVEL! NEQ 0 goto :src_fail
echo [ok] %~1 ready.
exit /b 0

:src_fail
echo ERROR: failed to download or prepare %~1.
echo        Delete build_cache\ and retry, or fetch %~2 manually.
exit /b 1

:stage_src_harfbuzz
tar -xf "%CACHE%\harfbuzz-14.5.0.tar.xz" -C "%CACHE%" harfbuzz-14.5.0/src harfbuzz-14.5.0/COPYING
if errorlevel 1 exit /b 1
if not exist vendor\harfbuzz mkdir vendor\harfbuzz
robocopy "%CACHE%\harfbuzz-14.5.0\src" vendor\harfbuzz\src /E >nul
if errorlevel 8 exit /b 1
copy /y "%CACHE%\harfbuzz-14.5.0\COPYING" vendor\harfbuzz\ >nul
exit /b 0

:stage_src_nasm
powershell -NoProfile -Command "Expand-Archive -Force '%CACHE%\nasm-3.02-win64.zip' '%CACHE%\nasm'"
if errorlevel 1 exit /b 1
exit /b 0

:stage_src_x264
REM Build the static library (no CLI, no OpenCL, 8-bit 4:2:0). configure is
REM run through `sh` explicitly: its #!/bin/bash line has no busybox applet.
if exist "%CACHE%\x264-src" rmdir /s /q "%CACHE%\x264-src"
tar -xf "%CACHE%\x264-%X264_COMMIT%.tar.bz2" -C "%CACHE%"
if errorlevel 1 exit /b 1
ren "%CACHE%\x264-%X264_COMMIT%" x264-src
if errorlevel 1 exit /b 1
where sh >nul 2>&1
if errorlevel 1 (
    echo ERROR: sh not found ^(w64devkit provides it^); cannot build x264.
    exit /b 1
)
echo [..] building x264 ^(about 30 s^)...
set "PATH=%CD%\%CACHE%\nasm\nasm-3.02;%PATH%"
pushd "%CACHE%\x264-src"
sh -c "sh configure --host=x86_64-w64-mingw32 --enable-static --disable-cli --disable-opencl --bit-depth=8 --chroma-format=420 >configure.log 2>&1 && make -j%NUMBER_OF_PROCESSORS% lib-static >make.log 2>&1"
set "X264_RC=!ERRORLEVEL!"
popd
if not "!X264_RC!"=="0" (
    echo ERROR: x264 build failed; see %CACHE%\x264-src\configure.log and make.log.
    exit /b 1
)
if not exist vendor\x264\lib     mkdir vendor\x264\lib
if not exist vendor\x264\include mkdir vendor\x264\include
copy /y "%CACHE%\x264-src\libx264.a"      vendor\x264\lib\ >nul
copy /y "%CACHE%\x264-src\x264.h"         vendor\x264\include\ >nul
copy /y "%CACHE%\x264-src\x264_config.h"  vendor\x264\include\ >nul
copy /y "%CACHE%\x264-src\COPYING"        vendor\x264\ >nul
exit /b 0
