# Apple PHASE spatial-audio backend (iOS + macOS). Sources are listed
# explicitly because xe_platform_sources does not glob .mm files.
add_library(xenia-apu-phase STATIC
  phase_audio_driver.h
  phase_audio_driver.mm
  phase_audio_system.cc
  phase_audio_system.h
)
target_link_libraries(xenia-apu-phase PUBLIC
  xenia-apu
  xenia-base
  "-framework Foundation"
  "-framework AVFoundation"
  "-framework PHASE"
  "-framework CoreMotion"
)
xe_target_defaults(xenia-apu-phase)
