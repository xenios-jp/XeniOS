# iPhoneOS is a cross target even on an arm64 Mac. Keep all generated files
# in this build tree and never configure the source or a shared host cache.
set(XENIA_MESA_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/mesa" CACHE PATH
    "Mesa source tree built for spirv_to_dxil")
if(NOT EXISTS "${XENIA_MESA_SOURCE_DIR}/meson.build")
  message(FATAL_ERROR "Missing pinned Mesa source: ${XENIA_MESA_SOURCE_DIR}")
endif()
# Build from the same generated overlay as macOS, so spirv_to_dxil keeps the
# explicit 32-bit multiply-adds that MSC needs for elided zero-multiply guards
# (XE_MESA_METAL_FMAD32). The pinned submodule stays unchanged.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_xe_mesa_prepare "${PROJECT_SOURCE_DIR}/tools/build/prepare_mesa_metal.py")
set(_xe_mesa_source "${CMAKE_BINARY_DIR}/third_party/mesa-metal-source")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_xe_mesa_prepare}")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${_xe_mesa_prepare}"
    "${XENIA_MESA_SOURCE_DIR}" "${_xe_mesa_source}"
  RESULT_VARIABLE _xe_mesa_prepare_result
  OUTPUT_QUIET ERROR_VARIABLE _xe_mesa_prepare_error)
if(NOT _xe_mesa_prepare_result EQUAL 0)
  message(FATAL_ERROR "Mesa Metal source preparation failed: ${_xe_mesa_prepare_error}")
endif()
find_program(XE_MESON_EXECUTABLE meson REQUIRED)
find_program(XE_NINJA_EXECUTABLE ninja REQUIRED)
execute_process(COMMAND xcrun --sdk iphoneos --show-sdk-path
  OUTPUT_VARIABLE _xe_ios_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
set(_xe_mesa_build "${CMAKE_BINARY_DIR}/third_party/mesa-ios-build")
set(_xe_mesa_cross "${CMAKE_CURRENT_BINARY_DIR}/mesa-ios-cross.ini")
file(CONFIGURE OUTPUT "${_xe_mesa_cross}" CONTENT
"[binaries]
c = ['@CMAKE_C_COMPILER@', '-target', 'arm64-apple-ios@CMAKE_OSX_DEPLOYMENT_TARGET@', '-isysroot', '@_xe_ios_sdk@']
cpp = ['@CMAKE_CXX_COMPILER@', '-target', 'arm64-apple-ios@CMAKE_OSX_DEPLOYMENT_TARGET@', '-isysroot', '@_xe_ios_sdk@']
objc = ['@CMAKE_OBJC_COMPILER@', '-target', 'arm64-apple-ios@CMAKE_OSX_DEPLOYMENT_TARGET@', '-isysroot', '@_xe_ios_sdk@']
ar = '@CMAKE_AR@'
strip = '@CMAKE_STRIP@'
pkg-config = '/usr/bin/false'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
" @ONLY)
set(_xe_mesa_archives
  src/microsoft/spirv_to_dxil/libspirv_to_dxil.a
  src/microsoft/compiler/libdxil_compiler.a
  src/compiler/spirv/libvtn.a
  src/compiler/nir/libnir.a
  src/compiler/libcompiler.a
  src/util/libmesa_util.a
  src/util/libmesa_util_simd.a
  src/c11/impl/libmesa_util_c11.a
  src/util/blake3/libblake3.a)
set(_xe_mesa_outputs ${_xe_mesa_archives})
list(TRANSFORM _xe_mesa_outputs PREPEND "${_xe_mesa_build}/")
include(ExternalProject)
ExternalProject_Add(mesa_spirv_to_dxil
  SOURCE_DIR "${_xe_mesa_source}"
  BINARY_DIR "${_xe_mesa_build}"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
  CONFIGURE_COMMAND "${XE_MESON_EXECUTABLE}" setup --reconfigure
    "${_xe_mesa_build}" "${_xe_mesa_source}"
    --cross-file "${_xe_mesa_cross}" --wrap-mode=nodownload
    --default-library=static --buildtype=release
    -Dspirv-to-dxil=true -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=
    -Dglx=disabled -Degl=disabled -Dgbm=disabled
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dllvm=disabled -Dvideo-codecs= -Dzstd=disabled -Dlibunwind=disabled
    -Dexpat=disabled -Dshader-cache=disabled
  BUILD_COMMAND "${XE_NINJA_EXECUTABLE}" -C "${_xe_mesa_build}" -j4 ${_xe_mesa_archives}
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${_xe_mesa_outputs}
    "${_xe_mesa_source}/xenia-metal/xenia_mesa_metal_version.h"
  BUILD_ALWAYS ON USES_TERMINAL_BUILD ON)
list(POP_FRONT _xe_mesa_outputs _xe_mesa_main)
add_library(spirv_to_dxil STATIC IMPORTED GLOBAL)
add_dependencies(spirv_to_dxil mesa_spirv_to_dxil)
set_target_properties(spirv_to_dxil PROPERTIES
  IMPORTED_LOCATION "${_xe_mesa_main}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${_xe_mesa_source}/src/microsoft/spirv_to_dxil;${_xe_mesa_source}/src/microsoft/compiler;${_xe_mesa_source}/xenia-metal"
  INTERFACE_COMPILE_DEFINITIONS XE_MESA_METAL_FMAD32=1
  INTERFACE_LINK_LIBRARIES "${_xe_mesa_outputs};z")
