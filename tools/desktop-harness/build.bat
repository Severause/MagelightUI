@echo off
call "%~dp0vcvars.bat" || exit /b 1
cl /nologo /EHsc /std:c++17 /MD /I "%~dp0..\..\extern\ultralight\include" harness.cpp /Fe:harness.exe /link /LIBPATH:"%~dp0..\..\extern\ultralight\lib" Ultralight.lib WebCore.lib UltralightCore.lib AppCore.lib
