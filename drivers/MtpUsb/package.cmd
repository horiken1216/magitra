@echo off
rem Create the MtpUsb (WinUSB) driver package: copy the INF and generate the catalog.
rem Signing the catalog is a separate step (needs the user's test certificate): see docs\MANUAL-INSTALL.md
setlocal
set ROOT=%~dp0..\..
set PKG=%ROOT%\build\drivers\MtpUsb
set WDK=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0

if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%" || exit /b 1
copy /y "%~dp0MtpUsb.inf" "%PKG%\MtpUsb.inf" >nul || exit /b 1

"%WDK%\x86\Inf2Cat.exe" /driver:"%PKG%" /os:10_X64 /uselocaltime || exit /b 1
echo Package: %PKG%
