add_library(xenia-ui-metal STATIC
  metal_api.cc
  metal_api.h
  metal_gpu_completion_timeline.cc
  metal_gpu_completion_timeline.h
  metal_immediate_drawer.h
  metal_immediate_drawer.mm
  metal_presenter.h
  metal_presenter.mm
  metal_provider.cc
  metal_provider.h
  metal_upload_buffer_pool.cc
  metal_upload_buffer_pool.h
)
target_link_libraries(xenia-ui-metal PUBLIC
  xenia-base
  xenia-ui
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalKit"
  "-framework MetalFX"
  "-framework QuartzCore"
  "$<$<NOT:$<BOOL:${XE_PLATFORM_IOS}>>:-framework AppKit>"
)
if(XE_PLATFORM_IOS)
  target_sources(xenia-ui-metal PRIVATE
    ../ios/game/metal_presenter_surface_ios.mm)
endif()
xe_shader_rules_metal(xenia-ui-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
xe_shader_rules_slang(xenia-ui-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                      TARGET msl)
xe_target_defaults(xenia-ui-metal)
