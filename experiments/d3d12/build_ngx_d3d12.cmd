@echo off
call E:\Dev\GTA6\tools\VS2022\VC\Auxiliary\Build\vcvars64.bat >nul
if errorlevel 1 exit /b 1
cd /d E:\Dev\GTA6\phase2
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /Zi /MD /I E:\Dev\GTA6\DLSS-sdk\include /I E:\Dev\GTA6\tools\Streamline2141\include ngx_d3d12_fg.cpp /Fe:ngx_d3d12_fg.exe /link /DEBUG /LIBPATH:E:\Dev\GTA6\DLSS-sdk\lib\Windows_x86_64\x64 nvsdk_ngx_d.lib d3d12.lib dxgi.lib shlwapi.lib advapi32.lib user32.lib psapi.lib
exit /b %errorlevel%
