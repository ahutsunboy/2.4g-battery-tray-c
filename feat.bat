@echo off
rem ============================================================
rem  Feature report power-state comparison
rem  Only 3 things to do:
rem    1) wait for ROUND 1
rem    2) flip the MOUSE switch OFF, press ENTER
rem    3) flip it back ON, press ENTER
rem  Takes ~30 seconds. Read-only, nothing is written.
rem ============================================================
cd /d "%~dp0"
if not exist featprobe.exe (
    echo [ERROR] featprobe.exe not found. Run build.bat first.
    pause
    exit /b 1
)
featprobe.exe
