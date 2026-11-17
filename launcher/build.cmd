@echo off
rem Build mtplauncher.exe (WPF, .NET Framework 4.8) with the C# compiler that ships with Windows.
rem Output goes next to mtpagent.exe, which starts it hidden and shows it on the launcher gesture.
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\agent
if not exist "%OUT%" mkdir "%OUT%"

set FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319
set WPF=%FW%\WPF
if not exist "%FW%\csc.exe" (
  echo C# compiler not found: %FW%\csc.exe
  exit /b 2
)

"%FW%\csc.exe" /nologo /codepage:65001 /target:winexe /optimize+ /warnaserror+ ^
  /out:"%OUT%\mtplauncher.exe" /win32icon:"%ROOT%\assets\brand\magitra.ico" ^
  /r:"%WPF%\PresentationFramework.dll" /r:"%WPF%\PresentationCore.dll" /r:"%WPF%\WindowsBase.dll" ^
  /r:"%FW%\System.Xaml.dll" /r:"%FW%\System.dll" ^
  "%~dp0MtpLauncher.cs" "%~dp0LauncherItems.cs" || exit /b 1
echo Built: %OUT%\mtplauncher.exe
