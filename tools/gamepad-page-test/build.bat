@echo off
rem Builds and runs gamepad_page_test.exe: the page core's gamepad helper (src\GamepadScript.h, 1.6.0) in Ultralight's
rem own WebKit: focus moved by geometry (also under a CSS zoom and at device scale 1.5), scopes and a modal's restore,
rem recovery after the DOM changes, range and select editing, text fields left alone, tabs on the shoulders, the keys
rem style's state-only packets, prompt labels, stale and oversized packets dropped, and the cancel a reset broadcasts.
rem %1 = output folder (default C:\b\mgl-gamepadpagetest). MG_PAGE_FIBER=1 runs it on the page fiber.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-gamepadpagetest"
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "SDK=%ROOT%\extern\ultralight"
call "%ROOT%\tools\desktop-harness\vcvars.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /Zc:char8_t- /MD /O2 /utf-8 /I "%ROOT%\src" /I "%SDK%\include" "%~dp0gamepad_page_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\gamepad_page_test.exe" /link /LIBPATH:"%SDK%\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib || exit /b 1
copy /y "%SDK%\bin\*.dll" "%OUT%\" >nul
"%OUT%\gamepad_page_test.exe" "%SDK%" "%~dp0." "%OUT%"
exit /b %ERRORLEVEL%
