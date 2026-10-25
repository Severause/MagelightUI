@echo off
rem Builds and runs ime_box_test.exe: the page core's IME candidate box in Ultralight's own WebKit, the reading composed
rem by the Editor and the anchor read as the host reads it: a field at the window's edge, under a zoom on <body> and on
rem <html>, in a same-origin iframe editor, a long candidate, a target clause, the opt-outs and an end, and the box's
rem font against the field's (22px, an iframe editor's own size, under a zoom, --ml-ime-font), and its colours (the
rem page's --ml-tooltip-*, a dark field on a translucent panel, a light page, an iframe editor's own, --ml-ime-*, an
rem unreadable pair, a chat bar over the game: its backdrop layer behind the field, or the gradient on an ancestor, a
rem vignette in front, a radial gradient, a url() ground), at device scale 1 and 1.5. Prints what each case measured and writes scale*_case*.png. %1 = output folder (default
rem C:\b\mgl-imeboxtest). Needs node on PATH (extract.cjs). MG_PAGE_FIBER=1 runs it on the page fiber.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-imeboxtest"
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "SDK=%ROOT%\extern\ultralight"
call "%ROOT%\tools\desktop-harness\vcvars.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
node "%~dp0extract.cjs" "%ROOT%\src\Magelight.cpp" "%OUT%\bridge-ime.js" || exit /b 1
cl /nologo /EHsc /std:c++20 /Zc:char8_t- /MD /O2 /utf-8 /I "%ROOT%\src" /I "%SDK%\include" "%~dp0ime_box_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\ime_box_test.exe" /link /LIBPATH:"%SDK%\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib || exit /b 1
copy /y "%SDK%\bin\*.dll" "%OUT%\" >nul
"%OUT%\ime_box_test.exe" "%SDK%" "%~dp0." "%OUT%\bridge-ime.js" "%OUT%"
exit /b %ERRORLEVEL%
