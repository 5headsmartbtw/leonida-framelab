cmake_minimum_required(VERSION 3.20)
project(FrameLabExternalConsumer LANGUAGES C CXX)
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED)
enable_testing()

add_executable(c_consumer consumer.c)
target_compile_features(c_consumer PRIVATE c_std_11)
add_executable(cpp_consumer consumer.cpp)
target_compile_features(cpp_consumer PRIVATE cxx_std_20)
get_target_property(framelab_type FrameLab::FrameLab TYPE)
set(consumers c_consumer cpp_consumer)
if(FrameLab_VulkanSR_FOUND)
  add_executable(vulkan_sr_header vulkan_sr_header.c)
  target_compile_features(vulkan_sr_header PRIVATE c_std_11)
  list(APPEND consumers vulkan_sr_header)
endif()
if(FrameLab_D3D12SR_FOUND)
  add_executable(d3d12_sr_header d3d12_sr_header.c)
  target_compile_features(d3d12_sr_header PRIVATE c_std_11)
  list(APPEND consumers d3d12_sr_header)
endif()
foreach(consumer IN LISTS consumers)
  target_link_libraries(${consumer} PRIVATE FrameLab::FrameLab)
  if(WIN32 AND framelab_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET ${consumer} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:${consumer}>"
      VERBATIM)
  endif()
  add_test(NAME ${consumer} COMMAND ${consumer})
endforeach()

# Native layout probe for the Rust ABI test. It compiles the installed C header.
add_executable(abi_layout abi_layout.c)
target_compile_features(abi_layout PRIVATE c_std_11)
target_link_libraries(abi_layout PRIVATE FrameLab::FrameLab)

# Native D3D12 SR/FG layout, constant and signature probe for the Rust D3D12 bindings. It uses the
# installed component headers when both are installed, otherwise this tree's copies; it references
# no FrameLab function and needs neither the components nor a GPU.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_executable(d3d12_abi_layout d3d12_abi_layout.c)
  target_compile_features(d3d12_abi_layout PRIVATE c_std_11)
  if(NOT (FrameLab_D3D12SR_FOUND AND FrameLab_D3D12FG_FOUND))
    target_include_directories(d3d12_abi_layout PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
  endif()
  target_include_directories(d3d12_abi_layout PRIVATE
    "$<TARGET_PROPERTY:FrameLab::FrameLab,INTERFACE_INCLUDE_DIRECTORIES>")
endif()

# GPU-free stand-in libraries for the Rust runtime-loader tests, all in one directory.
set(framelab_loader_fixtures
  "fixture_core\;"
  "fixture_wrong_abi\;FIXTURE_ABI_VERSION=2"
  "fixture_missing_core\;FIXTURE_OMIT_CORE_EXPORT"
  "fixture_sr\;FIXTURE_SR"
  "fixture_fg\;FIXTURE_FG"
  "fixture_sr_fg\;FIXTURE_SR\;FIXTURE_FG"
  "fixture_sr_partial\;FIXTURE_SR\;FIXTURE_SR_PARTIAL\;FIXTURE_FG")
foreach(fixture IN LISTS framelab_loader_fixtures)
  list(POP_FRONT fixture name)
  add_library(${name} SHARED loader_fixture.c)
  target_compile_definitions(${name} PRIVATE ${fixture})
  set_target_properties(${name} PROPERTIES PREFIX ""
    C_VISIBILITY_PRESET hidden
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader-fixtures$<0:>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader-fixtures$<0:>")
endforeach()
