@echo off
rem Builds and runs pathtext_test.exe: the path / UTF-8 / file:/// URL conversions of src\PathTextCore.h, and a page
rem loaded through Ultralight 2.0 (stock runtime, CPU view, no game) from a folder named in Chinese and Cyrillic with
rem '#' and '%' in it. %1 = output folder (default C:\b\mgl-pathtexttest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-pathtexttest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
set "UL=%ROOT%\extern\ultralight"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0pathtext_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\pathtext_test.exe" /link /LIBPATH:"%UL%\lib" || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
xcopy /y /i /q "%UL%\resources" "%OUT%\resources" >nul
"%OUT%\pathtext_test.exe"
exit /b %ERRORLEVEL%
