@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM ===========================================================================
REM fMCG build script (Windows)
REM
REM Third-party dependencies live in vendor\, laid out by bootstrap.bat:
REM   vendor\imgui\           Dear ImGui git submodule (sources + backends\)
REM   vendor\GLFW\GLFW\       GLFW headers          + libglfw3.a (static)
REM   vendor\libarchive\      headers + libarchive.dll.a + bin\libarchive.dll
REM
REM Run bootstrap.bat first on a fresh clone; it fetches the imgui submodule
REM and downloads the rest.
REM Output: fMCG_gui.exe (needs libarchive.dll next to it at runtime).
REM
REM "build.bat debug" adds -DFMCG_DEBUG=1: ffmpeg's stderr log is kept in
REM <name>_fMCG_progress.txt next to the output for diagnosis. Release builds
REM (default) discard it -- end users never see progress artifacts.
REM ===========================================================================

set EXTRA_CFLAGS=
if /i "%1"=="debug" (
    set EXTRA_CFLAGS=-DFMCG_DEBUG=1
    echo [debug build] ffmpeg stderr log enabled.
)

if not exist vendor\imgui\imgui.h (
    echo ERROR: vendor\imgui not found. Run bootstrap.bat first ^(or:
    echo        git submodule update --init^).
    exit /b 1
)
if not exist vendor\GLFW\GLFW\glfw3.h (
    echo ERROR: vendor\GLFW not found. Run bootstrap.bat first.
    exit /b 1
)
if not exist vendor\libarchive\lib\libarchive.dll.a (
    echo ERROR: vendor\libarchive not found. Run bootstrap.bat first.
    exit /b 1
)

where g++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: g++ not found in PATH.
    echo Please install w64devkit or add g++ to your PATH.
    exit /b 1
)

if not exist lib mkdir lib

echo [1/2] Checking imgui static library...
set NEED_REBUILD=0
if not exist lib\libimgui.a set NEED_REBUILD=1
for %%f in (imgui imgui_draw imgui_widgets imgui_tables imgui_demo) do (
    if not exist "lib\%%f.o" set NEED_REBUILD=1
)
for %%f in (imgui_impl_glfw imgui_impl_opengl3) do (
    if not exist "lib\%%f.o" set NEED_REBUILD=1
)
REM any imgui source newer than any object -> rebuild
for %%s in (vendor\imgui\*.cpp vendor\imgui\backends\imgui_impl_*.cpp) do (
    for %%g in (lib\*.o) do (
        if "%%~ts" GTR "%%~tg" set NEED_REBUILD=1
    )
)
if "%NEED_REBUILD%"=="1" (
    echo   Building imgui...
    for %%f in (imgui imgui_draw imgui_widgets imgui_tables imgui_demo) do (
        g++ -std=c++17 -O3 -c vendor\imgui\%%f.cpp -Ivendor\imgui -Ivendor\GLFW -o lib\%%f.o
        if !ERRORLEVEL! NEQ 0 (
            echo imgui build FAILED ^(%%f^).
            exit /b 1
        )
    )
    for %%f in (imgui_impl_glfw imgui_impl_opengl3) do (
        g++ -std=c++17 -O3 -c vendor\imgui\backends\%%f.cpp -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\GLFW -o lib\%%f.o
        if !ERRORLEVEL! NEQ 0 (
            echo imgui build FAILED ^(%%f^).
            exit /b 1
        )
    )
    del lib\libimgui.a 2>nul
    ar rcs lib\libimgui.a lib\imgui.o lib\imgui_draw.o lib\imgui_widgets.o lib\imgui_tables.o lib\imgui_demo.o lib\imgui_impl_glfw.o lib\imgui_impl_opengl3.o
    echo   OK: lib\libimgui.a
) else (
    echo   lib\libimgui.a is up to date.
)

echo [2/2] Building fMCG_gui (GUI)...
for %%f in (fmcg_path fmcg_util fmcg_midi fmcg_engine fmcg_format fmcg_fonts fmcg_render) do call :compile_core %%f
for %%f in (app_state dialogs jobs preview main colour_edit settings_store) do call :compile_gui %%f
g++ -std=c++17 -O3 -o fMCG_gui.exe lib\gui\app_state.o lib\gui\dialogs.o lib\gui\jobs.o lib\gui\preview.o lib\gui\main.o lib\gui\colour_edit.o lib\gui\settings_store.o ^
    lib\src\fmcg_path.o lib\src\fmcg_util.o lib\src\fmcg_midi.o lib\src\fmcg_engine.o lib\src\fmcg_format.o lib\src\fmcg_fonts.o lib\src\fmcg_render.o ^
    -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\GLFW -I. -Isrc -Igui -Ivendor\libarchive ^
    -Lvendor\GLFW -Lvendor\libarchive\lib ^
    lib\libimgui.a vendor\libarchive\lib\libarchive.dll.a ^
    -lglfw3 -lbcrypt -lopengl32 -lgdi32 -luser32 -lkernel32 -lpthread
if %ERRORLEVEL% NEQ 0 (
    echo GUI build FAILED.
    exit /b 1
)
echo   OK: fMCG_gui.exe
goto :eof

:compile_core
echo   compiling src\%1.cpp
g++ -std=c++17 -O3 %EXTRA_CFLAGS% -c src\%1.cpp -I. -Isrc -Ivendor\libarchive -o lib\src\%1.o
if %ERRORLEVEL% NEQ 0 exit /b 1
goto :eof

:compile_gui
echo   compiling gui\%1.cpp
g++ -std=c++17 -O3 %EXTRA_CFLAGS% -c gui\%1.cpp -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\GLFW -I. -Isrc -Igui -o lib\gui\%1.o
if %ERRORLEVEL% NEQ 0 exit /b 1
goto :eof

copy /y vendor\libarchive\bin\libarchive.dll libarchive.dll >nul

echo.
echo Build complete. Run fMCG_gui.exe ^(libarchive.dll is alongside it^).
