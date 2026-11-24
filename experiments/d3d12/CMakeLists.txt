cmake_minimum_required(VERSION 3.20)
project(FrameLabD3D12Reference LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The D3D12 reference requires a 64-bit Windows build.")
endif()

set(DLSS_NGX_SDK_ROOT "" CACHE PATH "DLSS SDK with D3D12 NGX headers and libraries")
set(STREAMLINE_ROOT_DIR "" CACHE PATH "Streamline SDK with headers")
set(ngx_library "${DLSS_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
if(NOT EXISTS "${ngx_library}" OR
   NOT EXISTS "${DLSS_NGX_SDK_ROOT}/include/nvsdk_ngx_helpers_dlssg_d3d.h" OR
   NOT EXISTS "${STREAMLINE_ROOT_DIR}/include/sl.h")
  message(FATAL_ERROR "Set DLSS_NGX_SDK_ROOT and STREAMLINE_ROOT_DIR to the external SDKs.")
endif()

add_executable(ngx_d3d12_fg ngx_d3d12_fg.cpp)
target_compile_features(ngx_d3d12_fg PRIVATE cxx_std_20)
target_include_directories(ngx_d3d12_fg PRIVATE "${DLSS_NGX_SDK_ROOT}/include" "${STREAMLINE_ROOT_DIR}/include")
target_link_libraries(ngx_d3d12_fg PRIVATE "${ngx_library}" d3d12 dxgi shlwapi advapi32 user32 psapi)
