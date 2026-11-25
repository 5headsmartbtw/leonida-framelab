cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME vk_streamline)
project(${PROJECT_NAME})

message(STATUS "-------------------------------")
message(STATUS "Processing Project ${PROJECT_NAME}:")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

set(STREAMLINE_MANUAL_HOOKING ON CACHE BOOL "Use manual hooking method of Streamline instead of linking against the interposer")
set(STREAMLINE_ROOT_DIR "" CACHE PATH "Streamline SDK root")
option(FRAMELAB_USE_INSTALLED_VULKAN_SR "Exercise Vulkan SR through the installed SDK" OFF)
if(FRAMELAB_USE_INSTALLED_VULKAN_SR)
  find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS VulkanSR)
endif()
set(DLSS_NGX_SDK_ROOT "" CACHE PATH "DLSS SDK root with NGX FG headers")
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")
option(FRAMELAB_USE_INSTALLED_VULKAN_FG "Exercise the installed VulkanFG C ABI on the reference scene" OFF)
if(FRAMELAB_USE_INSTALLED_VULKAN_FG)
  find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS VulkanFG)
endif()

if(NOT EXISTS "${STREAMLINE_ROOT_DIR}/include/sl.h" OR
   NOT EXISTS "${STREAMLINE_ROOT_DIR}/bin/x64/sl.interposer.dll")
  message(FATAL_ERROR "Set STREAMLINE_ROOT_DIR to the Streamline SDK with include/ and bin/x64/.")
endif()
if(NOT FRAMELAB_USE_INSTALLED_VULKAN_FG)
  if(NOT EXISTS "${DLSS_NGX_SDK_ROOT}/include/nvsdk_ngx_helpers_dlssg_vk.h" OR
     NOT EXISTS "${DLSS_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(FATAL_ERROR "Set DLSS_NGX_SDK_ROOT to the DLSS SDK with Vulkan FG headers and the x64 NGX import library.")
  endif()
endif()

#--------------------------------------------------------------------------------------------------
# look for nvpro_core 1) as a sub-folder 2) at some other locations
# this cannot be put anywhere else since we still didn't find setup.cmake yet
#
if(NOT BASE_DIRECTORY)
  find_path(BASE_DIRECTORY
    NAMES nvpro_core/cmake/setup.cmake
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    REQUIRED
    DOC "Directory containing nvpro_core"
    )
endif()

if(EXISTS "${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake")
  include("${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake")
  include("${BASE_DIRECTORY}/nvpro_core/cmake/utilities.cmake")
else()
  message(FATAL_ERROR "could not find base directory, please set BASE_DIRECTORY to folder containing nvpro_core")
endif()

set(NVPRO_CORE_DIR ${BASE_DIRECTORY}/nvpro_core)
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)

macro(_add_package_Streamline)
  if(STREAMLINE_ROOT_DIR AND EXISTS "${STREAMLINE_ROOT_DIR}")
    set(STREAMLINE_INCLUDE_DIR ${STREAMLINE_ROOT_DIR}/include)
    set(STREAMLINE_LIBRARY ${STREAMLINE_ROOT_DIR}/lib/x64/sl.interposer.lib)

    set(STREAMLINE_DLLS
        ${STREAMLINE_ROOT_DIR}/bin/x64/sl.interposer.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/sl.common.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/sl.dlss.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/sl.dlss_g.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/sl.reflex.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/NvLowLatencyVk.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/nvngx_dlss.dll
        ${STREAMLINE_ROOT_DIR}/bin/x64/nvngx_dlssg.dll
        )

    if(EXISTS "${STREAMLINE_ROOT_DIR}/bin/x64/sl.pcl.dll")
      list(APPEND STREAMLINE_DLLS "${STREAMLINE_ROOT_DIR}/bin/x64/sl.pcl.dll")
    endif()

    include_directories(${STREAMLINE_INCLUDE_DIR})

    if(STREAMLINE_MANUAL_HOOKING)
      add_definitions(-DSL_MANUAL_HOOKING=1)
    else()
      add_definitions(-DSL_MANUAL_HOOKING=0)

      # Replace Vulkan library with Streamline interposer
      list(APPEND LIBRARIES_OPTIMIZED ${STREAMLINE_LIBRARY})
      list(APPEND LIBRARIES_DEBUG ${STREAMLINE_LIBRARY})
      list(REMOVE_ITEM LIBRARIES_OPTIMIZED ${Vulkan_LIBRARY})
      list(REMOVE_ITEM LIBRARIES_DEBUG ${Vulkan_LIBRARY})
      list(REMOVE_ITEM PLATFORM_LIBRARIES ${Vulkan_LIBRARY})

      set(Vulkan_LIBRARY ${STREAMLINE_LIBRARY})
      set(Vulkan_LIBRARIES ${Vulkan_LIBRARY})
    endif()
  else()
    message("Streamline SDK not found.")
  endif()
endmacro()

_add_package_VulkanSDK()
_add_package_ImGUI()
_add_package_Streamline() # Add after Vulkan SDK package, to potentially replace Vulkan library

_add_nvpro_core_lib()

#####################################################################################
# Source files for this project
#
file(GLOB SOURCE_FILES
    *.cpp
    *.hpp
    *.h
    )

file(GLOB SHADER_SOURCE_FILES  # Shader sources
    ${SHADER_DIR}/*.vert
    ${SHADER_DIR}/*.frag
    ${SHADER_DIR}/*.rgen
    ${SHADER_DIR}/*.rchit
    ${SHADER_DIR}/*.rahit
    ${SHADER_DIR}/*.rmiss
    ${SHADER_DIR}/*.comp
    )

file(GLOB SHADER_HEADER_FILES  # Shader headers
    ${SHADER_DIR}/*.glsl
    ${SHADER_DIR}/*.h
    )

source_group("Shader Files" FILES ${SHADER_SOURCE_FILES} ${SHADER_HEADER_FILES})

#####################################################################################
# Executable
#
add_executable(${PROJECT_NAME}
    ${SOURCE_FILES}
    ${COMMON_SOURCE_FILES}
    ${PACKAGE_SOURCE_FILES}
    )

target_sources(${PROJECT_NAME} PRIVATE
    ${SHADER_SOURCE_FILES}
    ${SHADER_HEADER_FILES}
    )

target_link_libraries(${PROJECT_NAME}
    ${PLATFORM_LIBRARIES}
    ${LIBRARIES_OPTIMIZED}
    nvpro_core
    )

_add_project_definitions(${PROJECT_NAME})

if(NOT EXISTS "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")
  message(FATAL_ERROR "glslangValidator is required. Set VULKAN_SDK to the Vulkan SDK root.")
endif()
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_autogen")
foreach(shader IN LISTS SHADER_SOURCE_FILES)
  get_filename_component(filename "${shader}" NAME)
  string(REPLACE "." "_" symbol "${filename}")
  set(header "${SHADER_OUTPUT_DIR}/${filename}.h")
  add_custom_command(
    OUTPUT "${header}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -g --target-env vulkan1.2
            "-I${SHADER_DIR}" "-I${NVPRO_CORE_DIR}" --vn "${symbol}" -o "${header}" "${shader}"
    DEPENDS "${shader}" ${SHADER_HEADER_FILES}
    VERBATIM
  )
  target_sources(${PROJECT_NAME} PRIVATE "${header}")
endforeach()

_finalize_target(${PROJECT_NAME})
_copy_files_to_target(${PROJECT_NAME} "${STREAMLINE_DLLS}")
install(FILES ${STREAMLINE_DLLS} CONFIGURATIONS Release DESTINATION bin_${ARCH})
install(FILES ${STREAMLINE_DLLS} CONFIGURATIONS Debug DESTINATION bin_${ARCH}_debug)

# Independent direct-NGX backend.
if(FRAMELAB_USE_INSTALLED_VULKAN_FG)
  target_compile_definitions(${PROJECT_NAME} PRIVATE FRAMELAB_USE_INSTALLED_VULKAN_FG)
  target_link_libraries(${PROJECT_NAME} FrameLab::FrameLab)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    VERBATIM)
else()
  target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${DLSS_NGX_SDK_ROOT}/include")
  target_link_libraries(${PROJECT_NAME} "${DLSS_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" shlwapi advapi32)
endif()

if(FRAMELAB_USE_INSTALLED_VULKAN_SR)
  target_compile_definitions(${PROJECT_NAME} PRIVATE FRAMELAB_USE_INSTALLED_VULKAN_SR)
  target_link_libraries(${PROJECT_NAME} FrameLab::FrameLab)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    VERBATIM)
endif()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}")
