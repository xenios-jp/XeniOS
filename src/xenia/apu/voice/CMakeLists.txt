# The emulated Xbox 360 headset's host side: the G.726 codec, sample rings and
# the device interface the app's audio backend plugs into; and party voice.
add_library(xenia-apu-voice STATIC)
xe_platform_sources(xenia-apu-voice ${CMAKE_CURRENT_SOURCE_DIR})
# Party voice travels in a relay room (xenia-net).
target_link_libraries(xenia-apu-voice PUBLIC xenia-base xenia-net)
xe_target_defaults(xenia-apu-voice)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
