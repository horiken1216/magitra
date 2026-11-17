@echo off
rem Build and run mkbrand.exe: makes the icons and logotype.png in assets\brand from tiger.webp and logotype.webp.
rem Run this only when the source images change; the outputs are committed.
setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\mkbrand
if not exist "%OUT%" mkdir "%OUT%"

set FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319
set WPF=%FW%\WPF
"%FW%\csc.exe" /nologo /codepage:65001 /target:exe /optimize+ /warnaserror+ ^
  /out:"%OUT%\mkbrand.exe" ^
  /r:"%WPF%\PresentationCore.dll" /r:"%WPF%\WindowsBase.dll" /r:"%FW%\System.Xaml.dll" /r:"%FW%\System.dll" ^
  "%~dp0mkbrand.cs" || exit /b 1
"%OUT%\mkbrand.exe" "%ROOT%\assets\brand" || exit /b 1
