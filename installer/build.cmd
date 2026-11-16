@echo off
rem Build build\mtpsetup.exe: rebuilds everything it embeds, then packs the files into one executable.
rem Exit the running MTP Agent first if it was started from build\agent (its exe cannot be overwritten while running).
setlocal
set ROOT=%~dp0..
set B=%ROOT%\build

call "%ROOT%\agent\build.cmd" || exit /b 1
call "%ROOT%\settings\build.cmd" || exit /b 1
rem Fresh, unsigned catalog. The installer signs it on the target PC with a one-time certificate.
call "%ROOT%\drivers\MtpUsb\package.cmd" || exit /b 1

set FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319
set WPF=%FW%\WPF
"%FW%\csc.exe" /nologo /codepage:65001 /target:winexe /optimize+ /warnaserror+ ^
  /win32manifest:"%~dp0mtpsetup.manifest" /out:"%B%\mtpsetup.exe" ^
  /r:"%WPF%\PresentationFramework.dll" /r:"%WPF%\PresentationCore.dll" /r:"%WPF%\WindowsBase.dll" ^
  /r:"%FW%\System.Xaml.dll" /r:"%FW%\System.dll" /r:"%FW%\System.Xml.dll" ^
  /resource:"%~dp0setup-core.ps1",setup-core.ps1 ^
  /resource:"%B%\agent\mtpagent.exe",mtpagent-standard.exe ^
  /resource:"%B%\agent\install\mtpagent.exe",mtpagent-uiaccess.exe ^
  /resource:"%B%\agent\mtpsettings.exe",mtpsettings.exe ^
  /resource:"%B%\drivers\MtpUsb\MtpUsb.inf",MtpUsb.inf ^
  /resource:"%B%\drivers\MtpUsb\mtpusb.cat",mtpusb.cat ^
  "%~dp0MtpSetup.cs" || exit /b 1
echo Built: %B%\mtpsetup.exe
