@echo off
rem Builds font_test.exe into %1 (default C:\b\mgl-fonttest) beside the stock Ultralight runtime it runs on,
rem then runs it. See README.md.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-fonttest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
set "UL=%ROOT%\extern\ultralight"
if not exist "%OUT%" mkdir "%OUT%"
rc /nologo /i "%ROOT%\src" /fo "%OUT%\MagelightFonts.res" "%ROOT%\src\MagelightFonts.rc" || exit /b 1
cl /nologo /EHsc /std:c++20 /MD /O2 /I "%UL%\include" /I "%ROOT%\src" "%~dp0font_test.cpp" "%ROOT%\src\MagelightFonts.cpp" "%OUT%\MagelightFonts.res" /Fo"%OUT%\\" /Fe"%OUT%\font_test.exe" /link /LIBPATH:"%UL%\lib" user32.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
xcopy /y /i /q "%UL%\resources" "%OUT%\resources" >nul
copy /y "%ROOT%\src\fonts\DejaVuSans.ttf" "%OUT%" >nul
cd /d "%OUT%" && "%OUT%\font_test.exe"
exit /b %ERRORLEVEL%
