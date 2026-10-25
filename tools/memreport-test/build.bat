@echo off
rem Builds and runs memreport_test.exe: src\MemoryReportCore.h, the parser behind Magelight.log's memory lines, against
rem the captured reports in fixtures\. %1 = output folder (default C:\b\mgl-memreporttest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-memreporttest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /I "%ROOT%\src" "%~dp0memreport_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\memreport_test.exe" || exit /b 1
"%OUT%\memreport_test.exe" "%~dp0fixtures"
exit /b %ERRORLEVEL%
