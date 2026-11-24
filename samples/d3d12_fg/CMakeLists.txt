cmake_minimum_required(VERSION 3.20)
project(FrameLabD3D12Client LANGUAGES C CXX)
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12FG)

add_executable(d3d12_fg main.cpp)
target_compile_features(d3d12_fg PRIVATE cxx_std_20)
target_link_libraries(d3d12_fg PRIVATE FrameLab::FrameLab d3d12 dxgi)

add_executable(d3d12_header_test header_test.c)
target_compile_features(d3d12_header_test PRIVATE c_std_11)
target_link_libraries(d3d12_header_test PRIVATE FrameLab::FrameLab)

foreach(client IN ITEMS d3d12_fg d3d12_header_test)
  add_custom_command(TARGET ${client} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:${client}>"
    VERBATIM)
endforeach()

enable_testing()
add_test(NAME d3d12_c_public_header COMMAND d3d12_header_test)
