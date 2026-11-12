@echo off
rem Builds and runs lowmemory_test.exe: src\LowMemoryCore.h's needs, floor, hysteresis, growth room and setting clamp
rem (std only, no game).
rem %1 = output folder (default C:\b\mgl-lowmemorytest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-lowmemorytest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" "%~dp0lowmemory_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\lowmemory_test.exe" || exit /b 1
"%OUT%\lowmemory_test.exe"
exit /b %ERRORLEVEL%
