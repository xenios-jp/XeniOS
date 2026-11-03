/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// iOS entry point. Each piece of the launcher / settings / compatibility UI
// lives in its own ios_*.{h,mm} pair under src/xenia/ui/; this file is just
// the UIApplicationMain trampoline that hands control to XeniaAppDelegate.

#import <UIKit/UIKit.h>

#include <fcntl.h>
#include <unistd.h>

#include <initializer_list>

// UIKit owns the entry point; SDL must not replace main with SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#import "xenia/ui/ios/app/ios_app_delegate.h"

int main(int argc, char* argv[]) {
  // Launched from a debugger or JIT tool, stdout and stderr are a pipe that tool drains. If it
  // stops draining, every console write blocks, the log thread's and the system's own (sandbox
  // warnings) on the main thread, and the app freezes. Drop console output instead; the log
  // file keeps all of it.
  for (int fd : {STDOUT_FILENO, STDERR_FILENO}) {
    int flags = fcntl(fd, F_GETFL);
    if (flags != -1 && !(flags & O_NONBLOCK)) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
  // Mark the real UIKit thread as SDL's main thread before worker-side init.
  SDL_SetMainReady();
  NSLog(@"iOS: skipping ptrace/debugger JIT setup; using normal W^X app flow.");
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([XeniaAppDelegate class]));
  }
}
