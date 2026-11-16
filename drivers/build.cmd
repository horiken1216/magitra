@echo off
rem Build MtpBth.sys (KMDF 1.15, x64) with VS 2022 Build Tools + WDK 10.0.26100, without MSBuild integration.
rem (The WDK Visual Studio component cannot be added to Build Tools on this machine, so we call cl/link directly.)
rem Usage: build.cmd [Debug|Release] [analyze]
rem   analyze: also run the compiler's static analysis (/analyze, includes driver IRQL/SAL rules)
setlocal enabledelayedexpansion
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set ANALYZE=
if /i "%2"=="analyze" set ANALYZE=/analyze /analyze:WX-

set ROOT=%~dp0..
set SRC=%~dp0MtpBth
set OUT=%ROOT%\build\drivers\%CONFIG%
set OBJ=%ROOT%\build\obj\MtpBth\%CONFIG%
set PKG=%OUT%\MtpBth
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%PKG%" mkdir "%PKG%"

set WDKVER=10.0.26100.0
set KMDFVER=1.15
set WDK=%ProgramFiles(x86)%\Windows Kits\10

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  echo Visual Studio Build Tools not found
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2

rem vcvars adds user-mode include/lib paths; replace them with kernel-mode ones.
set INCLUDE=%WDK%\Include\%WDKVER%\km;%WDK%\Include\%WDKVER%\km\crt;%WDK%\Include\%WDKVER%\shared;%WDK%\Include\wdf\kmdf\%KMDFVER%
set LIB=

rem /kernel predefines _KERNEL_MODE. C4324 (structure padding) comes from the WDF headers themselves.
set DEFS=/D _WIN64 /D _AMD64_ /D AMD64 /D NTDDI_VERSION=0x0A000004 /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=15 /D POOL_NX_OPTIN=1 /D DEPRECATE_DDK_FUNCTIONS=1
if /i "%CONFIG%"=="Debug" (
  set CFG_CFLAGS=/Od /D DBG=1
) else (
  set CFG_CFLAGS=/O2 /D DBG=0
)

cl /nologo /c /kernel /utf-8 /W4 /WX /wd4324 /Zi /GS /Gy /Zc:wchar_t /Oi /GF /Zp8 /FC %ANALYZE% %CFG_CFLAGS% %DEFS% ^
  /I "%ROOT%\core\include" /Fd"%OBJ%\MtpBth.pdb" /Fo"%OBJ%\\" ^
  "%SRC%\Driver.c" "%SRC%\L2cap.c" "%ROOT%\core\src\mtp_report.c" || exit /b 1

link /nologo /DRIVER /KERNEL /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /NODEFAULTLIB /RELEASE /DEBUG ^
  /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /SECTION:INIT,d /OPT:REF /OPT:ICF /GUARD:CF /NXCOMPAT /DYNAMICBASE ^
  /PDB:"%OUT%\MtpBth.pdb" /OUT:"%PKG%\MtpBth.sys" ^
  "%OBJ%\Driver.obj" "%OBJ%\L2cap.obj" "%OBJ%\mtp_report.obj" ^
  "%WDK%\Lib\wdf\kmdf\x64\%KMDFVER%\WdfDriverEntry.lib" "%WDK%\Lib\wdf\kmdf\x64\%KMDFVER%\WdfLdr.lib" ^
  "%WDK%\Lib\%WDKVER%\km\x64\ntoskrnl.lib" "%WDK%\Lib\%WDKVER%\km\x64\hal.lib" ^
  "%WDK%\Lib\%WDKVER%\km\x64\wmilib.lib" "%WDK%\Lib\%WDKVER%\km\x64\BufferOverflowFastFailK.lib" || exit /b 1

rem Stamp DriverVer / KMDF version into the INF copy in the package directory.
copy /y "%SRC%\MtpBth.inf" "%PKG%\MtpBth.inf" >nul || exit /b 1
"%WDK%\bin\%WDKVER%\x64\stampinf.exe" -f "%PKG%\MtpBth.inf" -d * -v 0.1.0.0 -k %KMDFVER% -a amd64 >nul || exit /b 1

echo Built: %PKG%\MtpBth.sys
echo Next (catalog + test signing): see docs\INSTALL-SPIKE.md
