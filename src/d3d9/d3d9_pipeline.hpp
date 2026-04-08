#pragma once

#include "Metal.hpp"
#include "dxmt_tasks.hpp"
#include "log/log.hpp"
#include <atomic>

namespace dxmt {

class D3D9PipelineWork {
public:
  virtual ~D3D9PipelineWork() {}
  virtual D3D9PipelineWork *RunThreadpoolWork() = 0;
  virtual bool GetIsDone() = 0;
  virtual void SetIsDone(bool state) = 0;
};

template <> struct task_trait<D3D9PipelineWork *> {
  D3D9PipelineWork *run_task(D3D9PipelineWork *task) { return task->RunThreadpoolWork(); }
  bool get_done(D3D9PipelineWork *task) { return task->GetIsDone(); }
  void set_done(D3D9PipelineWork *task) { task->SetIsDone(true); }
};

class D3D9CompiledPipeline : public D3D9PipelineWork {
public:
  D3D9CompiledPipeline(WMT::Device device, WMTRenderPipelineInfo info)
      : device_(device), info_(info) {}

  obj_handle_t GetPipeline() {
    ready_.wait(false, std::memory_order_acquire);
    return state_.handle;
  }

  D3D9PipelineWork *RunThreadpoolWork() override {
    WMT::Reference<WMT::Error> err;
    state_ = device_.newRenderPipelineState(info_, err);
    if (!state_) {
      Logger::err("D3D9: Async PSO creation failed");
      if (err) {
        auto desc = err.description();
        if (desc) Logger::err(desc.getUTF8String());
      }
    }
    return this;
  }

  bool GetIsDone() override { return ready_.load(std::memory_order_relaxed); }
  void SetIsDone(bool state) override {
    ready_.store(state, std::memory_order_release);
    ready_.notify_all();
  }

private:
  WMT::Device device_;
  WMTRenderPipelineInfo info_;
  WMT::Reference<WMT::RenderPipelineState> state_;
  std::atomic_bool ready_{false};
};

} // namespace dxmt
