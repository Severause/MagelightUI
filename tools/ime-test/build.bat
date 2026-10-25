@echo off
rem Builds and runs the IME's offline checks against extern\ultralight (2.0), no game: ime_test.exe (src\ImeCore.h's
rem candidate-list parsing and '__ime' payload rules) and imeprobe.exe (the Editor calls the native composition relies
rem on, the anchor's space, a click mid-composition). Output in %1, else %MG_PROBE_DIR%, else C:\b\mgl-imeprobe.
rem MG_PAGE_FIBER=1 runs imeprobe on the page fiber. Exit 0 when every check passes.
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "UL=%ROOT%\extern\ultralight"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%MG_PROBE_DIR%"
if "%OUT%"=="" set "OUT=C:\b\mgl-imeprobe"
if not exist "%OUT%" mkdir "%OUT%"
call "%ROOT%\tools\desktop-harness\vcvars.bat" || exit /b 1
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /utf-8 /I "%ROOT%\src" "%~dp0ime_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\ime_test.exe" || exit /b 1
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /O2 /MD /utf-8 /I "%ROOT%\src" /I "%UL%\include" "%~dp0imeprobe.cpp" /Fo"%OUT%\\" /Fe"%OUT%\imeprobe.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
"%OUT%\ime_test.exe" || exit /b 1
"%OUT%\imeprobe.exe" "%UL%"
exit /b %ERRORLEVEL%
