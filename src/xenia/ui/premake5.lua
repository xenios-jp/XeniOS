project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-ui")
  uuid("d0407c25-b0ea-40dc-846c-82c46fbd9fa2")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
  })
  local_platform_files()
  removefiles({
    "*_demo.cc",
    "windowed_app_main_*.cc",
  })

  filter("platforms:Mac-*")
    files({
      "surface_mac.mm",
    })
    removefiles({
      "renderdoc_api.cc",
      "renderdoc_api.h",
    })
  filter("system:ios")
    files({
      "file_picker_ios.mm",
      "surface_ios.mm",
      "touch_controls_ios.mm",
      "window_ios.mm",
      "windowed_app_context_ios.mm",
    })
    removefiles({
      "renderdoc_api.cc",
      "renderdoc_api.h",
      -- miniaudio pulls in AudioToolbox which textually includes Foundation
      -- (ObjC) headers that cannot be compiled as C++.
      -- TODO(wmarti): Add audio_helper_ios.mm using AVAudioPlayer to restore
      -- achievement sounds on iOS.
      "audio_helper.cc",
      "audio_helper.h",
      -- Qt is not used on iOS; native UIKit is used instead.
      "*_qt.cc",
      "*_qt.h",
      "qt_util.h",
      "ui_resources_qrc.cpp",
      "window_qt.cc",
      "window_qt.h",
      "windowed_app_context_qt.cc",
      "windowed_app_context_qt.h",
      "windowed_app_main_qt.cc",
    })
  filter({})
  if os.istarget("android") then
    filter("platforms:Android-*")
      -- Exports JNI functions.
      wholelib("On")
  end

  filter("platforms:Windows-*")
    links({
      "dwmapi",
      "dxgi",
      "winmm",
    })

  filter("platforms:Linux-*")
    links({
      "xcb",
      "X11",
      "X11-xcb",
      "fontconfig"
    })
    files({
      "ui_resources_qrc.cpp",
    })
