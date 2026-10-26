@echo off
rem ============================================================
rem  Power-off capture experiment
rem  Follow the prompts printed by capture.exe.
rem  Takes ~2 minutes. Dongle must stay plugged in.
rem ============================================================
cd /d "%~dp0"
if not exist capture.exe (
    echo [ERROR] capture.exe not found. Run build.bat first.
    pause
    exit /b 1
)
capture.exe
