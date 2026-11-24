@echo off
call E:\Dev\GTA6\tools\VS2022\VC\Auxiliary\Build\vcvars64.bat >nul
if errorlevel 1 exit /b 1
cd /d E:\Dev\GTA6\dlssg-open\native\windows
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /Zi /MD /I E:\Dev\GTA6\tools\VulkanSDK\Include /LD cubin_layer_win.cpp /Fe:VkLayer_dlssg_open.dll /link /DEBUG psapi.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /Zi /MD /I E:\Dev\GTA6\tools\VulkanSDK\Include /I E:\Dev\GTA6\DLSS-sdk\include ngx_probe_win.cpp /Fe:ngx_probe_win.exe /link /DEBUG /LIBPATH:E:\Dev\GTA6\DLSS-sdk\lib\Windows_x86_64\x64 /LIBPATH:E:\Dev\GTA6\tools\VulkanSDK\Lib nvsdk_ngx_d.lib vulkan-1.lib shlwapi.lib advapi32.lib user32.lib psapi.lib
exit /b %errorlevel%
