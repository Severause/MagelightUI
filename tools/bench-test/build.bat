@echo off
rem Builds bench.exe against one Ultralight SDK and lays out what it runs on: %1 = SDK folder, %2 = output folder.
rem The output gets the SDK's DLLs beside the exe and assets\ (the SDK's resources, this folder's pages, and with %3 a
rem copy of a built view folder as assets\sa). run.ps1 builds both arms and pairs each SDK with its MagelightGPU.dll.
setlocal
set "UL=%~1"
set "OUT=%~2"
if "%UL%"=="" (echo usage: build.bat sdkDir outDir [viewDir]& exit /b 1)
if "%OUT%"=="" (echo usage: build.bat sdkDir outDir [viewDir]& exit /b 1)
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\gpu" /external:W0 /external:I "%UL%\include" "%~dp0bench.cpp" /Fo"%OUT%\\" /Fe"%OUT%\bench.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib d3d11.lib user32.lib psapi.lib winmm.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
xcopy /y /i /q /e "%UL%\resources" "%OUT%\assets\resources" >nul
xcopy /y /i /q "%~dp0pages" "%OUT%\assets\pages" >nul
if not "%~3"=="" xcopy /y /i /q /e "%~3" "%OUT%\assets\sa" >nul
exit /b 0
