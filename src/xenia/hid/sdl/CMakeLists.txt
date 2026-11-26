add_library(xenia-hid-sdl STATIC)
xe_platform_sources(xenia-hid-sdl ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-sdl PUBLIC xenia-base xenia-hid xenia-ui SDL3)

# Rumble for controllers SDL can't drive, over ExternalAccessory. The *_ios.cc
# profiles are picked up by xe_platform_sources.
if(XE_PLATFORM_IOS)
  target_sources(xenia-hid-sdl PRIVATE ea_rumble_ios.mm)
  set_source_files_properties(ea_rumble_ios.mm PROPERTIES
    COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(xenia-hid-sdl PRIVATE
    "-framework ExternalAccessory" "-framework UIKit")
endif()

xe_target_defaults(xenia-hid-sdl)

# Bake the bundled controller mappings.
xe_embed_compressed_bundle(xenia-hid-sdl
    "${XENIA_DATA_REPOS_DIR}/SDL_GameControllerDB/gamecontrollerdb.txt"
    gamecontrollerdb)
