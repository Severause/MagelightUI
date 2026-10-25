@echo off
rem Builds and runs editorprobe.exe: the host's text-focus rules (src\EditStateCore.h) on real Ultralight 2.0 reports,
rem the VR keyboards' typed text as the host fires it, and views\keyboard\index.html's layouts, offline on CPU views, no
rem game. Re-run it at every SDK bump. %1 = output folder (default C:\b\mgl-editortest); the rest name sections
rem (core fields host unfocused typed keyboard; default all). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-editortest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "UL=%ROOT%\extern\ultralight"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0editorprobe.cpp" /Fo"%OUT%\\" /Fe"%OUT%\editorprobe.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib user32.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
shift
set "SECTIONS="
:args
if "%~1"=="" goto run
set "SECTIONS=%SECTIONS% %1"
shift
goto args
:run
"%OUT%\editorprobe.exe" "%UL%" "%ROOT%" %SECTIONS%
exit /b %ERRORLEVEL%
