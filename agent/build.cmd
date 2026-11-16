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
rem Replays mtpagent-cli --log recordings through the gesture logic
cl %CFLAGS% "%~dp0mtp_gesture.c" "%~dp0tests\replay_gesture.c" /Fo"%OUT%\\" /Fe"%OUT%\replay_gesture.exe" || exit /b 1
if /i "%1"=="test" exit /b 0

set COMMON="%~dp0mtp_gesture.c" "%~dp0mtp_device.c" "%~dp0mtp_input.c" "%~dp0mtp_haptic_out.c" "%ROOT%\core\src\mtp_report.c" "%ROOT%\core\src\mtp_haptic.c"

if not exist "%OUT%\cli" mkdir "%OUT%\cli"
if not exist "%OUT%\tray" mkdir "%OUT%\tray"

rem Console version (diagnostics / tests)
cl %CFLAGS% "%~dp0mtpagent_cli.c" %COMMON% /Fo"%OUT%\cli\\" /Fe"%OUT%\mtpagent-cli.exe" ^
  /link winusb.lib cfgmgr32.lib user32.lib hid.lib || exit /b 1
echo Built: %OUT%\mtpagent-cli.exe

rem Tray version (everyday use, no console window)
cl %CFLAGS% /D UNICODE /D _UNICODE "%~dp0mtpagent_tray.c" %COMMON% /Fo"%OUT%\tray\\" /Fe"%OUT%\mtpagent.exe" ^
  /link /SUBSYSTEM:WINDOWS winusb.lib cfgmgr32.lib user32.lib hid.lib shell32.lib advapi32.lib || exit /b 1
echo Built: %OUT%\mtpagent.exe
