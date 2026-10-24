@echo off
rem Builds and runs ime_test.exe: the host half of the page-drawn IME (src\ImeCore.h) on the
rem desktop, no game. %1 = output folder (default C:\b\mgl-imetest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-imetest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /utf-8 /I "%ROOT%\src" "%~dp0ime_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\ime_test.exe" || exit /b 1
"%OUT%\ime_test.exe"
exit /b %ERRORLEVEL%
