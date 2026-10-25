@echo off
rem Builds and runs bridgetest.exe: src\JsBridgeCore.h and the page's __mlNative on Ultralight 2.0 (extern\ultralight),
rem on a CPU view, no game. %1 = output folder (default C:\b\mgl-bridgetest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-bridgetest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
set "UL=%ROOT%\extern\ultralight"
if not exist "%OUT%\resources" mkdir "%OUT%\resources"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0bridgetest.cpp" /Fo"%OUT%\\" /Fe"%OUT%\bridgetest.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
copy /y "%UL%\resources\*" "%OUT%\resources" >nul
"%OUT%\bridgetest.exe" "%OUT%"
exit /b %ERRORLEVEL%
