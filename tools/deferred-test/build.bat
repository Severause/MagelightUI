@echo off
rem The GPU driver on a deferred context (the page thread, invariant 27) against the immediate context (the in-frame
rem path): builds deferred_test.exe against extern\ultralight, then renders each page both ways, MSAA off and 4x, and
rem compares the View's resolved target byte for byte; then on WARP, plain and with flush splits (its DriverCommandLists says whether the
rem runtime's emulated command lists, and the driver's UpdateSubresource adjustment for them, were exercised); then the
rem upload case (a box away from the texture's origin), the state case (the game's bindings survive each page frame)
rem and the release case (a View freed while a list that drew it waits and the main thread still holds its target).
rem Every thread-mode run checks that each flush split the recording. MagelightGPU.dll comes from the build
rem dir (MG_BUILD_DIR, default C:\b\mgl: its unpatched copy). %1 = output folder (default C:\b\mgl-deferred).
rem Exit 0 when every pair matches.
setlocal EnableDelayedExpansion
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-deferred"
set "BUILD=%MG_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=C:\b\mgl"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "UL=%ROOT%\extern\ultralight"
if not exist "%BUILD%\MagelightGPU.dll" (echo No %BUILD%\MagelightGPU.dll: build.ps1 first& exit /b 1)
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /I "%ROOT%\gpu" /external:W0 /external:I "%UL%\include" "%~dp0deferred_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\deferred_test.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib d3d11.lib user32.lib || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
set "EXE=%OUT%\deferred_test.exe"
rem Ultralight writes its log into the working folder: keep it in OUT.
pushd "%OUT%"
set FAILS=0
for %%P in ("%~dp0pages\mixed.html" "%ROOT%\tools\desktop-harness\pages\translucent-borders.html") do (
  for %%M in (1 4) do (
    echo.
    echo == %%~nxP, msaa %%M
    "%EXE%" "%UL%" %%P --gpu "%BUILD%" --mode immediate --msaa %%M --out "%OUT%\a.raw" || (echo FAIL: immediate run& set /a FAILS+=1)
    "%EXE%" "%UL%" %%P --gpu "%BUILD%" --mode thread --msaa %%M --out "%OUT%\b.raw" || (echo FAIL: thread run& set /a FAILS+=1)
    "%EXE%" --compare "%OUT%\a.raw" "%OUT%\b.raw" || (echo FAIL: %%~nxP msaa %%M differs& set /a FAILS+=1)
  )
)
echo.
echo == WARP, mixed.html, msaa 4
"%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode immediate --msaa 4 --warp --out "%OUT%\a.raw" || (echo FAIL: WARP immediate run& set /a FAILS+=1)
"%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode thread --msaa 4 --warp --out "%OUT%\b.raw" || (echo FAIL: WARP thread run& set /a FAILS+=1)
"%EXE%" --compare "%OUT%\a.raw" "%OUT%\b.raw" || (echo FAIL: WARP differs& set /a FAILS+=1)
echo.
echo == WARP, translucent-borders.html, msaa 4: the flush splits on emulated command lists
"%EXE%" "%UL%" "%ROOT%\tools\desktop-harness\pages\translucent-borders.html" --gpu "%BUILD%" --mode immediate --msaa 4 --warp --out "%OUT%\a.raw" || (echo FAIL: WARP splits immediate run& set /a FAILS+=1)
"%EXE%" "%UL%" "%ROOT%\tools\desktop-harness\pages\translucent-borders.html" --gpu "%BUILD%" --mode thread --msaa 4 --warp --out "%OUT%\b.raw" || (echo FAIL: WARP splits thread run& set /a FAILS+=1)
"%EXE%" --compare "%OUT%\a.raw" "%OUT%\b.raw" || (echo FAIL: WARP splits differ& set /a FAILS+=1)
for %%D in (hardware warp) do (
  set "DEV="
  if "%%D"=="warp" set "DEV=--warp"
  echo.
  echo == upload case, %%D: a box away from the texture's origin, immediate then deferred
  "%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode immediate --case upload !DEV! --out "%OUT%\a.raw" || (echo FAIL: upload, %%D, immediate& set /a FAILS+=1)
  "%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode thread --case upload !DEV! --out "%OUT%\b.raw" || (echo FAIL: upload, %%D, deferred& set /a FAILS+=1)
)
for %%D in (hardware warp) do (
  set "DEV="
  if "%%D"=="warp" set "DEV=--warp"
  echo.
  echo == state case, %%D: the game's bindings survive every page frame, in the frame then on the page thread
  "%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode immediate --msaa 4 --case state !DEV! --out "%OUT%\a.raw" || (echo FAIL: state, %%D, immediate& set /a FAILS+=1)
  "%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode thread --msaa 4 --case state !DEV! --out "%OUT%\b.raw" || (echo FAIL: state, %%D, thread& set /a FAILS+=1)
)
echo.
echo == release case, mixed.html, msaa 4
"%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode immediate --msaa 4 --case release --out "%OUT%\a.raw" || (echo FAIL: release immediate run& set /a FAILS+=1)
"%EXE%" "%UL%" "%~dp0pages\mixed.html" --gpu "%BUILD%" --mode thread --msaa 4 --case release --out "%OUT%\b.raw" || (echo FAIL: release thread run& set /a FAILS+=1)
"%EXE%" --compare "%OUT%\a.raw" "%OUT%\b.raw" || (echo FAIL: release case differs& set /a FAILS+=1)
echo.
popd
if %FAILS% EQU 0 (echo ALL MATCH) else (echo FAILED: %FAILS%)
exit /b %FAILS%
