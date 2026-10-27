@echo off
rem The page fiber's gate (invariant 1; re-run it at every SDK update): builds fibergc.exe against extern\ultralight and
rem runs it without the page VM's lock, which must still crash (as Ultralight 2.0.0-beta.2 did in game on the fiber),
rem then with the host's lock (src\PageStackCore.h) on the CPU and on the GPU, three rounds each. GPU mode takes
rem MagelightGPU.dll from the build dir (MG_BUILD_DIR, default C:\b\mgl: its unpatched copy) and is skipped when there
rem is none. A failure means PageStackCore.h no longer holds for this SDK: an export it resolves is gone, or the lock
rem no longer cures the crash, or the crash no longer needs it. Then the page thread's gate (invariant 27): pages on a
rem thread of their own must pass WITHOUT the lock (a real thread's stack is WebKit's own, in a frame and between
rem frames) and with it (the host holds it there for speed), CPU and GPU. %1 = output folder (default
rem C:\b\mgl-fibergc). Exit 0 when all of it holds.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-fibergc"
set "BUILD=%MG_BUILD_DIR%"
if "%BUILD%"=="" set "BUILD=C:\b\mgl"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "UL=%ROOT%\extern\ultralight"
set "TESTS=%ROOT%\tools\stack-test\MgStackTest\views\test\stack-tests.js"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /Zc:char8_t- /EHsc /W4 /WX /O2 /MD /utf-8 /I "%ROOT%\src" /external:W0 /external:I "%UL%\include" "%~dp0fibergc.cpp" /Fo"%OUT%\\" /Fe"%OUT%\fibergc.exe" /link /LIBPATH:"%UL%\lib" Ultralight.lib UltralightCore.lib WebCore.lib AppCore.lib d3d11.lib user32.lib psapi.lib delayimp.lib /DELAYLOAD:UltralightCore.dll /DELAYLOAD:Ultralight.dll /DELAYLOAD:AppCore.dll || exit /b 1
copy /y "%UL%\bin\*.dll" "%OUT%" >nul
set FAILS=0
echo.
echo == without the lock (must crash)
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --lock none --rounds 3
if %ERRORLEVEL% EQU 0 (
  echo GATE FAIL: no crash without the lock - re-check whether the fiber still needs it
  set /a FAILS+=1
) else (
  echo gate: crashed without the lock, as expected
)
echo.
echo == the host's lock, CPU
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --lock session --rounds 3
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: CPU with the lock& set /a FAILS+=1)
echo.
echo == the page thread without the lock, CPU (must pass)
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --thread --lock none --rounds 3
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: the page thread without the lock, CPU& set /a FAILS+=1)
echo.
echo == the page thread with the host's lock, CPU
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --thread --lock session --rounds 3
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: the page thread with the lock, CPU& set /a FAILS+=1)
if not exist "%BUILD%\MagelightGPU.dll" (
  echo No %BUILD%\MagelightGPU.dll: GPU mode skipped ^(build.ps1 first^)
  goto done
)
echo.
echo == the host's lock, GPU
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --lock session --rounds 3 --gpu "%BUILD%"
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: GPU with the lock& set /a FAILS+=1)
echo.
echo == the page thread without the lock, GPU (must pass)
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --thread --lock none --rounds 3 --gpu "%BUILD%"
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: the page thread without the lock, GPU& set /a FAILS+=1)
echo.
echo == the page thread with the host's lock, GPU
"%OUT%\fibergc.exe" "%UL%" "%TESTS%" --thread --lock session --rounds 3 --gpu "%BUILD%"
if %ERRORLEVEL% NEQ 0 (echo GATE FAIL: the page thread with the lock, GPU& set /a FAILS+=1)
:done
echo.
if %FAILS% EQU 0 (echo GATE PASS) else (echo GATE FAILED: %FAILS%)
exit /b %FAILS%
