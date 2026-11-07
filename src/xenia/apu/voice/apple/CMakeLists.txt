# The headset on macOS and iOS (the voice processing I/O unit). Sources
# are listed because xe_platform_sources does not glob .mm files.
add_library(xenia-apu-voice-apple STATIC
  apple_voice_device.h
  apple_voice_device.mm
)
target_link_libraries(xenia-apu-voice-apple PUBLIC
  xenia-apu-voice
  xenia-base
  "-framework Foundation"
  "-framework AVFAudio"
  "-framework AudioToolbox"
)
if(NOT XE_PLATFORM_IOS)
  target_link_libraries(xenia-apu-voice-apple PUBLIC "-framework CoreAudio")
endif()
target_compile_options(xenia-apu-voice-apple PRIVATE
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
xe_target_defaults(xenia-apu-voice-apple)
