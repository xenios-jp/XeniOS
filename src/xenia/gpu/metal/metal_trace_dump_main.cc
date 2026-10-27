/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_graphics_system.h"
#include "xenia/gpu/trace_dump.h"
#include "xenia/ui/metal/metal_api.h"
#include "xenia/ui/metal/metal_provider.h"

DEFINE_string(
    metal_trace_dump_capture, "",
    "Path of a .gputrace to write around the replayed frame. Empty takes no "
    "capture. Needs METAL_CAPTURE_ENABLED=1 in the environment.",
    "Metal");

namespace xe {
namespace gpu {
namespace metal {

class MetalTraceDump : public TraceDump {
 public:
  std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem() override {
    return std::unique_ptr<gpu::GraphicsSystem>(new MetalGraphicsSystem());
  }

  void BeginHostCapture() override {
    if (cvars::metal_trace_dump_capture.empty()) {
      return;
    }
    auto* provider = static_cast<const ui::metal::MetalProvider*>(
        graphics_system_->provider());
    MTL::Device* device = provider ? provider->GetDevice() : nullptr;
    if (!device) {
      XELOGE("Metal trace dump: no device to capture");
      return;
    }

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    if (!manager || !manager->supportsDestination(
                        MTL::CaptureDestinationGPUTraceDocument)) {
      XELOGE(
          "Metal trace dump: GPU trace documents unavailable - run with "
          "METAL_CAPTURE_ENABLED=1");
      return;
    }

    // Metal refuses to overwrite, and a .gputrace is a directory.
    std::filesystem::path capture_path(cvars::metal_trace_dump_capture);
    std::error_code remove_error;
    std::filesystem::remove_all(capture_path, remove_error);

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    NS::String* path_string = NS::String::string(capture_path.string().c_str(),
                                                 NS::UTF8StringEncoding);
    MTL::CaptureDescriptor* descriptor =
        MTL::CaptureDescriptor::alloc()->init();
    descriptor->setCaptureObject(device);
    descriptor->setDestination(MTL::CaptureDestinationGPUTraceDocument);
    descriptor->setOutputURL(NS::URL::fileURLWithPath(path_string));

    NS::Error* error = nullptr;
    if (manager->startCapture(descriptor, &error)) {
      capturing_ = true;
      XELOGI("Metal trace dump: capturing to {}",
             xe::strip_pii(xe::path_to_utf8(capture_path)));
    } else {
      const char* message = "unknown error";
      if (error && error->localizedDescription()) {
        message = error->localizedDescription()->utf8String();
      }
      XELOGE("Metal trace dump: failed to start capture - {}", message);
    }
    descriptor->release();
    pool->release();
  }

  void EndHostCapture() override {
    // A trace whose frame holds no swap never reaches the presenter, so there
    // would be nothing for the PNG.
    auto* command_processor = static_cast<MetalCommandProcessor*>(
        graphics_system_->command_processor());
    if (command_processor && !command_processor->HasSeenSwap()) {
      // The swap records into the command processor's command buffer, which
      // only its own thread may touch.
      threading::Fence swap_fence;
      command_processor->CallInThread([&]() {
        command_processor->ForceIssueSwap();
        swap_fence.Signal();
      });
      swap_fence.Wait();
    }

    if (!capturing_) {
      return;
    }
    capturing_ = false;
    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    if (manager && manager->isCapturing()) {
      manager->stopCapture();
    }
    XELOGI("Metal trace dump: capture written to {}",
           cvars::metal_trace_dump_capture);
  }

  void AwaitGuestMemoryWrites() override {
    // Zero-copy shared memory aliases guest memory, so completing the queue
    // is enough.
    auto* command_processor = static_cast<MetalCommandProcessor*>(
        graphics_system_->command_processor());
    threading::Fence fence;
    command_processor->CallInThread([&]() {
      command_processor->AwaitAllQueueOperationsCompletion();
      fence.Signal();
    });
    fence.Wait();
  }

 private:
  bool capturing_ = false;
};

int trace_dump_main(const std::vector<std::string>& args) {
  MetalTraceDump trace_dump;
  return trace_dump.Main(args);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia-gpu-metal-trace-dump",
                      xe::gpu::metal::trace_dump_main, "some.trace",
                      "target_trace_file");
