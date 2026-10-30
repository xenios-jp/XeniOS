add_library(xenia-hid-touch-model STATIC)

target_sources(xenia-hid-touch-model PRIVATE
  touch_layout_editor.cc
  touch_layout_editor.h
  touch_input_resolver.cc
  touch_input_resolver.h
  touch_layout_ios.cc
  touch_layout_ios_codec.cc
  touch_layout_ios_defaults.cc
  touch_layout_ios_geometry.cc
  touch_layout_ios.h
  touch_layout_ios_internal.h
  touch_layout_ios_labels.cc
  touch_layout_ios_runtime.cc
)

target_link_libraries(xenia-hid-touch-model PUBLIC
  xenia-base
  xenia-hid
)

xe_target_defaults(xenia-hid-touch-model)

if(XE_PLATFORM_IOS)
  add_library(xenia-hid-touch STATIC)

  target_sources(xenia-hid-touch PRIVATE
    touch_hid_ios.cc
    touch_hid_ios.h
    touch_input_driver_ios.cc
    touch_input_driver_ios.h
  )

  target_link_libraries(xenia-hid-touch PUBLIC
    xenia-hid
    xenia-hid-sdl
    xenia-hid-touch-model
  )

  xe_target_defaults(xenia-hid-touch)
endif()

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
endif()
