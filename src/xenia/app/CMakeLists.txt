if(XE_PLATFORM_IOS)
  set(_xe_ios_entitlements "${PROJECT_SOURCE_DIR}/xenia_ios.entitlements")
  # NOTE: The iOS asset catalog (XeniOSAssets.xcassets) is intentionally NOT
  # added to the build. actool's AssetCatalogAgent crashes compiling ANY image
  # (even a single plain PNG) under the current Xcode 27 beta / macOS 26.4
  # toolchain ("AVFCore ... missing from an installed root"), which fails the
  # CompileAssetCatalogVariant build phase. Compiling the catalog is disabled to
  # keep the build green; the app falls back to SF Symbols for the SettingsLink
  # icons and uses the default app icon. Re-add the catalog and the app-icon
  # settings below once the toolchain is fixed (released Xcode / newer macOS).
  add_executable(xenia-app MACOSX_BUNDLE
    ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main_ios.mm
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/ios/app/windowed_app_main_ios.mm
  )
  set_target_properties(xenia-app PROPERTIES
    OUTPUT_NAME "XeniOS"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info_ios.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "XeniOS"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.xenios"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.xenios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    # App-icon rendering is intentionally disabled: actool's AssetCatalogAgent
    # crashes rendering any app icon (PNG or Icon Composer .icon) under the
    # current Xcode/macOS toolchain. With no APPICON_NAME/ALTERNATE_APPICON_NAMES
    # designated, actool compiles the catalog without invoking that renderer, so
    # the build succeeds. Restore these settings once the toolchain is fixed:
    #   XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    #   XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_ALTERNATE_APPICON_NAMES "AppIconLight AppIconDark"
    #   XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_INCLUDE_ALL_APPICON_ASSETS YES
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
      "${_xe_ios_entitlements}")
  target_sources(xenia-app PRIVATE "${_xe_ios_entitlements}")
  # Asset catalog compilation disabled (see note above) — the catalog is not
  # added to xenia-app sources, so actool/CompileAssetCatalogVariant never runs.
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
    aes_128
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
    "-framework QuartzCore"
    "-framework Security"
    "-framework UIKit"
    "-framework UniformTypeIdentifiers"
  )
  if(XENIA_ENABLE_IOS_MOLTENVK)
    target_compile_definitions(xenia-app PRIVATE
      XE_IOS_MOLTENVK_ENABLED=1
    )
    target_link_libraries(xenia-app PRIVATE
      xenia-gpu-vulkan
      xenia-ui-vulkan
      glslang-spirv
    )
  endif()
  target_link_options(xenia-app PRIVATE
    "-Wl,-u,___clear_cache"
    "-Wl,-rpath,@executable_path/Frameworks"
    "-Wl,-rpath,@loader_path/Frameworks"
  )
  set(_xe_ios_embedded_dylibs)
  foreach(_xe_app_dylib xenia-third-party-metal-shader-converter
                        xenia-third-party-dxilconv)
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
    "${PROJECT_SOURCE_DIR}/build/data_repos/xenia-manager-database/data/game-compatibility"
    game_compat)
  set(_xe_ios_package_command
    /bin/bash "${PROJECT_SOURCE_DIR}/tools/package_ios_ipa.sh"
    "$<TARGET_BUNDLE_DIR:xenia-app>"
    "${_xe_ios_entitlements}"
    "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.ipa"
    "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.requested-entitlements.plist"
    "$<TARGET_BUNDLE_DIR:xenia-app>/../XeniOS.signed-entitlements.plist")
  if(XCODE)
    add_custom_target(xenia-app-ios-package
      COMMAND ${_xe_ios_package_command}
      DEPENDS xenia-app
      VERBATIM)
  else()
    add_custom_target(xenia-app-ios-package
      COMMAND "${CMAKE_COMMAND}" -E echo
              "xenia-app-ios-package requires the Xcode generator so Icon Composer .icon packages are compiled. Use: ./xb build --config=release --target-os=ios --target=xenia-app-ios-package"
      COMMAND "${CMAKE_COMMAND}" -E false
      DEPENDS xenia-app
      VERBATIM)
  endif()
  xe_target_defaults(xenia-app)
  return()
endif()

if(WIN32)
  add_executable(xenia-app WIN32)
elseif(APPLE)
  add_executable(xenia-app MACOSX_BUNDLE)
else()
  add_executable(xenia-app)
endif()

if(APPLE)
  # Drives the .app bundle directory name and the binary in Contents/MacOS/.
  set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "Xenia-edge")
else()
  set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "xenia_edge")
endif()

target_sources(xenia-app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc
)

xe_platform_sources(xenia-app ${CMAKE_CURRENT_SOURCE_DIR})

# Per-platform windowed entry point.
if(WIN32)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_resources.rc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/app_win32.manifest
  )
  if(XE_TARGET_X86_64)
    # main_init runs the AVX feature check — it must itself not use AVX.
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
      PROPERTIES COMPILE_OPTIONS "/arch:SSE2"
    )
  endif()
  # WIN32 executable defaults to /SUBSYSTEM:WINDOWS; override to CONSOLE
  # in Debug so stdout/stderr reach an attached terminal. Pin the entry
  # explicitly: wxIMPLEMENT_APP_NO_MAIN only emits wWinMain, so without
  # /ENTRY the CONSOLE subsystem looks for a missing main().
  target_link_options(xenia-app PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
    /ENTRY:wWinMainCRTStartup
  )
elseif(APPLE)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_mac.cc
  )
else()
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_linux.cc
  )
endif()

target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)

target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  aes_128
  capstone
  fmt
  dxbc
  discord-rpc
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
  xenia-app-discord
  xenia-apu-sdl
  xenia-debug-ui
  xenia-helper-sdl
  xenia-hid-sdl
)

# Vulkan/SPIR-V backends.
target_link_libraries(xenia-app PRIVATE
  xenia-gpu-vulkan
  xenia-ui-vulkan
  glslang-spirv
)

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-a64)
endif()

if(WIN32)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-debug-gdb
    xenia-gpu-d3d12
    xenia-hid-keyboard
    xenia-ui-d3d12
    uxtheme
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-app PRIVATE
    xenia-hid-keyboard
    X11
    xcb
    X11-xcb
    SDL3
  )
elseif(APPLE)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-phase
    xenia-gpu-metal
    xenia-hid-keyboard
    xenia-ui-metal
  )

  # macOS .app bundle: identity, Info.plist substitutions, AppIcon.icns,
  # rpath for frameworks alongside the executable, ad-hoc codesign with the
  # JIT entitlements so the binary can map writable+executable pages.
  #
  # iconutil only accepts a strictly-named .iconset directory, so stage one
  # in the build tree by copying assets/icon/<size>.png to the canonical
  # icon_<size>x<size>[@2x].png names.
  set(_xe_app_icon_src "${PROJECT_SOURCE_DIR}/assets/icon")
  set(_xe_app_iconset_dir "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
  set(_xe_app_icns "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
  set(_xe_app_iconset_pairs
    16   icon_16x16.png
    32   icon_16x16@2x.png
    32   icon_32x32.png
    64   icon_32x32@2x.png
    128  icon_128x128.png
    256  icon_128x128@2x.png
    256  icon_256x256.png
    512  icon_256x256@2x.png
    512  icon_512x512.png
    1024 icon_512x512@2x.png)
  set(_xe_app_iconset_copy_cmds
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_xe_app_iconset_dir}")
  set(_xe_app_iconset_deps "")
  while(_xe_app_iconset_pairs)
    list(POP_FRONT _xe_app_iconset_pairs _xe_size _xe_dst)
    list(APPEND _xe_app_iconset_copy_cmds
      COMMAND "${CMAKE_COMMAND}" -E copy
              "${_xe_app_icon_src}/${_xe_size}.png"
              "${_xe_app_iconset_dir}/${_xe_dst}")
    list(APPEND _xe_app_iconset_deps "${_xe_app_icon_src}/${_xe_size}.png")
  endwhile()
  add_custom_command(
    OUTPUT "${_xe_app_icns}"
    ${_xe_app_iconset_copy_cmds}
    COMMAND iconutil --convert icns
            --output "${_xe_app_icns}"
            "${_xe_app_iconset_dir}"
    DEPENDS ${_xe_app_iconset_deps}
    VERBATIM)
  target_sources(xenia-app PRIVATE "${_xe_app_icns}")
  set_source_files_properties("${_xe_app_icns}" PROPERTIES
    GENERATED TRUE
    MACOSX_PACKAGE_LOCATION Resources)

  set_target_properties(xenia-app PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "Xenia-edge"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.xenia.edge"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")

  target_link_options(xenia-app PRIVATE
    "-Wl,-rpath,@executable_path/../Frameworks"
    "-Wl,-rpath,@loader_path/../Frameworks")

  foreach(_xe_app_runtime_dylib
          xenia-third-party-metal-shader-converter
          xenia-third-party-dxilconv)
    if(TARGET ${_xe_app_runtime_dylib})
      add_custom_command(TARGET xenia-app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:xenia-app>/Contents/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${_xe_app_runtime_dylib}>"
                "$<TARGET_BUNDLE_DIR:xenia-app>/Contents/Frameworks/$<TARGET_FILE_NAME:${_xe_app_runtime_dylib}>"
        VERBATIM)
    endif()
  endforeach()
endif()

xe_target_defaults(xenia-app)

# Sign the bundle last, after every POST_BUILD step that drops files into
# it — otherwise Gatekeeper rejects the downloaded artifact with "a sealed
# resource is missing or invalid".
if(APPLE)
  add_custom_command(TARGET xenia-app POST_BUILD
    COMMAND codesign --force --deep --sign -
            --entitlements "${PROJECT_SOURCE_DIR}/xenia.entitlements"
            "$<TARGET_BUNDLE_DIR:xenia-app>"
    # Strip macos-26 provenance/macl xattrs so AMFI on macOS 15 accepts the seal.
    COMMAND xattr -cr "$<TARGET_BUNDLE_DIR:xenia-app>"
    VERBATIM)
endif()

# Bake icons into the app binary (font lives with xenia-ui).
xe_embed_binary_assets(xenia-app
    "${PROJECT_SOURCE_DIR}/assets/icons" icons)

# Bake game-compat data (canary.json from xenia-manager/database).
xe_embed_compressed_bundle(xenia-app
    "${PROJECT_SOURCE_DIR}/build/data_repos/xenia-manager-database/data/game-compatibility"
    game_compat)
target_include_directories(xenia-app PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")
