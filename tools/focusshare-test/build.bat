@echo off
rem Builds and runs focusshare_test.exe: src\FocusShareCore.h's press and settle rules (std only, no game).
rem %1 = output folder (default C:\b\mgl-focussharetest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-focussharetest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" "%~dp0focusshare_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\focusshare_test.exe" || exit /b 1
"%OUT%\focusshare_test.exe"
exit /b %ERRORLEVEL%
