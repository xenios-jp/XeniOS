set(_xe_ios_entitlements "${PROJECT_SOURCE_DIR}/xenia_ios.entitlements")
set(XENIOS_BUNDLE_IDENTIFIER "com.xenios" CACHE STRING "XeniOS bundle identifier")
set(XENIOS_VERSION "" CACHE STRING
    "CFBundleShortVersionString; empty takes the newest v<version> tag reachable from HEAD")
set(XENIOS_BUILD_NUMBER "" CACHE STRING
    "CFBundleVersion; empty takes the commit count of HEAD")
set(XENIA_IOS_COMPATIBILITY_DATA_DIR
    "${XENIA_DATA_REPOS_DIR}/xenia-manager-database/data/game-compatibility"
    CACHE PATH "Existing native launcher compatibility data")
foreach(_xe_ios_data
    "${XENIA_DATA_REPOS_DIR}/SDL_GameControllerDB/gamecontrollerdb.txt"
    "${XENIA_DATA_REPOS_DIR}/game-patches/patches"
    "${XENIA_IOS_COMPATIBILITY_DATA_DIR}")
  if(NOT EXISTS "${_xe_ios_data}")
    message(FATAL_ERROR "Missing iOS bundled data: ${_xe_ios_data}")
  endif()
endforeach()
# Compile the asset catalog for both Xcode and command-line builds.
add_executable(xenia-app MACOSX_BUNDLE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main_ios.mm
  ${PROJECT_SOURCE_DIR}/src/xenia/ui/ios/app/windowed_app_main_ios.mm
)
set_target_properties(xenia-app PROPERTIES
  OUTPUT_NAME "XeniOS"
  SKIP_BUILD_RPATH TRUE
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info_ios.plist"
  MACOSX_BUNDLE_BUNDLE_NAME "XeniOS"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${XENIOS_BUNDLE_IDENTIFIER}"
  # Placeholders: stamp_ios_version.py writes the real values after linking.
  MACOSX_BUNDLE_BUNDLE_VERSION "1"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "0.0.0"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${XENIOS_BUNDLE_IDENTIFIER}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
    "${_xe_ios_entitlements}")
target_sources(xenia-app PRIVATE "${_xe_ios_entitlements}")
target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)
target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-apu-phase
  xenia-apu-sdl
  xenia-base
  xenia-core
  xenia-cpu
  xenia-cpu-backend-a64
  xenia-gpu
  xenia-gpu-metal
  xenia-helper-sdl
  xenia-hid
  xenia-hid-nop
  xenia-hid-sdl
  xenia-hid-touch
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-ui-ios
  xenia-ui-metal
  xenia-vfs
  capstone
  dxbc
  fmt
  imgui
  libavcodec
  libavformat
  libavutil
  mspack
  snappy
  xxhash
  "-framework AVFoundation"
  "-framework AudioToolbox"
  "-framework CoreFoundation"
  "-framework Foundation"
  "-framework GameController"
  "-framework Metal"
  "-framework MetalKit"
  "-framework MetricKit"
  "-framework QuartzCore"
  "-framework Security"
  "-framework UIKit"
  "-framework UniformTypeIdentifiers"
)
target_link_options(xenia-app PRIVATE
  "-Wl,-u,___clear_cache"
  "-Wl,-rpath,@executable_path/Frameworks"
  "-Wl,-rpath,@loader_path/Frameworks"
)
set(_xe_ios_embedded_dylibs)
foreach(_xe_app_dylib xenia-third-party-metal-shader-converter)
  if(TARGET ${_xe_app_dylib})
    if(XCODE)
      get_target_property(_xe_app_dylib_path ${_xe_app_dylib}
                          IMPORTED_LOCATION_RELEASE)
      if(NOT _xe_app_dylib_path)
        get_target_property(_xe_app_dylib_path ${_xe_app_dylib}
                            IMPORTED_LOCATION)
      endif()
      if(_xe_app_dylib_path)
        list(APPEND _xe_ios_embedded_dylibs "${_xe_app_dylib_path}")
      endif()
    else()
      add_custom_command(TARGET xenia-app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:xenia-app>/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${_xe_app_dylib}>"
                "$<TARGET_BUNDLE_DIR:xenia-app>/Frameworks/$<TARGET_FILE_NAME:${_xe_app_dylib}>"
        VERBATIM)
    endif()
  endif()
endforeach()
if(XCODE AND _xe_ios_embedded_dylibs)
  set_target_properties(xenia-app PROPERTIES
    XCODE_EMBED_FRAMEWORKS "${_xe_ios_embedded_dylibs}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES)
endif()
target_include_directories(xenia-app PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include"
)
xe_embed_binary_assets(xenia-app "${PROJECT_SOURCE_DIR}/assets/icons" icons)
xe_embed_compressed_bundle(xenia-app
  "${XENIA_IOS_COMPATIBILITY_DATA_DIR}"
  game_compat)
# Version from the newest release tag and build number from the commit count, so
# every build of a commit carries the same, ever-growing build number.
set(_xe_ios_stamp_command
  "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build/stamp_ios_version.py"
  "$<TARGET_BUNDLE_DIR:xenia-app>" "${PROJECT_SOURCE_DIR}"
  "--version=${XENIOS_VERSION}" "--build=${XENIOS_BUILD_NUMBER}")
set(_xe_ios_package_command
  /bin/bash "${PROJECT_SOURCE_DIR}/tools/package_ios_ipa.sh"
  "$<TARGET_BUNDLE_DIR:xenia-app>"
  "${_xe_ios_entitlements}"
  "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.ipa"
  "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.requested-entitlements.plist"
  "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.signed-entitlements.plist")
include(${PROJECT_SOURCE_DIR}/cmake/ios/Assets.cmake)
add_custom_command(TARGET xenia-app POST_BUILD COMMAND ${_xe_ios_stamp_command} VERBATIM)
# CMake rewrites the bundle's Info.plist from the template whenever it regenerates,
# dropping what the post-build steps added, so packaging adds it again.
add_custom_target(xenia-app-ios-package
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build/merge_ios_assets.py"
    "${_xe_ios_assets}" "$<TARGET_BUNDLE_DIR:xenia-app>"
  COMMAND ${_xe_ios_stamp_command}
  COMMAND ${_xe_ios_package_command}
  DEPENDS xenia-app
  VERBATIM)
xe_target_defaults(xenia-app)
