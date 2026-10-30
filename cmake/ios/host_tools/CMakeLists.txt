cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum host macOS version")
project(xenia-ios-host-tools LANGUAGES CXX)
if(NOT APPLE OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Configure the iOS host shader tool for native macOS.")
endif()

# Rebuild only the changed tool, using host libraries from the same dependency
# pins. This project never configures or writes into the supplied cache.
set(XENIA_HOST_LIBRARY_DIR "" CACHE PATH
    "Directory containing compatible macOS glslang and SPIRV-Tools archives")
get_filename_component(_xe_source_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_xe_host_libraries)
foreach(_xe_lib glslang SPIRV-Tools-opt SPIRV-Tools)
  set(_xe_archive "${XENIA_HOST_LIBRARY_DIR}/lib${_xe_lib}.a")
  if(NOT EXISTS "${_xe_archive}")
    message(FATAL_ERROR "Missing cached host archive: ${_xe_archive}")
  endif()
  list(APPEND _xe_host_libraries "${_xe_archive}")
endforeach()
add_executable(xenia-shader-cc "${_xe_source_root}/tools/build/shader_cc.cc")
target_compile_features(xenia-shader-cc PRIVATE cxx_std_20)
target_compile_definitions(xenia-shader-cc PRIVATE
  XE_SHADER_CC_METAL=1 XE_METAL_MIN_OS="${CMAKE_OSX_DEPLOYMENT_TARGET}")
target_include_directories(xenia-shader-cc PRIVATE
  "${_xe_source_root}/third_party/glslang"
  "${_xe_source_root}/third_party/glslang/StandAlone"
  "${_xe_source_root}/third_party/SPIRV-Tools/include")
target_link_libraries(xenia-shader-cc PRIVATE ${_xe_host_libraries})
