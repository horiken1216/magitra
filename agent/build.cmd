@echo off
rem Build the MTP agent and its gesture unit tests with VS 2022 Build Tools (x64)
rem Usage: build.cmd [test]   (test: build and run only the gesture unit tests)
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\agent
if not exist "%OUT%" mkdir "%OUT%"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio Build Tools not found
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2

set CFLAGS=/nologo /utf-8 /W4 /WX /I "%ROOT%\core\include" /I "%~dp0."

cl %CFLAGS% "%~dp0mtp_gesture.c" "%~dp0tests\test_gesture.c" /Fo"%OUT%\\" /Fe"%OUT%\test_gesture.exe" || exit /b 1
"%OUT%\test_gesture.exe" || exit /b 1
if /i "%1"=="test" exit /b 0

if exist "%~dp0mtpagent.c" (
  cl %CFLAGS% "%~dp0mtpagent.c" "%~dp0mtp_gesture.c" "%ROOT%\core\src\mtp_report.c" "%ROOT%\core\src\mtp_haptic.c" ^
    /Fo"%OUT%\\" /Fe"%OUT%\mtpagent.exe" /link winusb.lib cfgmgr32.lib user32.lib || exit /b 1
  echo Built: %OUT%\mtpagent.exe
)
