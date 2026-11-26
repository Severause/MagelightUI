@echo off
rem Builds and runs a11y_test.exe: src\A11yCore.h's rules (message prefix, size, rate gate, delivery), then the page
rem core's accessibility reader (src\A11yScript.h, 1.10.0) in Ultralight's own WebKit: off until '__a11y' "on",
rem names, roles, states and values, a password's value never sent, composites' active items, live regions read
rem once they settle, dialogs, data-ml-a11y="off", a same-origin iframe, the page answer, magelight.a11y.announce,
rem and "off" taking everything down. %1 = output folder (default C:\b\mgl-a11ytest). MG_PAGE_FIBER=1 runs it on the
rem page fiber, MG_PAGE_THREAD=1 on a page thread.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-a11ytest"
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "SDK=%ROOT%\extern\ultralight"
call "%ROOT%\tools\desktop-harness\vcvars.bat" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /Zc:char8_t- /MD /O2 /utf-8 /I "%ROOT%\src" /I "%SDK%\include" "%~dp0a11y_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\a11y_test.exe" /link /LIBPATH:"%SDK%\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib || exit /b 1
copy /y "%SDK%\bin\*.dll" "%OUT%\" >nul
"%OUT%\a11y_test.exe" "%SDK%" "%~dp0." "%OUT%"
exit /b %ERRORLEVEL%
