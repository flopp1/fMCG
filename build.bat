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
REM
REM Incremental builds: each source recompiles only when it or anything it
REM includes (tracked with g++ -MD dependency files in lib\) is newer than
REM its object. Unchanged objects are reused; an up-to-date exe is not
REM relinked. Switching between release and debug recompiles everything.
REM
REM Timestamp comparisons use "xcopy /D /L": it performs a REAL timestamp
REM check (string comparison on the formatted date stamps is lexicographic
REM and wrong across months/years). "/L" lists without copying;
REM "1 File(s)" means the source is newer.
REM ===========================================================================

set EXTRA_CFLAGS=
REM Release (default) links -mwindows: GUI subsystem, no console window.
REM "build.bat debug" additionally allocates one at startup for tracing.
set CONSOLE_FLAG=-mwindows
set MODE=release
if /i "%1"=="debug" (
    set EXTRA_CFLAGS=-DFMCG_DEBUG=1
    set CONSOLE_FLAG=
    set MODE=debug
    echo [debug build] ffmpeg stderr log + console window enabled.
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
if not exist lib\src mkdir lib\src
if not exist lib\gui mkdir lib\gui

REM Switching build mode invalidates every object: wipe and remember.
if exist lib\buildmode.txt (
    set /p OLD_MODE=<lib\buildmode.txt
) else (
    set OLD_MODE=
)
if /i not "!OLD_MODE!"=="%MODE%" (
    if not "!OLD_MODE!"=="" echo   Build mode changed to %MODE%: recompiling everything...
    del /q lib\src\*.o lib\src\*.d lib\gui\*.o lib\gui\*.d 2>nul
    echo %MODE%>"lib\buildmode.txt"
)

echo [1/2] Checking imgui static library...
set NEED_REBUILD=0
if not exist lib\libimgui.a set NEED_REBUILD=1
for %%f in (imgui imgui_draw imgui_widgets imgui_tables imgui_demo imgui_impl_glfw imgui_impl_opengl3) do (
    if not exist "lib\%%f.o" set NEED_REBUILD=1
)
REM Any imgui source newer than the archive -> rebuild the library.
if "!NEED_REBUILD!"=="0" (
    for %%s in (vendor\imgui\*.cpp vendor\imgui\backends\imgui_impl_glfw.cpp vendor\imgui\backends\imgui_impl_opengl3.cpp) do (
        for /f %%c in ('echo F ^| xcopy /D /L /Y "%%s" "lib\libimgui.a" 2^>nul ^| findstr /C:"1 File"') do set NEED_REBUILD=1
    )
)
if "!NEED_REBUILD!"=="1" (
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
set FAILED=0
for %%f in (fmcg_path fmcg_util fmcg_midi fmcg_engine fmcg_format fmcg_fonts fmcg_render) do call :compile_core %%f
if "!FAILED!"=="1" (
    echo Build FAILED during core compilation.
    exit /b 1
)
for %%f in (app_state dialogs jobs preview main colour_edit settings_store) do call :compile_gui %%f
if "!FAILED!"=="1" (
    echo Build FAILED during GUI compilation.
    exit /b 1
)

REM Link only when some input object/library is newer than the executable.
set NEED_LINK=0
if not exist fMCG_gui.exe (
    set NEED_LINK=1
    goto do_link
)
for %%o in (lib\gui\app_state.o lib\gui\dialogs.o lib\gui\jobs.o lib\gui\preview.o lib\gui\main.o lib\gui\colour_edit.o lib\gui\settings_store.o lib\src\fmcg_path.o lib\src\fmcg_util.o lib\src\fmcg_midi.o lib\src\fmcg_engine.o lib\src\fmcg_format.o lib\src\fmcg_fonts.o lib\src\fmcg_render.o lib\libimgui.a) do (
    for /f %%c in ('echo F ^| xcopy /D /L /Y "%%o" "fMCG_gui.exe" 2^>nul ^| findstr /C:"1 File"') do set NEED_LINK=1
)
if "!NEED_LINK!"=="0" (
    echo   fMCG_gui.exe is up to date.
    goto link_done
)
:do_link
g++ -std=c++17 -O3 -o fMCG_gui.exe lib\gui\app_state.o lib\gui\dialogs.o lib\gui\jobs.o lib\gui\preview.o lib\gui\main.o lib\gui\colour_edit.o lib\gui\settings_store.o lib\src\fmcg_path.o lib\src\fmcg_util.o lib\src\fmcg_midi.o lib\src\fmcg_engine.o lib\src\fmcg_format.o lib\src\fmcg_fonts.o lib\src\fmcg_render.o -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\GLFW -I. -Isrc -Igui -Ivendor\libarchive -Lvendor\GLFW -Lvendor\libarchive\lib lib\libimgui.a vendor\libarchive\lib\libarchive.dll.a -lglfw3 -lbcrypt -lopengl32 -lgdi32 -luser32 -lkernel32 -lpthread %CONSOLE_FLAG%
if errorlevel 1 (
    echo GUI build FAILED.
    exit /b 1
)
echo   OK: fMCG_gui.exe
:link_done

copy /y vendor\libarchive\bin\libarchive.dll libarchive.dll >nul

echo.
echo Build complete. Run fMCG_gui.exe ^(libarchive.dll is alongside it^).
goto :eof

:compile_core
call :uptodate "lib\src\%~1.o" "lib\src\%~1.d"
if "!UT!"=="1" goto :eof
echo   compiling src\%~1.cpp
g++ -std=c++17 -O3 %EXTRA_CFLAGS% -c src\%~1.cpp -I. -Isrc -Ivendor\libarchive -o lib\src\%~1.o -MD -MF lib\src\%~1.d
if errorlevel 1 (
    echo   compile FAILED: src\%~1.cpp
    set FAILED=1
)
goto :eof

:compile_gui
call :uptodate "lib\gui\%~1.o" "lib\gui\%~1.d"
if "!UT!"=="1" goto :eof
echo   compiling gui\%~1.cpp
g++ -std=c++17 -O3 %EXTRA_CFLAGS% -c gui\%~1.cpp -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\GLFW -I. -Isrc -Igui -o lib\gui\%~1.o -MD -MF lib\gui\%~1.d
if errorlevel 1 (
    echo   compile FAILED: gui\%~1.cpp
    set FAILED=1
)
goto :eof

REM :uptodate <object> <depfile>  --  sets UT=1 when the object is UP TO DATE
REM (exists, depfile exists, and no dependency named in the GCC -MD depfile
REM is newer). Tokens ending ".o" (the object names itself) and absolute
REM paths (toolchain headers, containing ":") are skipped; forward slashes
REM are normalized to backslashes for xcopy. On non-English Windows the
REM "1 File" match fails harmlessly: files always rebuild, never stale.
:uptodate
set UT=0
if not exist "%~1" goto :eof
if not exist "%~2" goto :eof
REM DIRTY=1 as soon as any dependency is newer than the object.
set DIRTY=0
for /f "usebackq delims=" %%L in ("%~2") do (
    for %%t in (%%L) do (
        set "DEP=%%t"
        if /i not "%%~xt"==".o" if /i not "%%~xt"==".o:" (
            set "DEP=!DEP:/=\!"
            if not "!DEP!"=="\" if "!DEP::=!"=="!DEP!" (
                if exist "!DEP!" if "!DIRTY!"=="0" (
                    REM xcopy /D /L lists (without copying) when the source is
                    REM newer than the target: a real timestamp comparison.
                    echo F| xcopy /D /L /Y "!DEP!" "%~1" >"!TEMP!\fmcg_xc.txt" 2>&1
                    findstr /C:"1 File" "!TEMP!\fmcg_xc.txt" >nul 2>&1 && set DIRTY=1
                )
            )
        )
    )
)
set UT=1
if "!DIRTY!"=="1" set UT=0
goto :eof
