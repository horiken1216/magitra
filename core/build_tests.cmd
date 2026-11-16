@echo off
rem Build and run MtpCore unit tests with VS 2022 Build Tools (x64)
setlocal
set ROOT=%~dp0
set OUT=%ROOT%..\build\core
if not exist "%OUT%" mkdir "%OUT%"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio Build Tools not found
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2

cl /nologo /utf-8 /W4 /WX /std:c11 /I "%ROOT%include" "%ROOT%src\mtp_report.c" "%ROOT%tests\test_report.c" /Fo"%OUT%\\" /Fe"%OUT%\test_report.exe" || exit /b 1
"%OUT%\test_report.exe"
