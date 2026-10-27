@echo off
rem Builds and runs gamepad_test.exe: the gamepad layer's rules in src\GamepadCore.h (1.6.0: the ownership ledger,
rem dead zones, repeat, the router, the keys style and the page packet) on the desktop, no game. %1 = output folder
rem (default C:\b\mgl-gamepadtest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-gamepadtest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /I "%ROOT%\src" "%~dp0gamepad_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\gamepad_test.exe" || exit /b 1
"%OUT%\gamepad_test.exe"
exit /b %ERRORLEVEL%
