@echo off
rem ============================================================
rem  c-tray build script (MinGW-w64 / w64devkit)
rem  usage:  build.bat             build dev tools only
rem          build.bat tray        also build tray.exe (main program)
rem          build.bat lowtest     also build tray_lowtest.exe (test only)
rem ============================================================
setlocal

rem ---- locate gcc: %GCC% > PATH > common install locations ----
if not defined GCC for /f "delims=" %%i in ('where gcc 2^>nul') do if not defined GCC set "GCC=%%i"
if not defined GCC if exist "%~dp0..\mingw64\bin\gcc.exe"        set "GCC=%~dp0..\mingw64\bin\gcc.exe"
if not defined GCC if exist "D:\tools\env\mingw64\bin\gcc.exe"    set "GCC=D:\tools\env\mingw64\bin\gcc.exe"
if not defined GCC if exist "C:\msys64\mingw64\bin\gcc.exe"       set "GCC=C:\msys64\mingw64\bin\gcc.exe"
if not defined GCC if exist "C:\w64devkit\bin\gcc.exe"            set "GCC=C:\w64devkit\bin\gcc.exe"
if not defined GCC if exist "%LOCALAPPDATA%\Programs\mingw64\bin\gcc.exe" set "GCC=%LOCALAPPDATA%\Programs\mingw64\bin\gcc.exe"

if not defined GCC (
    echo [ERROR] gcc not found.
    echo.
    echo   Pick one:
    echo     1^) put gcc on PATH
    echo     2^) set GCC=C:\path\to\gcc.exe
    echo     3^) unpack w64devkit to D:\tools\env\mingw64
    echo   See the "Build" section of README.md.
    exit /b 1
)

cd /d "%~dp0"
echo using gcc: %GCC%
echo.

set "CFLAGS=-O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE"
set "LIBS=-lsetupapi -lhid -luser32"

echo [1/7] dump.exe     - raw frame dumper
"%GCC%" %CFLAGS% -s -o dump.exe src\dump.c src\hid.c %LIBS% || exit /b 1

echo [2/7] capture.exe  - staged power-off experiment
"%GCC%" %CFLAGS% -s -o capture.exe src\capture.c src\hid.c %LIBS% || exit /b 1

echo [3/7] dmtest.exe   - monitor layer test bench
"%GCC%" %CFLAGS% -s -o dmtest.exe src\dmtest.c src\devmon.c src\log.c src\hid.c %LIBS% || exit /b 1

echo [4/7] feat2.exe    - feature report id/length scan
"%GCC%" %CFLAGS% -s -o feat2.exe src\feat2.c src\hid.c %LIBS% || exit /b 1

echo [5/7] featprobe.exe - feature report power-state diff
"%GCC%" %CFLAGS% -s -o featprobe.exe src\featprobe.c src\hid.c %LIBS% || exit /b 1

echo [6/7] iconout.exe   - export tray icons to bmp (for art diff)
"%GCC%" -O2 -Wall -Wextra -DUNICODE -D_UNICODE -s -o iconout.exe ^
    src\iconout.c src\art.c -lgdi32 -luser32 || exit /b 1

if /i "%~1"=="tray" (
    echo [7/7] tray.exe     - tray application
    "%GCC%" %CFLAGS% -mwindows -static -s -o tray.exe ^
        src\tray.c src\devmon.c src\log.c src\settings.c src\hid.c src\art.c ^
        %LIBS% -lshell32 -lgdi32 || exit /b 1
)

rem ---- diagnostic build: fake battery injection, NOT shipped ----
if /i "%~1"=="lowtest" (
    echo [diag] tray_lowtest.exe - LOWTEST build (fake_batt.txt injection)
    "%GCC%" %CFLAGS% -DLOWTEST -mwindows -static -s -o tray_lowtest.exe ^
        src\tray.c src\devmon.c src\log.c src\settings.c src\hid.c src\art.c ^
        %LIBS% -lshell32 -lgdi32 || exit /b 1
)

echo.
echo done.
dir /b *.exe
