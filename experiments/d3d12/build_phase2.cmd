@echo off
call E:\Dev\GTA6\tools\VS2022\VC\Auxiliary\Build\vcvars64.bat >nul
if errorlevel 1 exit /b 1
cd /d E:\Dev\GTA6\phase2
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /Zi /I E:\Dev\GTA6\tools\Streamline2141\include phase2_cap.cpp /Fe:phase2_cap.exe /link /DEBUG d3d12.lib dxgi.lib
exit /b %errorlevel%
