@echo off
REM ===========================================================================
REM  build.bat - build handeye.exe
REM
REM  Requires the Visual Studio "Desktop development with C++" workload.
REM  If cl.exe is not on PATH, run this from the
REM  "x64 Native Tools Command Prompt for VS" instead.
REM
REM  Note: keep this file pure ASCII. cmd.exe reads .bat as the OEM codepage,
REM  so UTF-8 Chinese comments here get mangled and break parsing.
REM ===========================================================================
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found.
    echo         Open "x64 Native Tools Command Prompt for VS" and run this again,
    echo         or call vcvars64.bat first.
    exit /b 1
)

echo Building handeye.exe ...
rem /MP: compile the translation units in parallel (cuts a full build from ~35s to ~10s)
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /MP /Fe:handeye.exe src\*.cpp windowsapp.lib gdi32.lib user32.lib ole32.lib

if errorlevel 1 (
    echo [FAILED]
    exit /b 1
)

if exist *.obj del /q *.obj

echo [OK] handeye.exe
exit /b 0
