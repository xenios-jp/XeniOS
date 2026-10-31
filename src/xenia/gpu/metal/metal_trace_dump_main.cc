/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
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
DEFINE_bool(
    metal_trace_dump_timing, false,
    "Drain the GPU after every replayed frame and write its command-buffer GPU "
    "time and wall time to <trace>_timing.json. Draining stops the CPU and GPU "
    "from overlapping across frames.",
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

  void AwaitGuestMemoryWrites(uint32_t base, uint32_t size) override {
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

  void EndFrame(int pass, int frame) override {
    if (!cvars::metal_trace_dump_timing) {
      return;
    }
    auto* command_processor = static_cast<MetalCommandProcessor*>(
        graphics_system_->command_processor());
    uint64_t gpu_ns = 0, command_buffers = 0;
    threading::Fence fence;
    command_processor->CallInThread([&]() {
      gpu_ns = command_processor->AwaitCompletedGpuTimeNs(&command_buffers);
      fence.Signal();
    });
    fence.Wait();
    auto now = std::chrono::steady_clock::now();
    if (timing_frames_.empty()) {
      timing_last_wall_ = timing_start_;
    }
    timing_frames_.push_back(
        {pass, frame, player_->frame(frame)->commands.size(),
         gpu_ns - timing_last_gpu_ns_,
         command_buffers - timing_last_command_buffers_,
         uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now - timing_last_wall_)
                      .count())});
    timing_last_gpu_ns_ = gpu_ns;
    timing_last_command_buffers_ = command_buffers;
    timing_last_wall_ = now;
  }

  void EndReplay(const std::filesystem::path& base_output_path) override {
    if (!cvars::metal_trace_dump_timing) {
      return;
    }
    std::filesystem::path path = base_output_path;
    path.replace_filename(path.stem().concat("_timing.json"));
    FILE* file = filesystem::OpenFile(path, "wb");
    if (!file) {
      XELOGE("Metal trace dump: could not write {}",
             xe::strip_pii(xe::path_to_utf8(path)));
      return;
    }
    // Frame 0's wall time includes setup and trace loading.
    fmt::print(file,
               "{{\"provenance\": \"measured\", \"gpu\": \"sum of "
               "GPUEndTime - GPUStartTime over the frame's command buffers\", "
               "\"wall\": \"steady clock from the previous frame's drain to "
               "this one's\", \"frames\": [");
    for (size_t i = 0; i < timing_frames_.size(); ++i) {
      const TimingFrame& f = timing_frames_[i];
      fmt::print(file,
                 "{}\n  {{\"pass\": {}, \"frame\": {}, \"commands\": {}, "
                 "\"gpu_ms\": {:.4f}, \"command_buffers\": {}, "
                 "\"wall_ms\": {:.4f}}}",
                 i ? "," : "", f.pass, f.frame, f.commands, f.gpu_ns * 1e-6,
                 f.command_buffers, f.wall_ns * 1e-6);
    }
    fmt::print(file, "\n]}}\n");
    fclose(file);
    XELOGI("Metal trace dump: timing written to {}",
           xe::strip_pii(xe::path_to_utf8(path)));
  }

 private:
  struct TimingFrame {
    int pass;
    int frame;
    size_t commands;
    uint64_t gpu_ns;
    uint64_t command_buffers;
    uint64_t wall_ns;
  };

  bool capturing_ = false;
  std::vector<TimingFrame> timing_frames_;
  std::chrono::steady_clock::time_point timing_start_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point timing_last_wall_;
  uint64_t timing_last_gpu_ns_ = 0;
  uint64_t timing_last_command_buffers_ = 0;
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
