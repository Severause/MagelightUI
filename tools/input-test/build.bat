@echo off
rem Builds and runs inputprobe.exe: what a page sees from the input Magelight hands Ultralight 2.0 (src\InputCore.h),
rem offline on CPU views, no game. Keys through Input::MakeKeyEvent as the flat window and the VR paths queue them,
rem the polled-keys filter around the constructor, the releases sent when focus goes, the leave sent to a view the
rem pointer left, the page core's pointer guard, which view each pointer source's events go to (two VR hands), and
rem (printed) the matrix the design came from. Re-run it at every SDK
rem bump. %1 = output folder (default C:\b\mgl-inputtest). The polled-keys sections need MinHook from the build dir's
rem vcpkg (MG_BUILD_DIR, default C:\b\mgl); the es-419 cases need that keyboard layout installed (skipped otherwise).
rem Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-inputtest"
set "BUILD=%MG_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=C:\b\mgl"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
set "UL=%ROOT%\extern\ultralight"
set "VCPKG=%BUILD%\vcpkg_installed\x64-windows-static"
rem The runtime sits where the game has it, in a folder named SKSE\Plugins\Magelight, which the polled-keys filter
rem exempts by name; the control copy sits in another plugin's folder.
set "RUN=%OUT%\Data\SKSE\Plugins\Magelight"
set "CTL=%OUT%\Data\SKSE\Plugins\OtherPlugin"
if not exist "%RUN%" mkdir "%RUN%"
if not exist "%CTL%" mkdir "%CTL%"
set "HOOKDEF=/DMG_NO_MINHOOK"
set "HOOKLIB="
if exist "%VCPKG%\lib\minhook.x64.lib" (
  set "HOOKDEF=/DMG_MINHOOK"
  set "HOOKLIB=minhook.x64.lib"
  set "INCLUDE=%VCPKG%\include;%INCLUDE%"
  set "LIB=%VCPKG%\lib;%LIB%"
) else (
  echo No MinHook under %VCPKG%: the polled-keys sections are skipped ^(build.ps1 first^)
)
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MT /utf-8 %HOOKDEF% /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0inputprobe.cpp" /Fo"%OUT%\\" /Fe"%RUN%\inputprobe.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib user32.lib %HOOKLIB% || exit /b 1
copy /y "%UL%\bin\*.dll" "%RUN%" >nul
"%RUN%\inputprobe.exe" "%UL%"
set "FAILS=%ERRORLEVEL%"
if "%HOOKLIB%"=="" exit /b %FAILS%
copy /y "%RUN%\*.dll" "%CTL%" >nul
copy /y "%RUN%\inputprobe.exe" "%CTL%" >nul
"%CTL%\inputprobe.exe" "%UL%" polledcontrol
set /a FAILS=%FAILS%+%ERRORLEVEL%
exit /b %FAILS%
