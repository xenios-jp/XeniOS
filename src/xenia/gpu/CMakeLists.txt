add_library(xenia-gpu STATIC)
xe_platform_sources(xenia-gpu ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xenia-gpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/glslang
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)
target_link_libraries(xenia-gpu PUBLIC fmt snappy xxhash xenia-base xenia-ui)
target_link_libraries(xenia-gpu PRIVATE glslang-spirv)
# spirv_to_dxil_compiler.cc wraps Mesa for D3D12 on Windows. dxbc is there
# for DXBCChecksum, which stamps the container digest of the produced DXIL.
if(WIN32)
  target_link_libraries(xenia-gpu PUBLIC spirv_to_dxil dxbc)
endif()
xe_target_defaults(xenia-gpu)

if(XENIA_BUILD_MISC)
  # Shader compiler tool
  add_executable(xenia-gpu-shader-compiler
    ${CMAKE_CURRENT_SOURCE_DIR}/shader_compiler_main.cc
  )
  if(WIN32)
    target_sources(xenia-gpu-shader-compiler PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(xenia-gpu-shader-compiler PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()
  target_include_directories(xenia-gpu-shader-compiler PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
    ${PROJECT_SOURCE_DIR}/third_party/glslang
  )
  # The shader translators only need xenia-gpu, but linking it drags in
  # command_processor and graphics_system, so the kernel side comes with it.
  target_link_libraries(xenia-gpu-shader-compiler PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-hid xenia-hid-nop xenia-hid-portal
    xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    capstone fmt glslang-spirv imgui libavcodec libavutil mspack
    snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-shader-compiler PRIVATE xenia-cpu-backend-x64)
  endif()
  if(XE_TARGET_AARCH64)
    target_link_libraries(xenia-gpu-shader-compiler PRIVATE xenia-cpu-backend-a64)
  endif()
  xe_target_defaults(xenia-gpu-shader-compiler)
endif()
