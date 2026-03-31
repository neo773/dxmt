#pragma once

#include "com/com_object.hpp"
#include "dxmt_buffer.hpp"
#include <d3d9.h>
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <vector>

namespace dxmt {

class D3D9Device;

class BufferRecyclePool {
public:
  // Round up to next power of 2 (min 256)
  static uint64_t sizeClass(uint64_t length) {
    if (length <= 256) return 256;
    length--;
    length |= length >> 1; length |= length >> 2;
    length |= length >> 4; length |= length >> 8;
    length |= length >> 16; length |= length >> 32;
    return length + 1;
  }

  Rc<BufferAllocation> tryGet(uint64_t alloc_size, uint64_t coherent_seq) {
    auto it = pools_.find(alloc_size);
    if (it == pools_.end()) return {};
    auto &q = it->second;
    if (!q.empty() && q.front().will_free_at <= coherent_seq) {
      auto alloc = std::move(q.front().alloc);
      q.pop();
      total_bytes_ -= alloc_size;
      return alloc;
    }
    return {};
  }

  void put(uint64_t alloc_size, Rc<BufferAllocation> alloc, uint64_t current_seq) {
    if (std::find(active_sizes_.begin(), active_sizes_.end(), alloc_size) == active_sizes_.end())
      active_sizes_.push_back(alloc_size);
    pools_[alloc_size].push({std::move(alloc), current_seq});
    total_bytes_ += alloc_size;
  }

  // Evicts completed entries for sizes not used this frame (call from Present)
  std::vector<Rc<BufferAllocation>> trim(uint64_t coherent_seq) {
    std::vector<Rc<BufferAllocation>> evicted;
    if (total_bytes_ <= kMaxBytes) { active_sizes_.clear(); return evicted; }
    for (auto &[len, q] : pools_) {
      if (std::find(active_sizes_.begin(), active_sizes_.end(), len) != active_sizes_.end()) continue;
      while (!q.empty() && q.front().will_free_at <= coherent_seq) {
        evicted.push_back(std::move(q.front().alloc));
        q.pop();
        total_bytes_ -= len;
        if (total_bytes_ <= kMaxBytes) {
          active_sizes_.clear();
          return evicted;
        }
      }
    }
    active_sizes_.clear();
    return evicted;
  }

private:
  static constexpr uint64_t kMaxBytes = 512 * 1024 * 1024;

  struct Entry {
    Rc<BufferAllocation> alloc;
    uint64_t will_free_at;
  };

  std::unordered_map<uint64_t, std::queue<Entry>> pools_;
  std::vector<uint64_t> active_sizes_;
  uint64_t total_bytes_ = 0;
};

class D3D9VertexBuffer final : public ComObjectClamp<IDirect3DVertexBuffer9> {
public:
  D3D9VertexBuffer(D3D9Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                   Rc<Buffer> buffer, Rc<BufferAllocation> allocation);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_VERTEXBUFFER; }

  // IDirect3DVertexBuffer9
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) final;

  Rc<Buffer> &buffer() { return buffer_; }
  BufferAllocation *allocation() { return allocation_.ptr(); }
  bool isDynamic() const { return usage_ & D3DUSAGE_DYNAMIC; }
  UINT length() const { return length_; }

private:
  D3D9Device *device_;
  UINT length_;
  DWORD usage_;
  DWORD fvf_;
  D3DPOOL pool_;
  Rc<Buffer> buffer_;
  Rc<BufferAllocation> allocation_;
};

class D3D9IndexBuffer final : public ComObjectClamp<IDirect3DIndexBuffer9> {
public:
  D3D9IndexBuffer(D3D9Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                  Rc<Buffer> buffer, Rc<BufferAllocation> allocation);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_INDEXBUFFER; }

  // IDirect3DIndexBuffer9
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) final;

  Rc<Buffer> &buffer() { return buffer_; }
  BufferAllocation *allocation() { return allocation_.ptr(); }
  D3DFORMAT format() const { return format_; }
  bool isDynamic() const { return usage_ & D3DUSAGE_DYNAMIC; }
  UINT length() const { return length_; }

private:
  D3D9Device *device_;
  UINT length_;
  DWORD usage_;
  D3DFORMAT format_;
  D3DPOOL pool_;
  Rc<Buffer> buffer_;
  Rc<BufferAllocation> allocation_;
};

} // namespace dxmt
