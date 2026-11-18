@echo off
rem Build the MtpBth (Bluetooth) driver package for the installer: Release build + catalog.
rem The package is unsigned here. The installer signs MtpBth.sys and the catalog on the target PC with its one-time
rem certificate, and only when the PC is already in test-signing mode (the user turns that on).
setlocal
set ROOT=%~dp0..\..
set PKG=%ROOT%\build\drivers\Release\MtpBth
set WDK=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0

call "%ROOT%\drivers\build.cmd" Release || exit /b 1
if exist "%PKG%\mtpbth.cat" del /f /q "%PKG%\mtpbth.cat"
"%WDK%\x86\Inf2Cat.exe" /driver:"%PKG%" /os:10_X64 /uselocaltime || exit /b 1
echo Package: %PKG%
