@echo off
rem Build mtpdump.exe with VS 2022 Build Tools (x64)
setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\mtpdump
if not exist "%OUT%" mkdir "%OUT%"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio Build Tools not found
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2

cl /nologo /utf-8 /W4 /WX /I "%ROOT%\core\include" "%~dp0mtpdump.c" "%ROOT%\core\src\mtp_report.c" "%ROOT%\core\src\mtp_haptic.c" /Fo"%OUT%\\" /Fe"%OUT%\mtpdump.exe" /link cfgmgr32.lib
