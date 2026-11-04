@echo off
rem Builds and runs polledkeys_test.exe: src\PolledKeysCore.h's rule, list checks and key names (std only, no game).
rem %1 = output folder (default C:\b\mgl-polledkeystest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-polledkeystest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" "%~dp0polledkeys_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\polledkeys_test.exe" || exit /b 1
"%OUT%\polledkeys_test.exe"
exit /b %ERRORLEVEL%
