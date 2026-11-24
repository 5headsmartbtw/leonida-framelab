include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(FrameLab src/core/fg_stream.cpp)
add_library(FrameLab::FrameLab ALIAS FrameLab)
target_compile_features(FrameLab PRIVATE cxx_std_20)
target_include_directories(FrameLab
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${PROJECT_SOURCE_DIR}/fg"
)
set_target_properties(FrameLab PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
get_target_property(framelab_type FrameLab TYPE)
if(framelab_type STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(FrameLab PUBLIC FRAMELAB_SHARED PRIVATE FRAMELAB_BUILDING)
  set_target_properties(FrameLab PROPERTIES VERSION "${PROJECT_VERSION}" SOVERSION 1)
endif()

set(framelab_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/FrameLab")
configure_package_config_file(cmake/FrameLabConfig.cmake.in
  "${PROJECT_BINARY_DIR}/FrameLabConfig.cmake" INSTALL_DESTINATION "${framelab_package_dir}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/FrameLabConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)

install(TARGETS FrameLab EXPORT FrameLabTargets)
install(DIRECTORY include/framelab DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT FrameLabTargets NAMESPACE FrameLab:: DESTINATION "${framelab_package_dir}")
install(FILES "${PROJECT_BINARY_DIR}/FrameLabConfig.cmake" "${PROJECT_BINARY_DIR}/FrameLabConfigVersion.cmake"
  DESTINATION "${framelab_package_dir}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/FrameLab")
