add_library(xenia-gpu-metal STATIC
  dxbc_to_dxil_converter.cc
  dxbc_to_dxil_converter.h
  ir_runtime_impl.mm
  metal_backend_telemetry.cc
  metal_backend_telemetry.h
  metal_command_processor.cc
  metal_command_processor.h
  metal_draw_batch.cc
  metal_direct_host_resolve.cc
  metal_geometry_shader.cc
  metal_geometry_shader.h
  metal_graphics_system.cc
  metal_graphics_system.h
  metal_heap_pool.cc
  metal_heap_pool.h
  metal_pipeline_cache.cc
  metal_pipeline_cache.h
  metal_primitive_processor.cc
  metal_primitive_processor.h
  metal_render_target_cache.cc
  metal_render_target_cache.h
  metal_shader.cc
  metal_shader.h
  metal_shader_cache.cc
  metal_shader_cache.h
  metal_shader_converter.cc
  metal_shader_converter.h
  metal_stage_compile_cache.cc
  metal_stage_compile_cache.h
  metal_shared_memory.cc
  metal_shared_memory.h
  metal_texture_cache.cc
  metal_texture_cache.h
  metal_upload_buffer_pool.cc
  metal_upload_buffer_pool.h
  metal_zpd_visibility_pool.cc
  metal_zpd_visibility_pool.h
  native_msl_bindings.cc
  native_msl_bindings.h
)
target_link_libraries(xenia-gpu-metal PUBLIC
  xenia-base
  xenia-gpu
  xenia-ui-metal
  xenia-third-party-dxilconv
  xenia-third-party-metal-shader-converter
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalKit"
)
xe_shader_rules_metal(xenia-gpu-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
if(APPLE)
  option(XE_METAL_SHADER_DEBUG_INFO
         "Embed MSL source + line tables in generated Metal metallibs so the compute/resolve shaders are viewable with per-line cost in the Xcode GPU trace (increases generated header/binary size)."
         ON)
  set(_metal_shader_args --msl)
  if(XE_METAL_SHADER_DEBUG_INFO)
    list(APPEND _metal_shader_args --metal-debug)
  endif()
  set(_direct_host_resolve_generated_root "${PROJECT_BINARY_DIR}/generated")
  set(_direct_host_resolve_bytecode_dir
      "${_direct_host_resolve_generated_root}/xenia/gpu/shaders/bytecode/metal")
  set(_direct_host_resolve_outputs)
  file(MAKE_DIRECTORY "${_direct_host_resolve_bytecode_dir}")

  macro(_xe_metal_direct_host_resolve_variant id input)
    set(_variant_define_args)
    foreach(_define ${ARGN})
      list(APPEND _variant_define_args --define "${_define}")
    endforeach()
    set(_variant_out "${_direct_host_resolve_bytecode_dir}/${id}.h")
    set(_variant_dep "${_variant_out}.d")
    list(APPEND _direct_host_resolve_outputs "${_variant_out}")
    add_custom_command(
      OUTPUT "${_variant_out}"
      COMMAND $<TARGET_FILE:xenia-shader-cc>
              ${_metal_shader_args}
              --identifier "${id}"
              ${_variant_define_args}
              --depfile "${_variant_dep}"
              "${input}"
              "${_variant_out}"
      DEPENDS "${input}" xenia-shader-cc
      DEPFILE "${_variant_dep}"
      COMMENT "Metal: ${id}"
      VERBATIM
    )
  endmacro()

  set(_resolve_host_color_entry
      "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/resolve_host_color_entry.xesli")
  set(_resolve_host_color_full_entry
      "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/resolve_host_color_full_entry.xesli")
  set(_resolve_host_depth_entry
      "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/resolve_host_depth_entry.xesli")

  foreach(_source_uint 0 1)
    foreach(_bpp 32 64)
      foreach(_msaa 1 2 4)
        foreach(_scaled 0 1)
          set(_id "resolve_host_color")
          if(_source_uint)
            string(APPEND _id "_uint")
          endif()
          string(APPEND _id "_${_bpp}bpp_${_msaa}xmsaa")
          set(_defines
              "XE_RESOLVE_HOST_COLOR_BPP=${_bpp}"
              "XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES=${_msaa}"
              "XE_RESOLVE_HOST_COLOR_SOURCE_UINT=${_source_uint}")
          if(_scaled)
            string(APPEND _id "_scaled")
            list(APPEND _defines "XE_RESOLVE_RESOLUTION_SCALED=1")
          endif()
          string(APPEND _id "_cs")
          _xe_metal_direct_host_resolve_variant(
              "${_id}" "${_resolve_host_color_entry}" ${_defines})
        endforeach()
      endforeach()
    endforeach()

    foreach(_bpp 8 16 32 64 128)
      foreach(_msaa 1 2 4)
        foreach(_scaled 0 1)
          set(_id "resolve_host_color_full")
          if(_source_uint)
            string(APPEND _id "_uint")
          endif()
          string(APPEND _id "_${_bpp}bpp_${_msaa}xmsaa")
          set(_defines
              "XE_RESOLVE_HOST_COLOR_FULL_DEST_BPP=${_bpp}"
              "XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES=${_msaa}"
              "XE_RESOLVE_HOST_COLOR_SOURCE_UINT=${_source_uint}")
          if(_scaled)
            string(APPEND _id "_scaled")
            list(APPEND _defines "XE_RESOLVE_RESOLUTION_SCALED=1")
          endif()
          string(APPEND _id "_cs")
          _xe_metal_direct_host_resolve_variant(
              "${_id}" "${_resolve_host_color_full_entry}" ${_defines})
        endforeach()
      endforeach()
    endforeach()
  endforeach()

  foreach(_msaa 1 2 4)
    foreach(_scaled 0 1)
      set(_id "resolve_host_depth_32bpp_${_msaa}xmsaa")
      set(_defines "XE_RESOLVE_HOST_DEPTH_MSAA_SAMPLES=${_msaa}")
      if(_scaled)
        string(APPEND _id "_scaled")
        list(APPEND _defines "XE_RESOLVE_RESOLUTION_SCALED=1")
      endif()
      string(APPEND _id "_cs")
      _xe_metal_direct_host_resolve_variant(
          "${_id}" "${_resolve_host_depth_entry}" ${_defines})
    endforeach()
  endforeach()

  add_custom_target(xenia-gpu-metal-direct-host-resolve-shaders
                    DEPENDS ${_direct_host_resolve_outputs})
  add_dependencies(xenia-gpu-metal
                   xenia-gpu-metal-direct-host-resolve-shaders)
  target_include_directories(xenia-gpu-metal BEFORE PRIVATE
                             "${_direct_host_resolve_generated_root}")

  set(_texture_upload_repack_shader
      "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/texture_upload_repack.metal")
  set(_texture_upload_repack_generated_root
      "${PROJECT_BINARY_DIR}/generated")
  set(_texture_upload_repack_out
      "${_texture_upload_repack_generated_root}/xenia/gpu/shaders/bytecode/metal/texture_upload_repack.h")
  set(_texture_upload_repack_dep "${_texture_upload_repack_out}.d")
  file(MAKE_DIRECTORY
       "${_texture_upload_repack_generated_root}/xenia/gpu/shaders/bytecode/metal")
  add_custom_command(
    OUTPUT "${_texture_upload_repack_out}"
    COMMAND $<TARGET_FILE:xenia-shader-cc>
            ${_metal_shader_args}
            --depfile "${_texture_upload_repack_dep}"
            "${_texture_upload_repack_shader}"
            "${_texture_upload_repack_out}"
    DEPENDS "${_texture_upload_repack_shader}" xenia-shader-cc
    DEPFILE "${_texture_upload_repack_dep}"
    COMMENT "Metal: texture_upload_repack.metal"
    VERBATIM
  )
  add_custom_target(xenia-gpu-metal-texture-upload-repack-shader
                    DEPENDS "${_texture_upload_repack_out}")
  add_dependencies(xenia-gpu-metal
                   xenia-gpu-metal-texture-upload-repack-shader)
  target_include_directories(xenia-gpu-metal BEFORE PRIVATE
                             "${_texture_upload_repack_generated_root}")
  set_source_files_properties("${_texture_upload_repack_shader}"
                              PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(xenia-gpu-metal PRIVATE "${_texture_upload_repack_shader}")
endif()
xe_target_defaults(xenia-gpu-metal)
