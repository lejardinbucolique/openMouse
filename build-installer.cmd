@echo off
REM openMouse - build the Windows installer (test or release).
REM Usage: build-installer.cmd [version]
REM
REM Builds openmouse.exe with build.cmd, then compiles installer\openmouse.iss
REM with Inno Setup 6. The result is dist\openMouse-setup-<version>.exe; dist\ is
REM not tracked by git and the installer is never committed.
REM
REM Keep this file pure ASCII (see build.cmd for why).
setlocal
cd /d "%~dp0"

set "VERSION=%~1"
if "%VERSION%"=="" (
    set "VERSION=0.1.0"
    for /f %%h in ('git rev-parse --short HEAD 2^>nul') do set "VERSION=0.1.0-%%h"
)

set "ISCC="
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo [installer] Could not find ISCC.exe. Install Inno Setup 6 from jrsoftware.org.
    exit /b 1
)

echo [installer] building openmouse.exe
call "%~dp0build.cmd"
if errorlevel 1 (
    echo [installer] build.cmd failed.
    exit /b 1
)

echo [installer] compiling the installer, version %VERSION%
"%ISCC%" /Qp "/DMyVersion=%VERSION%" installer\openmouse.iss
if errorlevel 1 (
    echo [installer] ISCC failed.
    exit /b 1
)
echo [installer] done: dist\openMouse-setup-%VERSION%.exe
exit /b 0
