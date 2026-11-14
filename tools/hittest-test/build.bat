@echo off
rem Builds and runs hittest_test.exe: src\HitTestCore.h's walk, cursor, mode and answer rules (std only, no game).
rem %1 = output folder (default C:\b\mgl-hittesttest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-hittesttest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" "%~dp0hittest_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\hittest_test.exe" || exit /b 1
"%OUT%\hittest_test.exe"
exit /b %ERRORLEVEL%
