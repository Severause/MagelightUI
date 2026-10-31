@echo off
rem Builds and runs pagethread_test.exe: the page thread's handoff (src\PageThreadCore.h, 1.7.0) on the desktop, no
rem game: when the page thread may run (Decide), and that every page frame reaches the game thread once, in order,
rem without the game thread ever waiting on it, a page stuck in a script included. %1 = output folder (default
rem C:\b\mgl-pagethreadtest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-pagethreadtest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /I "%ROOT%\src" "%~dp0pagethread_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\pagethread_test.exe" || exit /b 1
"%OUT%\pagethread_test.exe"
exit /b %ERRORLEVEL%
