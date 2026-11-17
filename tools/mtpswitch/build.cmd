@echo off
rem Build mtpswitch.exe with VS 2022 Build Tools (x64)
setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\mtpswitch
if not exist "%OUT%" mkdir "%OUT%"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio Build Tools not found
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2

cl /nologo /utf-8 /W4 /WX /D UNICODE /D _UNICODE /I "%ROOT%\agent" "%~dp0mtpswitch.c" "%ROOT%\agent\mtp_switch.c" ^
  /Fo"%OUT%\\" /Fe"%OUT%\mtpswitch.exe" ^
  /link newdev.lib setupapi.lib cfgmgr32.lib advapi32.lib || exit /b 1
echo Built: %OUT%\mtpswitch.exe
