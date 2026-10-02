@echo off
rem Locate vcvars64.bat for the desktop harness. Honours VCVARS when set;
rem otherwise probes the usual Visual Studio 18 / 2022 installs. No machine
rem path lives here - set VCVARS=<full path to vcvars64.bat> for anything else.
if defined VCVARS goto :call
call :try "%ProgramFiles%\Microsoft Visual Studio\18\Community"
call :try "%ProgramFiles%\Microsoft Visual Studio\18\Professional"
call :try "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
:call
if not defined VCVARS echo vcvars64.bat not found - set VCVARS to its full path& exit /b 1
if not exist "%VCVARS%" echo VCVARS points at a missing file: %VCVARS%& exit /b 1
call "%VCVARS%" >nul
exit /b 0
:try
if defined VCVARS exit /b 0
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvars64.bat"
exit /b 0
