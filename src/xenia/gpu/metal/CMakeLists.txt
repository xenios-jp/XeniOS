add_library(xenia-gpu-metal STATIC
  dxil_shader.cc
  dxil_shader.h
  ir_runtime_impl.cc
  metal_buffer_hazards.cc
  metal_buffer_hazards.h
  metal_command_processor.cc
  metal_command_processor.h
  metal_dxil_binder.cc
  metal_dxil_binder.h
  metal_graphics_system.cc
  metal_graphics_system.h
  metal_heap_pool.cc
  metal_heap_pool.h
  metal_primitive_processor.cc
  metal_primitive_processor.h
  metal_render_target_cache.cc
  metal_render_target_cache.h
  metal_shader_converter.cc
  metal_shader_converter.h
  metal_shared_memory.cc
  metal_shared_memory.h
  metal_tessellation_shaders.cc
  metal_tessellation_shaders.h
  metal_texture_cache.cc
  metal_texture_cache.h
  metal_transfer_spirv_cross.cc
  metal_transfer_spirv_cross.h
  metal_upload_cache.h
  metal_zpd_visibility_pool.cc
  metal_zpd_visibility_pool.h
  msl_bindings.h
  msl_shader.cc
  msl_shader.h
  msl_tess_factor_kernels.h
)
target_include_directories(xenia-gpu-metal PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/glslang
)
target_link_libraries(xenia-gpu-metal PUBLIC
  xenia-base
  xenia-gpu
  xenia-ui-metal
  spirv-cross
  xenia-third-party-metal-shader-converter
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalKit"
)
target_link_libraries(xenia-gpu-metal PRIVATE glslang-spirv)
# The tessellation path feeds the SPIR-V host vertex/hull shaders through Mesa,
# so it includes their vulkan_spirv headers, which the Vulkan backend generates.
# Depend on that generation so the headers exist on a clean build.
foreach(_spirv_shaders_dep
        xenia-gpu-vulkan-spirv-shaders xenia-gpu-vulkan-slang-spirv-shaders)
  if(TARGET ${_spirv_shaders_dep})
    add_dependencies(xenia-gpu-metal ${_spirv_shaders_dep})
  endif()
endforeach()
xe_shader_rules_metal(xenia-gpu-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
xe_shader_rules_slang(xenia-gpu-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                      TARGET msl)
xe_target_defaults(xenia-gpu-metal)

if(XENIA_BUILD_MISC)
  # Metal trace dump
  add_executable(xenia-gpu-metal-trace-dump
    ${CMAKE_CURRENT_SOURCE_DIR}/metal_trace_dump_main.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc
  )
  target_include_directories(xenia-gpu-metal-trace-dump PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/glslang
  )
  target_link_libraries(xenia-gpu-metal-trace-dump PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-metal xenia-hid xenia-hid-nop xenia-hid-portal
    xenia-kernel xenia-patcher xenia-ui xenia-ui-metal xenia-vfs
    capstone fmt glslang-spirv imgui libavcodec libavutil mspack snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-gpu-metal-trace-dump PRIVATE xenia-cpu-backend-x64)
  endif()
  if(XE_TARGET_AARCH64)
    target_link_libraries(xenia-gpu-metal-trace-dump PRIVATE xenia-cpu-backend-a64)
  endif()
  xe_target_defaults(xenia-gpu-metal-trace-dump)
endif()
