/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_GRAPHICS_SYSTEM_H
#define XENIA_GPU_METAL_GRAPHICS_SYSTEM_H

#include <memory>

#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"

namespace xe {
namespace gpu {
namespace metal {
class MetalGraphicsSystem : public GraphicsSystem {
 public:
  MetalGraphicsSystem();
  ~MetalGraphicsSystem();

  static bool IsAvailable();

  // Commits the open command buffer and blocks until every submitted command
  // buffer has completed. Call only on |command_processor|'s worker thread (from
  // CallInThread). Used to drain the GPU before the app backgrounds.
  static void AwaitGpuIdleOnWorkerThread(CommandProcessor* command_processor);
  // Lets the compile threads use every core while the guest is held waiting for
  // a shader rebuild. Any thread.
  static void SetShaderWaitCompileBoost(CommandProcessor* command_processor,
                                        bool waiting);

  std::string name() const override;

  X_STATUS Setup(cpu::Processor* processor, kernel::KernelState* kernel_state,
                 ui::WindowedAppContext* app_context,
                 bool is_surface_required) override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_GRAPHICS_SYSTEM_H
