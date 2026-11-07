xe_test_suite(xenia-apu-voice-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS fmt xenia-apu-voice xenia-base xenia-net
)
if(APPLE)
  # Apple's Opus in the party voice test.
  target_link_libraries(xenia-apu-voice-tests PRIVATE xenia-apu-voice-apple)
endif()
