add_library(xenia-ui STATIC)
# miniaudio's iOS backend includes AVAudioSession Objective-C declarations.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/audio_helper.cc"
  PROPERTIES LANGUAGE OBJCXX)
xe_platform_sources(xenia-ui ${CMAKE_CURRENT_SOURCE_DIR})
get_target_property(_xe_ios_ui_sources xenia-ui SOURCES)
list(FILTER _xe_ios_ui_sources EXCLUDE REGEX
  "(_wx\\.(cc|h)|/wx_locale\\.(cc|h)|/icon_decode\\.(cc|h)|_demo\\.cc|/windowed_app_main_[^/]*\\.cc)$")
# The system files installer is wx only for its unzip, which iOS does itself
# (system_files_source_ios.cc).
list(APPEND _xe_ios_ui_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/system_files_installer_wx.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/system_files_installer_wx.h")
set_target_properties(xenia-ui PROPERTIES SOURCES "${_xe_ios_ui_sources}")
target_link_libraries(xenia-ui PUBLIC xenia-base)
xe_target_defaults(xenia-ui)
xe_embed_binary_assets(xenia-ui "${PROJECT_SOURCE_DIR}/assets/font" font)
xe_embed_binary_assets(xenia-ui "${PROJECT_SOURCE_DIR}/assets/icon" app_icon)
# The system files installer writes these.
xe_embed_binary_assets(xenia-ui "${PROJECT_SOURCE_DIR}/assets/system_font" system_font)
