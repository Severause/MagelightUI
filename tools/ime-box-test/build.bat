@echo off
rem Builds and runs ime_box_test.exe: the page core's IME box in Ultralight's own WebKit, at no zoom, under a
rem body zoom, in a same-origin iframe editor, with a long candidate and at an inline reading's target clause.
rem Prints what each case measured and writes case0..8.png; case 8 types a commit with real key events. %1 = output folder (default C:\b\mgl-imeboxtest). Needs node on PATH (extract.cjs).
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-imeboxtest"
set "ROOT=%~dp0..\.."
set "SDK=%ROOT%\extern\ultralight"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
node "%~dp0extract.cjs" "%ROOT%\src\Magelight.cpp" "%OUT%\bridge-ime.js" || exit /b 1
cl /nologo /EHsc /std:c++17 /MD /utf-8 /I "%SDK%\include" "%~dp0ime_box_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\ime_box_test.exe" /link /LIBPATH:"%SDK%\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib || exit /b 1
copy /y "%SDK%\bin\*.dll" "%OUT%\" >nul
"%OUT%\ime_box_test.exe" "%SDK%" "%~dp0." "%OUT%\bridge-ime.js" "%OUT%"
exit /b %ERRORLEVEL%
