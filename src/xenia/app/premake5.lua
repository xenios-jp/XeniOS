project_root = "../../.."
local metal_converter_libdir =
    path.getabsolute(path.join(project_root, "third_party/metal-shader-converter/lib"))
local dxilconv_libdir_arm64 =
    path.getabsolute(path.join(project_root,
                               "third_party/DirectXShaderCompiler/build_dxilconv_macos/lib"))
local dxilconv_libdir_x86_64 =
    path.getabsolute(path.join(project_root,
                               "third_party/DirectXShaderCompiler/build_dxilconv_macos_x86_64/lib"))
include(project_root.."/tools/build")

-- iOS builds use a native UIKit entry point instead of the Qt-based one.

group("src")
project("xenia-app")
  uuid("d7e98620-d007-4ad8-9dbd-b47c8853a17f")
  language("C++")
  xcodebuildresources({
    "**/*.xcassets",
  })
  links({
    "xenia-apu",
    "xenia-apu-nop",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-gpu",
    "xenia-gpu-null",
    "xenia-hid",
    "xenia-hid-nop",
    "xenia-kernel",
    "xenia-patcher",
    "xenia-ui",
    "xenia-vfs",
  })
  links({
    "aes_128",
    "capstone",
    "fmt",
    "dxbc",
    "discord-rpc",
    "glslang-spirv",
    "imgui",
    "libavcodec",
    "libavutil",
    "mspack",
    "snappy",
    "xxhash",
  })
  defines({
    "XBYAK_NO_OP_NAMES",
    "XBYAK_ENABLE_OMITTED_OPERAND",
  })
  apu_transitive_deps()
  local_platform_files()
  files({
    "../base/main_init_"..platform_suffix..".cc",
  })
  filter("not system:ios")
    files({
      "../ui/windowed_app_main_qt.cc",
    })
  filter("system:ios")
    files({
      "../ui/windowed_app_main_ios.mm",
      "xenia_main_ios.mm",
    })
    -- iOS uses native UIKit, not the Qt-based desktop emulator window.
    removefiles({
      "emulator_window.cc",
      "emulator_window.h",
      "xenia_main.cc",
    })
  filter({})

  resincludedirs({
    project_root,
  })

  filter(SINGLE_LIBRARY_FILTER)
    -- Unified library containing all apps as StaticLibs, not just the main
    -- emulator windowed app.
    kind("SharedLib")
  if enableMiscSubprojects then
      links({
        "xenia-gpu-vulkan-trace-viewer",
        "xenia-hid-demo",
        "xenia-ui-window-vulkan-demo",
      })
  end
  filter(NOT_SINGLE_LIBRARY_FILTER)
    kind("WindowedApp")
  filter({NOT_SINGLE_LIBRARY_FILTER, "platforms:Windows-*", "configurations:Debug"})
    kind("ConsoleApp")

  -- `targetname` is broken if building from Gradle, works only for toggling the
  -- `lib` prefix, as Gradle uses LOCAL_MODULE_FILENAME, not a derivative of
  -- LOCAL_MODULE, to specify the targets to build when executing ndk-build.
  filter("platforms:not Android-*")
    targetname("xenios")

  filter("architecture:x86_64")
    links({
      "xenia-cpu-backend-x64",
    })

  -- TODO(Triang3l): The emulator itself on Android.
  -- iOS uses a native UIKit entry point, not xenia_main.cc.
  filter({"platforms:not Android-*", "not system:ios"})
    files({
      "xenia_main.cc",
    })

  filter("platforms:Windows-*")
    files({
      "main_resources.rc",
    })
    linkoptions({"/ENTRY:mainCRTStartup"})

  filter({"not system:macosx", "not system:ios"})
    links({
      "xenia-gpu-vulkan",
      "xenia-ui-vulkan",
    })

  filter({"architecture:x86_64", "files:../base/main_init_"..platform_suffix..".cc"})
    vectorextensions("SSE2")  -- Disable AVX for main_init_win.cc so our AVX check doesn't use AVX instructions.

  filter({"platforms:not Android-*", "not system:ios"})
    links({
      "xenia-app-discord",
      "xenia-apu-sdl",
      -- TODO(Triang3l): CPU debugger on Android.
      "xenia-debug-ui",
      "xenia-helper-sdl",
      "xenia-hid-sdl",
    })

  -- iOS: use SDL for audio and input (built from source as static lib).
  filter("system:ios")
    links({
      "xenia-apu-sdl",
      "xenia-helper-sdl",
      "xenia-hid-sdl",
    })

  filter({"platforms:not Android-*", "architecture:ARM64"})
    links({
      "xenia-cpu-backend-a64",
    })

  filter("platforms:Linux-*")
    links({
      "xenia-apu-alsa",
      "X11",
      "xcb",
      "X11-xcb",
      "SDL2",
    })

  filter("platforms:Windows-*")
    links({
      "xenia-apu-xaudio2",
      "xenia-debug-gdb",
      "xenia-gpu-d3d12",
      "xenia-hid-winkey",
      "xenia-hid-xinput",
      "xenia-ui-d3d12",
    })
    includedirs({
      project_root.."/third_party/DirectXShaderCompiler/include",
    })

  filter("platforms:Windows-*")

  filter("system:macosx")
    xcodebuildsettings({
      ["INFOPLIST_FILE"] = path.getabsolute("Info.plist"),
      ["CODE_SIGN_ENTITLEMENTS"] =
          path.getabsolute(path.join(project_root, "xenia.entitlements")),
      ["LD_RUNPATH_SEARCH_PATHS"] =
          "@executable_path/../Frameworks @loader_path/../Frameworks",
    })
    links({
      "xenia-gpu-metal",
      "xenia-ui-metal",
      "dxilconv",
      "metalirconverter",
      "Cocoa.framework",
      "CoreFoundation.framework",
      "CoreServices.framework",
      "Foundation.framework",
      "Metal.framework",
      "MetalFX.framework",
      "MetalKit.framework",
      "QuartzCore.framework",
      "SDL2",
      "iconv",
    })
    linkoptions({
      "-ldxilconv",
      "-lLLVMDxcSupport",
    })
    if _OPTIONS["mac-x86_64"] then
      libdirs({
        metal_converter_libdir,
        "/usr/local/opt/sdl2/lib",
      })
    else
      libdirs({
        metal_converter_libdir,
        "/opt/homebrew/opt/sdl2/lib",
      })
    end
  filter({"system:macosx", "architecture:arm64"})
    libdirs({ dxilconv_libdir_arm64 })
  filter({"system:macosx", "architecture:x86_64"})
    libdirs({ dxilconv_libdir_x86_64 })
    linkoptions({
      "-Wl,-pagezero_size,0x1000",
      path.getabsolute(path.join(dxilconv_libdir_x86_64, "libdxilconv.dylib")),
    })
  filter({})

  -- iOS app configuration (SPIRV-Cross path + SDL audio/input).
  filter("system:ios")
    local ios_entitlements_path =
        path.getabsolute(path.join(project_root, "xenia_ios.entitlements"))
    files({
      project_root.."/xenia_ios.entitlements",
      project_root.."/assets/apple/AppIcon.xcassets",
    })
    links({
      "xenia-gpu-metal",
      "xenia-ui-metal",
      "spirv-cross",
      "metal-cpp",
      "SDL2",
      "iconv",
      "CoreFoundation.framework",
      "Foundation.framework",
      "Metal.framework",
      "MetalKit.framework",
      "PhotosUI.framework",
      "QuartzCore.framework",
      "UIKit.framework",
      "UniformTypeIdentifiers.framework",
      -- SDL audio (CoreAudio backend).
      "CoreAudio.framework",
      "AudioToolbox.framework",
      "AVFoundation.framework",
      -- SDL input (MFi/GameController backend).
      "GameController.framework",
      "CoreMotion.framework",
      "CoreHaptics.framework",
      "CoreGraphics.framework",
      "CoreBluetooth.framework",
    })
    xcodebuildsettings({
      ["INFOPLIST_FILE"] = path.getabsolute("Info_ios.plist"),
      ["IPHONEOS_DEPLOYMENT_TARGET"] = "17.0",
      ["SDKROOT"] = "iphoneos",
      ["TARGETED_DEVICE_FAMILY"] = "1,2",
      ["PRODUCT_NAME"] = "XeniOS",
      ["EXECUTABLE_NAME"] = "xenios",
      ["PRODUCT_BUNDLE_IDENTIFIER"] = "jp.xenios.xenios.ios",
      ["ASSETCATALOG_COMPILER_APPICON_NAME"] = "AppIcon",
      ["CODE_SIGN_STYLE"] = "Automatic",
      ["CODE_SIGN_ENTITLEMENTS"] = ios_entitlements_path,
      ["CODE_SIGN_ALLOW_ENTITLEMENTS_MODIFICATION"] = "YES",
    })
    local ios_lldbinit_installer =
        path.getabsolute(path.join(project_root, "tools", "ios", "install_lldbinit_xenios_jit.sh"))
    local ios_achievement_sound =
        path.getabsolute(path.join(project_root, "assets", "apple", "achievement_unlocked_360.mp3"))
    buildinputs({
      ios_achievement_sound,
      ios_lldbinit_installer,
    })
    buildoutputs({
      "$(TARGET_BUILD_DIR)/$(WRAPPER_NAME)/achievement_unlocked_360.mp3",
    })
    postbuildcommands({
      'rm -f "${TARGET_BUILD_DIR}/${WRAPPER_NAME}/xbox360_startup.mp4"',
      'cp -f "' .. ios_achievement_sound .. '" "${TARGET_BUILD_DIR}/${WRAPPER_NAME}/achievement_unlocked_360.mp3"',
      'if [ -z "${XENIA_SKIP_LLDBINIT_INSTALL}" ]; then /bin/bash "'
          .. ios_lldbinit_installer .. '"; fi',
    })
  filter({})

  if enableMiscSubprojects then
    filter({"platforms:Windows-*", SINGLE_LIBRARY_FILTER})
      links({
        "xenia-gpu-d3d12-trace-viewer",
        "xenia-ui-window-d3d12-demo",
      })
  end

  filter("platforms:Windows-*")
    -- Only create the .user file if it doesn't already exist.
    local user_file = project_root.."/build/xenia-app.vcxproj.user"
    if not os.isfile(user_file) then
      debugdir(project_root)
    end

  -- Run windeployqt as post-build event to copy Qt DLLs
  filter({"platforms:Windows-*", "configurations:Debug or Checked"})
    local qt_dir = os.getenv("QT_DIR")
    if qt_dir then
      local windeployqt = path.translate(path.join(qt_dir, "bin", "windeployqt.exe"), "\\")
      postbuildcommands {
        'if exist "' .. windeployqt .. '" "' .. windeployqt .. '" --debug --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime "$(TargetPath)"'
      }
    end

  filter({"platforms:Windows-*", "configurations:Release"})
    local qt_dir = os.getenv("QT_DIR")
    if qt_dir then
      local windeployqt = path.translate(path.join(qt_dir, "bin", "windeployqt.exe"), "\\")
      postbuildcommands {
        'if exist "' .. windeployqt .. '" "' .. windeployqt .. '" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime "$(TargetPath)"'
      }
    end

  -- Copy optimized-settings JSON files next to executable
  filter("platforms:Windows-*")
    -- Use absolute path to avoid issues with relative paths
    local optimized_settings_src = path.translate(path.getabsolute(path.join(project_root, ".data_repos", "optimized-settings", "settings")), "\\")
    postbuildcommands {
      'if not exist "$(TargetDir)optimized_settings" mkdir "$(TargetDir)optimized_settings"',
      'xcopy /I /Y /Q "' .. optimized_settings_src .. '\\*.json" "$(TargetDir)optimized_settings\\"'
    }

  -- Copy game-patches TOML files next to executable
  filter("platforms:Windows-*")
    local game_patches_src = path.translate(path.getabsolute(path.join(project_root, ".data_repos", "game-patches", "patches")), "\\")
    postbuildcommands {
      'if not exist "$(TargetDir)game_patches" mkdir "$(TargetDir)game_patches"',
      'xcopy /I /Y /Q "' .. game_patches_src .. '\\*.toml" "$(TargetDir)game_patches\\"'
    }

  -- Copy assets/font next to executable
  filter("platforms:Windows-*")
    local assets_font_src = path.translate(path.getabsolute(path.join(project_root, "assets", "font")), "\\")
    postbuildcommands {
      'if not exist "$(TargetDir)assets\\font" mkdir "$(TargetDir)assets\\font"',
      'xcopy /I /Y /Q "' .. assets_font_src .. '\\*.*" "$(TargetDir)assets\\font\\"'
    }

  filter("platforms:Linux-*")
    local optimized_settings_src = path.getabsolute(path.join(project_root, ".data_repos", "optimized-settings", "settings"))
    local optimized_settings_dst = path.getabsolute(path.join(project_root, "build", "bin", "Linux")) .. "/%{cfg.buildcfg}/optimized_settings"
    postbuildcommands {
      '{MKDIR} ' .. optimized_settings_dst,
      '{COPY} ' .. optimized_settings_src .. '/*.json ' .. optimized_settings_dst
    }

  filter("platforms:Linux-*")
    local game_patches_src = path.getabsolute(path.join(project_root, ".data_repos", "game-patches", "patches"))
    local game_patches_dst = path.getabsolute(path.join(project_root, "build", "bin", "Linux")) .. "/%{cfg.buildcfg}/game_patches"
    postbuildcommands {
      '{MKDIR} ' .. game_patches_dst,
      '{COPY} ' .. game_patches_src .. '/*.toml ' .. game_patches_dst
    }

  -- Copy assets/font next to executable
  filter("platforms:Linux-*")
    local assets_font_src = path.getabsolute(path.join(project_root, "assets", "font"))
    local assets_font_dst = path.getabsolute(path.join(project_root, "build", "bin", "Linux")) .. "/%{cfg.buildcfg}/assets/font"
    postbuildcommands {
      '{MKDIR} ' .. assets_font_dst,
      '{COPY} ' .. assets_font_src .. '/* ' .. assets_font_dst
    }

  -- macOS app bundle configuration.
  filter("platforms:Mac-*")
    local entitlements_path = path.getabsolute(project_root .. "/xenia.entitlements")
    local app_bundle = "${TARGET_BUILD_DIR}/${FULL_PRODUCT_NAME}"
    local app_contents = app_bundle .. "/Contents"
    local app_frameworks = app_contents .. "/Frameworks"
    local app_macos = app_contents .. "/MacOS"
    local app_executable = app_macos .. "/xenios"
    local macdeployqt_command =
        'app_arch="$(lipo -archs "' .. app_executable .. '" | awk \'{print $1}\')"; '
            .. 'qt_dir="${QT_DIR:-}"; '
            .. 'qtcore_dep="$(otool -L "' .. app_executable
            .. '" | grep "QtCore.framework" | awk \'{print $1}\' | head -n 1)"; '
            .. 'if [ -z "$qt_dir" ] && [ -n "$qtcore_dep" ] && [ "${qtcore_dep#/}" != "$qtcore_dep" ]; then '
            .. 'qt_dir="${qtcore_dep%%/lib/QtCore.framework/*}"; fi; '
            .. 'if [ -z "$qt_dir" ] && command -v brew >/dev/null 2>&1; then '
            .. 'for qt_prefix in "$(brew --prefix qtbase 2>/dev/null)" "$(brew --prefix qt@6 2>/dev/null)" "$(brew --prefix qt 2>/dev/null)"; do '
            .. 'if [ -n "$qt_prefix" ] && [ -f "$qt_prefix/lib/QtCore.framework/QtCore" ] && '
            .. 'lipo -archs "$qt_prefix/lib/QtCore.framework/QtCore" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'qt_dir="$qt_prefix"; break; fi; done; '
            .. 'if [ -z "$qt_dir" ] && [ "$(uname -m)" = "arm64" ] && command -v arch >/dev/null 2>&1; then '
            .. 'for qt_prefix in "$(arch -x86_64 brew --prefix qtbase 2>/dev/null || true)" "$(arch -x86_64 brew --prefix qt@6 2>/dev/null || true)" "$(arch -x86_64 brew --prefix qt 2>/dev/null || true)"; do '
            .. 'if [ -n "$qt_prefix" ] && [ -f "$qt_prefix/lib/QtCore.framework/QtCore" ] && '
            .. 'lipo -archs "$qt_prefix/lib/QtCore.framework/QtCore" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'qt_dir="$qt_prefix"; break; fi; done; fi; '
            .. 'fi; '
            .. 'if [ -n "$qt_dir" ] && [ -x "$qt_dir/bin/macdeployqt" ]; then '
            .. '"$qt_dir/bin/macdeployqt" "' .. app_bundle .. '"; '
            .. 'elif command -v macdeployqt >/dev/null 2>&1; then '
            .. 'macdeployqt "' .. app_bundle .. '"; '
            .. 'else '
            .. 'echo "warning: macdeployqt not found; Qt frameworks/plugins are not bundled" >&2; '
            .. 'fi'
    local bundle_sdl_command =
        'app_arch="$(lipo -archs "' .. app_executable .. '" | awk \'{print $1}\')"; '
            .. 'sdl_dep="$(otool -L "' .. app_executable
            .. '" | grep "libSDL2-2.0.0.dylib" | awk \'{print $1}\' | head -n 1)"; '
            .. 'bundle_sdl="' .. app_frameworks .. '/libSDL2-2.0.0.dylib"; '
            .. 'rm -f "$bundle_sdl"; '
            .. 'sdl_src=""; '
            .. 'if [ -n "$sdl_dep" ] && [ "${sdl_dep#/}" != "$sdl_dep" ] && [ -f "$sdl_dep" ] && '
            .. 'lipo -archs "$sdl_dep" | tr " " "\\n" | grep -qx "$app_arch"; then sdl_src="$sdl_dep"; fi; '
            .. 'if [ -z "$sdl_src" ] && [ -n "${SDL2_DIR:-}" ] && [ -f "${SDL2_DIR}/lib/libSDL2-2.0.0.dylib" ] && '
            .. 'lipo -archs "${SDL2_DIR}/lib/libSDL2-2.0.0.dylib" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'sdl_src="${SDL2_DIR}/lib/libSDL2-2.0.0.dylib"; fi; '
            .. 'if [ -z "$sdl_src" ] && command -v brew >/dev/null 2>&1; then '
            .. 'for sdl_prefix in "$(brew --prefix sdl2 2>/dev/null)"; do '
            .. 'if [ -n "$sdl_prefix" ] && [ -f "$sdl_prefix/lib/libSDL2-2.0.0.dylib" ] && '
            .. 'lipo -archs "$sdl_prefix/lib/libSDL2-2.0.0.dylib" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'sdl_src="$sdl_prefix/lib/libSDL2-2.0.0.dylib"; break; fi; done; '
            .. 'if [ -z "$sdl_src" ] && [ "$(uname -m)" = "arm64" ] && command -v arch >/dev/null 2>&1; then '
            .. 'for sdl_prefix in "$(arch -x86_64 brew --prefix sdl2 2>/dev/null || true)"; do '
            .. 'if [ -n "$sdl_prefix" ] && [ -f "$sdl_prefix/lib/libSDL2-2.0.0.dylib" ] && '
            .. 'lipo -archs "$sdl_prefix/lib/libSDL2-2.0.0.dylib" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'sdl_src="$sdl_prefix/lib/libSDL2-2.0.0.dylib"; break; fi; done; fi; '
            .. 'fi; '
            .. 'if [ -z "$sdl_src" ]; then '
            .. 'for brew_bin in /usr/local/bin/brew /opt/homebrew/bin/brew; do '
            .. 'if [ -x "$brew_bin" ]; then '
            .. 'sdl_prefix="$("$brew_bin" --prefix sdl2 2>/dev/null || true)"; '
            .. 'if [ -n "$sdl_prefix" ] && [ -f "$sdl_prefix/lib/libSDL2-2.0.0.dylib" ] && '
            .. 'lipo -archs "$sdl_prefix/lib/libSDL2-2.0.0.dylib" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'sdl_src="$sdl_prefix/lib/libSDL2-2.0.0.dylib"; break; fi; '
            .. 'fi; '
            .. 'done; '
            .. 'fi; '
            .. 'if [ -z "$sdl_src" ]; then '
            .. 'for sdl_prefix in /usr/local/opt/sdl2 /opt/homebrew/opt/sdl2; do '
            .. 'if [ -f "$sdl_prefix/lib/libSDL2-2.0.0.dylib" ] && '
            .. 'lipo -archs "$sdl_prefix/lib/libSDL2-2.0.0.dylib" | tr " " "\\n" | grep -qx "$app_arch"; then '
            .. 'sdl_src="$sdl_prefix/lib/libSDL2-2.0.0.dylib"; break; fi; '
            .. 'done; '
            .. 'fi; '
            .. 'if [ -n "$sdl_src" ]; then '
            .. 'cp -f "$sdl_src" "$bundle_sdl"; '
            .. 'install_name_tool -id "@rpath/libSDL2-2.0.0.dylib" "'
            .. app_frameworks .. '/libSDL2-2.0.0.dylib"; '
            .. 'elif [ -n "$sdl_dep" ]; then '
            .. 'echo "error: compatible SDL2 dylib not found for app architecture $app_arch" >&2; exit 1; '
            .. 'fi'
    local relink_sdl_command =
        'sdl_dep="$(otool -L "' .. app_executable
            .. '" | grep "libSDL2-2.0.0.dylib" | awk \'{print $1}\' | head -n 1)"; '
            .. 'if [ -n "$sdl_dep" ] && [ "$sdl_dep" != "@rpath/libSDL2-2.0.0.dylib" ] && [ -f "'
            .. app_frameworks .. '/libSDL2-2.0.0.dylib" ]; then '
            .. 'install_name_tool -change "$sdl_dep" "@rpath/libSDL2-2.0.0.dylib" "'
            .. app_executable .. '"; '
            .. 'fi'
    files({
      "Info.plist",
      project_root.."/xenia.entitlements",
      project_root.."/assets/apple/AppIcon.xcassets",
    })
    linkoptions({
      "-Wl,-rpath,@executable_path/../Frameworks",
      "-Wl,-rpath,@loader_path/../Frameworks",
    })
    xcodebuildsettings({
      ["INFOPLIST_FILE"] = path.getabsolute("Info.plist"),
      ["MACOSX_DEPLOYMENT_TARGET"] = "15.0",
      ["PRODUCT_NAME"] = "XeniOS",
      ["EXECUTABLE_NAME"] = "xenios",
      ["PRODUCT_BUNDLE_IDENTIFIER"] = "jp.xenios.xenios.macos",
      ["ASSETCATALOG_COMPILER_APPICON_NAME"] = "AppIcon",
      ["CODE_SIGN_STYLE"] = "Automatic",
      ["CODE_SIGN_ENTITLEMENTS"] = entitlements_path,
      ["CODE_SIGN_ALLOW_ENTITLEMENTS_MODIFICATION"] = "YES",
      ["LD_RUNPATH_SEARCH_PATHS"] =
          "@executable_path/../Frameworks @loader_path/../Frameworks",
    })
    local optimized_settings_src =
        path.getabsolute(path.join(project_root, ".data_repos",
                                   "optimized-settings", "settings"))
    local game_patches_src =
        path.getabsolute(path.join(project_root, ".data_repos",
                                   "game-patches", "patches"))
    local assets_font_src =
        path.getabsolute(path.join(project_root, "assets", "font"))
    postbuildcommands({
      'mkdir -p "' .. app_frameworks .. '"',
      'mkdir -p "' .. app_macos .. '/optimized_settings"',
      'mkdir -p "' .. app_macos .. '/game_patches"',
      'mkdir -p "' .. app_macos .. '/assets/font"',
      'if ls "' .. optimized_settings_src .. '/*.json" >/dev/null 2>&1; then '
          .. 'cp -f "' .. optimized_settings_src .. '/*.json" "'
          .. app_macos .. '/optimized_settings/"; fi; true',
      'if ls "' .. game_patches_src .. '/*.toml" >/dev/null 2>&1; then '
          .. 'cp -f "' .. game_patches_src .. '/*.toml" "'
          .. app_macos .. '/game_patches/"; fi; true',
      'if ls "' .. assets_font_src .. '/*" >/dev/null 2>&1; then '
          .. 'cp -f "' .. assets_font_src .. '/*" "'
          .. app_macos .. '/assets/font/"; fi; true',
      'cp -f "' .. path.join(metal_converter_libdir, "libmetalirconverter.dylib")
          .. '" "' .. app_frameworks .. '/"',
      bundle_sdl_command,
      'if ! otool -l "' .. app_executable
          .. '" | grep -q "@executable_path/../Frameworks"; then '
          .. 'install_name_tool -add_rpath "@executable_path/../Frameworks" "'
          .. app_executable .. '"; fi',
      'if ! otool -l "' .. app_executable
          .. '" | grep -q "@loader_path/../Frameworks"; then '
          .. 'install_name_tool -add_rpath "@loader_path/../Frameworks" "'
          .. app_executable .. '"; fi',
      macdeployqt_command,
      relink_sdl_command,
      'codesign --force --sign - "' .. app_frameworks
          .. '/libmetalirconverter.dylib"',
      'if [ -f "' .. app_frameworks .. '/libSDL2-2.0.0.dylib" ]; then '
          .. 'codesign --force --sign - "'
          .. app_frameworks .. '/libSDL2-2.0.0.dylib"; fi',
      'rm -f "' .. app_macos .. '"/*.log',
      'codesign --force --deep --sign - --entitlements "'
          .. entitlements_path .. '" "' .. app_bundle .. '"',
    })
  filter({"platforms:Mac-*", "architecture:arm64"})
    postbuildcommands({
      'cp -f "' .. path.join(dxilconv_libdir_arm64, "libdxilconv.dylib")
          .. '" "' .. app_frameworks .. '/"',
      'codesign --force --sign - "' .. app_frameworks .. '/libdxilconv.dylib"',
    })
  filter({"platforms:Mac-*", "architecture:x86_64"})
    postbuildcommands({
      'cp -f "' .. path.join(dxilconv_libdir_x86_64, "libdxilconv.dylib")
          .. '" "' .. app_frameworks .. '/"',
      'codesign --force --sign - "' .. app_frameworks .. '/libdxilconv.dylib"',
    })
  filter({"platforms:Mac-*", "files:**.icns"})
    buildaction("Resources")
