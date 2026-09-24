# xenia-cpu-replay: compiles a JIT corpus recorded with --jit_corpus_out.
add_executable(xenia-cpu-replay
  ${CMAKE_CURRENT_SOURCE_DIR}/cpu_replay_main.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/invocation_replay.cc
)
if(WIN32)
  target_sources(xenia-cpu-replay PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
else()
  target_sources(xenia-cpu-replay PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
endif()
target_link_libraries(xenia-cpu-replay PRIVATE
  xenia-apu xenia-apu-nop
  xenia-base xenia-core xenia-cpu
  xenia-gpu xenia-gpu-null
  xenia-hid xenia-hid-nop
  xenia-kernel xenia-patcher xenia-ui xenia-vfs
  capstone fmt imgui mspack
)
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-cpu-replay PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-cpu-replay PRIVATE xenia-cpu-backend-a64)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-cpu-replay PRIVATE xenia-apu-sdl)
endif()
xe_target_defaults(xenia-cpu-replay)
