@echo off
rem Build everything: run all unit tests, then build build\mtpsetup.exe (see docs\BUILD.md)
setlocal
call "%~dp0core\build_tests.cmd" || exit /b 1
call "%~dp0installer\build.cmd" || exit /b 1
