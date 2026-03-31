#pragma once

#include "util_flags.hpp"
#include <array>
#include <chrono>

#ifdef __i386__
#include <x86intrin.h>
#endif

namespace dxmt {

using clock = std::chrono::high_resolution_clock;

// Low-overhead timestamp counter for sampled profiling.
// Avoids Wine PE→Unix thunking overhead of std::chrono (~500ns → ~10ns).
inline uint64_t rdtsc() {
#ifdef __i386__
  return __rdtsc();
#else
  return clock::now().time_since_epoch().count();
#endif
}

// Calibrate TSC ticks per millisecond (call once at init).
inline double calibrate_tsc_to_ms() {
  auto c0 = clock::now();
  uint64_t t0 = rdtsc();
  // Busy-spin ~1ms
  while (std::chrono::duration_cast<std::chrono::microseconds>(
             clock::now() - c0).count() < 1000) {}
  uint64_t t1 = rdtsc();
  auto c1 = clock::now();
  double ms = std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count() / 1000.0;
  return (double)(t1 - t0) / ms;
}

enum class FeatureCompatibility {
    UnsupportedGeometryDraw,
    UnsupportedTessellationOutputPrimitive,
    UnsupportedIndirectTessellationDraw,
    UnsupportedGeometryTessellationDraw,
    UnsupportedDrawAuto,
    UnsupportedPredication,
    UnsupportedStreamOutputAppending,
    UnsupportedMultipleStreamOutput,
  };

enum class ScalerType {
  None,
  Spatial,
  Temporal,
};

struct ScalerInfo {
  ScalerType type = ScalerType::None;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t output_width;
  uint32_t output_height;
  bool auto_exposure;
  bool motion_vector_highres;
};

struct FrameStatistics {
  Flags<FeatureCompatibility> compatibility_flags;
  uint32_t command_buffer_count = 0;
  uint32_t sync_count = 0;
  clock::duration sync_interval{};
  clock::duration commit_interval{};
  uint32_t render_pass_count = 0;
  uint32_t render_pass_optimized = 0;
  uint32_t clear_pass_count = 0;
  uint32_t clear_pass_optimized = 0;
  uint32_t resolve_pass_optimized = 0;
  uint32_t compute_pass_count = 0;
  uint32_t blit_pass_count = 0;
  uint32_t event_stall = 0;
  uint32_t latency = 0;
  clock::duration encode_prepare_interval{};
  clock::duration encode_flush_interval{};
  clock::duration drawable_blocking_interval{};
  clock::duration present_lantency_interval{};
  ScalerInfo last_scaler_info{};

  // D3D9 main-thread profiling (written only when DXMT_DEBUG is defined)
  uint32_t d3d9_draw_count = 0;
  uint32_t d3d9_draw_up_count = 0;
  uint32_t d3d9_state_change_count = 0;
  uint32_t d3d9_pso_miss_count = 0;
  uint64_t d3d9_frame_ticks = 0;
  uint64_t d3d9_capture_ticks = 0;
  uint64_t d3d9_lock_ticks = 0;

  void
  reset() {
    compatibility_flags.clrAll();
    command_buffer_count = 0;
    sync_count = 0;
    sync_interval = {};
    commit_interval = {};
    render_pass_count = 0;
    render_pass_optimized = 0;
    clear_pass_count = 0;
    clear_pass_optimized = 0;
    resolve_pass_optimized = 0;
    compute_pass_count = 0;
    blit_pass_count = 0;
    event_stall = 0;
    latency = 0;
    encode_prepare_interval = {};
    encode_flush_interval = {};
    drawable_blocking_interval = {};
    present_lantency_interval = {};
    last_scaler_info.type = {};
    d3d9_draw_count = 0;
    d3d9_draw_up_count = 0;
    d3d9_state_change_count = 0;
    d3d9_pso_miss_count = 0;
    d3d9_frame_ticks = 0;
    d3d9_capture_ticks = 0;
    d3d9_lock_ticks = 0;
  };
};

constexpr size_t kFrameStatisticsCount = 16;

class FrameStatisticsContainer {
  std::array<FrameStatistics, kFrameStatisticsCount> frames_;
  FrameStatistics min_;
  FrameStatistics max_;
  FrameStatistics average_;

public:
  FrameStatistics &
  at(uint64_t frame) {
    return frames_[frame % kFrameStatisticsCount];
  }
  const FrameStatistics &
  at(uint64_t frame) const {
    return frames_.at(frame % kFrameStatisticsCount);
  }

  const FrameStatistics &
  min() const {
    return min_;
  }

  const FrameStatistics &
  max() const {
    return max_;
  }

  const FrameStatistics &
  average() const {
    return average_;
  }

  void
  compute(uint64_t current_frame) {
    min_.reset();
    max_.reset();
    average_.reset();
    current_frame = current_frame % kFrameStatisticsCount;
    for(unsigned i = 0; i < kFrameStatisticsCount; i++) {
      if (i == current_frame)
        continue; // deliberately exclude current frame since it

#define STATS_MIN(field) min_.field = std::min(min_.field, frames_[i].field)
#define STATS_MAX(field) max_.field = std::max(max_.field, frames_[i].field)
#define STATS_SUM(field) average_.field += frames_[i].field

      STATS_MIN(command_buffer_count);
      STATS_MIN(sync_count);
      STATS_MIN(event_stall);
      STATS_MIN(render_pass_count);
      STATS_MIN(render_pass_optimized);
      STATS_MIN(clear_pass_count);
      STATS_MIN(clear_pass_optimized);
      STATS_MIN(commit_interval);
      STATS_MIN(sync_interval);
      STATS_MIN(encode_prepare_interval);
      STATS_MIN(encode_flush_interval);
      STATS_MIN(drawable_blocking_interval);
      STATS_MIN(present_lantency_interval);
      STATS_MIN(d3d9_draw_count);
      STATS_MIN(d3d9_draw_up_count);

      STATS_MIN(d3d9_state_change_count);
      STATS_MIN(d3d9_pso_miss_count);
      STATS_MIN(d3d9_frame_ticks);
      STATS_MIN(d3d9_capture_ticks);
      STATS_MIN(d3d9_lock_ticks);

      STATS_MAX(command_buffer_count);
      STATS_MAX(sync_count);
      STATS_MAX(event_stall);
      STATS_MAX(render_pass_count);
      STATS_MAX(render_pass_optimized);
      STATS_MAX(clear_pass_count);
      STATS_MAX(clear_pass_optimized);
      STATS_MAX(commit_interval);
      STATS_MAX(sync_interval);
      STATS_MAX(encode_prepare_interval);
      STATS_MAX(encode_flush_interval);
      STATS_MAX(drawable_blocking_interval);
      STATS_MAX(present_lantency_interval);
      STATS_MAX(d3d9_draw_count);
      STATS_MAX(d3d9_draw_up_count);

      STATS_MAX(d3d9_state_change_count);
      STATS_MAX(d3d9_pso_miss_count);
      STATS_MAX(d3d9_frame_ticks);
      STATS_MAX(d3d9_capture_ticks);
      STATS_MAX(d3d9_lock_ticks);

      STATS_SUM(command_buffer_count);
      STATS_SUM(sync_count);
      STATS_SUM(event_stall);
      STATS_SUM(render_pass_count);
      STATS_SUM(render_pass_optimized);
      STATS_SUM(clear_pass_count);
      STATS_SUM(clear_pass_optimized);
      STATS_SUM(commit_interval);
      STATS_SUM(sync_interval);
      STATS_SUM(encode_prepare_interval);
      STATS_SUM(encode_flush_interval);
      STATS_SUM(drawable_blocking_interval);
      STATS_SUM(present_lantency_interval);
      STATS_SUM(d3d9_draw_count);
      STATS_SUM(d3d9_draw_up_count);

      STATS_SUM(d3d9_state_change_count);
      STATS_SUM(d3d9_pso_miss_count);
      STATS_SUM(d3d9_frame_ticks);
      STATS_SUM(d3d9_capture_ticks);
      STATS_SUM(d3d9_lock_ticks);

#undef STATS_MIN
#undef STATS_MAX
#undef STATS_SUM
    }
    constexpr auto N = kFrameStatisticsCount - 1;

#define STATS_AVG(field) average_.field /= N

    STATS_AVG(command_buffer_count);
    STATS_AVG(sync_count);
    STATS_AVG(event_stall);
    STATS_AVG(render_pass_count);
    STATS_AVG(render_pass_optimized);
    STATS_AVG(clear_pass_count);
    STATS_AVG(clear_pass_optimized);
    STATS_AVG(commit_interval);
    STATS_AVG(sync_interval);
    STATS_AVG(encode_prepare_interval);
    STATS_AVG(encode_flush_interval);
    STATS_AVG(drawable_blocking_interval);
    STATS_AVG(present_lantency_interval);
    STATS_AVG(d3d9_draw_count);
    STATS_AVG(d3d9_draw_up_count);

    STATS_AVG(d3d9_state_change_count);
    STATS_AVG(d3d9_pso_miss_count);
    STATS_AVG(d3d9_frame_ticks);
    STATS_AVG(d3d9_capture_ticks);
    STATS_AVG(d3d9_lock_ticks);

#undef STATS_AVG
  };
};

} // namespace dxmt
