@echo off
rem stack_probe.exe (see README.md): the stack tests on a 1 MB thread, the way pages run with "ulFiberMB": 0. Built
rem into %MG_PROBE_DIR% (default C:\b\mgl-stackprobe) with the stock Ultralight runtime copied beside it, so the repo
rem stays clean. The page fiber's own test is tools\fiber-gc-test.
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
if not defined MG_PROBE_DIR set MG_PROBE_DIR=C:\b\mgl-stackprobe
if not exist "%MG_PROBE_DIR%" mkdir "%MG_PROBE_DIR%"
cl /nologo /EHsc /std:c++20 /MD /O2 /utf-8 /I "%~dp0..\..\src" /I "%~dp0..\..\extern\ultralight\include" "%~dp0stack_probe.cpp" /Fo:"%MG_PROBE_DIR%\\" /Fe:"%MG_PROBE_DIR%\stack_probe.exe" /link /LIBPATH:"%~dp0..\..\extern\ultralight\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib || exit /b 1
copy /y "%~dp0..\..\extern\ultralight\bin\*.dll" "%MG_PROBE_DIR%\" >nul
echo built %MG_PROBE_DIR%\stack_probe.exe
