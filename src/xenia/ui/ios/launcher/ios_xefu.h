/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_XEFU_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_XEFU_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef __OBJC__
#import <Foundation/Foundation.h>

@class UIDocumentPickerViewController;
@protocol UIDocumentPickerDelegate;
#endif  // __OBJC__

namespace xe {
namespace ui {

// Original Xbox games run through the Xbox 360's own Xbox emulator, which XeniOS
// doesn't include: the user brings the Compatibility folder of a console's hard
// drive, with xbox.xex and the xefu builds. It lives in the xefu folder of the
// storage root, where the emulator looks for it (Emulator::xefu_path).

// What xbox.xex stands for in the xefu_launcher setting: it picks the build for
// each game, as the console does.
extern const char kIOSXeFuAutomaticLauncher[];

// The folder the emulator reads it from.
std::filesystem::path IOSXeFuPath();

struct IOSXeFuInfo {
  // xbox.xex or a build is there.
  bool installed = false;
  bool has_automatic = false;
  // The files a game can start through, besides xbox.xex: the builds and the
  // config loader. Sorted naturally.
  std::vector<std::string> launchers;
  uint64_t size = 0;
};
// Walks the folder, so keep it off the main thread.
IOSXeFuInfo ReadIOSXeFuInfo();

// The file xefu_launcher names, when it is installed. When it isn't (after a
// reinstall or a removal), xbox.xex when installed, else the first of
// |info|'s launchers: that becomes the setting, and is saved.
std::string ResolveIOSXeFuLauncher(const IOSXeFuInfo& info);
void SetIOSXeFuLauncher(const std::string& launcher);
// Whether an original Xbox game can start: the launcher it would use is
// installed. Quick enough for the main thread.
bool IsIOSXeFuReady();

enum class IOSXeFuResult {
  kSuccess,
  kCancelled,
  // What was picked has neither xbox.xex nor a build.
  kNotXeFu,
  kCopyFailed,
};

// Removes the installed folder.
bool RemoveIOSXeFu();

#ifdef __OBJC__
// What an install that didn't succeed says. Nil for a finished or cancelled
// one.
NSString* IOSXeFuInstallFailureMessage(IOSXeFuResult result);

// The Files picker for the install: the Compatibility folder, the folder it is
// in, or its files. Nothing is copied.
UIDocumentPickerViewController* NewIOSXeFuPicker(id<UIDocumentPickerDelegate> delegate);

// Copies what |urls| picked into the xefu folder off the main thread. The copy
// is made beside the folder and swapped in whole, so a failed or cancelled
// install leaves the installed one as it was. |progress| gets whole percents
// and |completion| the result, both on the main thread. Setting |cancel| stops
// it early. When one is installed already, |confirm_replace| is asked on the
// main thread, once the picked files are checked, and answers through
// |answer|; declining finishes as cancelled. Holds the security scope of
// |urls| throughout.
void InstallIOSXeFuAsync(NSArray<NSURL*>* urls, std::shared_ptr<std::atomic<bool>> cancel,
                         void (^progress)(int percent),
                         void (^confirm_replace)(void (^answer)(BOOL replace)),
                         void (^completion)(IOSXeFuResult result));
#endif  // __OBJC__

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_XEFU_H_
