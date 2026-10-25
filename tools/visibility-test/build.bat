@echo off
rem Builds and runs visprobe.exe: what View::set_visible does on Ultralight 2.0 (extern\ultralight), the behaviour
rem SyncViewVisibility and the prepaint rely on, on CPU views and then on GPU views through MagelightGPU.dll from the
rem build dir (MG_BUILD_DIR, default C:\b\mgl: its unpatched copy; skipped when there is none). No game.
rem %1 = output folder (default C:\b\mgl-vistest). Exit 0 when every check passes in every mode run.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-vistest"
set "BUILD=%MG_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=C:\b\mgl"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
set "UL=%ROOT%\extern\ultralight"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0visprobe.cpp" /Fo"%OUT%\\" /Fe"%OUT%\visprobe.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
"%OUT%\visprobe.exe" "%UL%" cpu
set "FAILS=%ERRORLEVEL%"
if not exist "%BUILD%\MagelightGPU.dll" echo No %BUILD%\MagelightGPU.dll: GPU mode skipped (build.ps1 first)& exit /b %FAILS%
"%OUT%\visprobe.exe" "%UL%" gpu "%BUILD%"
set /a FAILS=%FAILS%+%ERRORLEVEL%
exit /b %FAILS%
