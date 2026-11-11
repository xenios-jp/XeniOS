/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <cstdint>
#include <functional>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  // Whether guest memory in a range is yet to be written by GPU work already
  // recorded (CommandProcessor::AwaitGpuWritesTo): vertices and indices there
  // can't be read yet, so nothing is estimated from them.
  void SetUnfinishedGpuWritesQuery(
      std::function<bool(uint32_t address, uint32_t length)> query) {
    unfinished_gpu_writes_query_ = std::move(query);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader,
                              bool window_offset_in_edram);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader,
                        bool window_offset_in_edram);

  // For the clear quad rule: the exported positions of the last draw whose
  // vertex shader ran on the CPU, when it had at most kMaxLastVertices
  // vertices and every one of them exported x, y and z without being killed.
  static constexpr uint32_t kMaxLastVertices = 4;
  struct LastVertexPositions {
    bool valid = false;
    uint32_t count = 0;
    float x[kMaxLastVertices] = {};
    float y[kMaxLastVertices] = {};
    float z[kMaxLastVertices] = {};
    // NaN if not exported.
    float w[kMaxLastVertices] = {};
  };
  const LastVertexPositions& last_vertex_positions() const {
    return last_vertex_positions_;
  }
  void ResetLastDraw() { last_vertex_positions_ = LastVertexPositions(); }

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_x_.reset();
      position_z_.reset();
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_x() const { return position_x_; }
    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_z() const { return position_z_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_x_;
    std::optional<float> position_z_;
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;
  std::function<bool(uint32_t address, uint32_t length)>
      unfinished_gpu_writes_query_;

  ShaderInterpreter shader_interpreter_;

  LastVertexPositions last_vertex_positions_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
