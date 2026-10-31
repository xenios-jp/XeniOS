/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Xenia Trace Bench: the Metal trace dumper as an iOS app. Launch arguments
// are the dumper's cvars, relative paths resolve against the app's Documents,
// and the app exits with the dumper's status once the replay ends, e.g.
//   xcrun devicectl device process launch --console --terminate-existing
//     --device ID com.xenios.tracebench --target_trace_file=a.xtr
//     --trace_dump_path=Run/a --trace_dump_capture_frame=3
//     --metal_trace_dump_capture=Run/a/a_f3.gputrace

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

namespace xe::gpu::metal {
int trace_dump_main(const std::vector<std::string>& args);
}

namespace {
int launch_argc;
char** launch_argv;
}  // namespace

@interface XeniaTraceBenchAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation XeniaTraceBenchAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  application.idleTimerDisabled = YES;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    // Defaults first, so the launch arguments override them. The replay runs
    // no guest code, so it needs no JIT and no debugger.
    std::vector<std::string> args = {launch_argv[0], "--cpu=null",
                                     "--framerate_limit=0",
                                     "--metal_trace_dump_timing=true"};
    args.insert(args.end(), launch_argv + 1, launch_argv + launch_argc);
    std::vector<char*> argv;
    for (std::string& argument : args) {
      argv.push_back(argument.data());
    }
    int argc = int(argv.size());
    char** argv_data = argv.data();
    cvar::ParseLaunchArguments(argc, argv_data, "", {});
    xe::InitializeLogging("xenia-gpu-metal-trace-app");
    int result = xe::gpu::metal::trace_dump_main({args[0]});
    xe::ShutdownLogging();
    std::fprintf(stderr, "Xenia Trace Bench: exit %d\n", result);
    std::exit(result);
  });
  return YES;
}

@end

// iOS requires the scene life cycle; the replay draws nothing, so the scene has
// no window.
@interface XeniaTraceBenchSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation XeniaTraceBenchSceneDelegate
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    launch_argc = argc;
    launch_argv = argv;
    NSString* documents = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES)
                              .firstObject;
    chdir(documents.fileSystemRepresentation);
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(XeniaTraceBenchAppDelegate.class));
  }
}
