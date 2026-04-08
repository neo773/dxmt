
#include "d3d9_buffer.hpp"
#include "d3d9_device.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include <cstring>

namespace dxmt {

D3D9VertexBuffer::D3D9VertexBuffer(
    D3D9Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
    Rc<Buffer> buffer, Rc<BufferAllocation> allocation)
    : device_(device), length_(length), usage_(usage), fvf_(fvf), pool_(pool),
      buffer_(std::move(buffer)), allocation_(std::move(allocation)) {}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexBuffer9) ||
      riid == __uuidof(IDirect3DResource9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Lock(
    UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
#ifdef DXMT_PERF
  auto perf_t0 = rdtsc();
#endif
  if (!ppbData) return D3DERR_INVALIDCALL;

  if ((Flags & D3DLOCK_DISCARD) && (usage_ & D3DUSAGE_DYNAMIC)) {
    // Fast path: bump-allocate from ring — no Metal buffer creation
    auto &ring = device_->GetDynamicBufferRing();
    auto &queue = device_->GetQueue();
    auto [block, offset] = ring.allocate(
        queue.CurrentSeqId(), queue.CoherentSeqId(), length_, 256);
    ring_mtl_buffer_ = block.buffer;
    ring_gpu_address_ = block.gpu_address + offset;
    ring_mapped_ = static_cast<char *>(block.mapped_address) + offset;
    using_ring_ = true;
  } else if ((usage_ & D3DUSAGE_DYNAMIC) &&
             !(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    // Dynamic buffer, regular write: suballocate + copy existing data
    void *old_data = mappedMemory();
    auto &ring = device_->GetDynamicBufferRing();
    auto &queue = device_->GetQueue();
    auto [block, offset] = ring.allocate(
        queue.CurrentSeqId(), queue.CoherentSeqId(), length_, 256);
    ring_mtl_buffer_ = block.buffer;
    ring_gpu_address_ = block.gpu_address + offset;
    ring_mapped_ = static_cast<char *>(block.mapped_address) + offset;
    memcpy(ring_mapped_, old_data, length_);
    using_ring_ = true;
  } else if (!(usage_ & D3DUSAGE_DYNAMIC) &&
             !(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    // Static buffer, writable lock: COW with new allocation (rare path)
    auto old_alloc = allocation_;
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    allocation_ = buffer_->allocate(allocFlags);
    memcpy(allocation_->mappedMemory(0), old_alloc->mappedMemory(0), length_);
    buffer_->rename(Rc(allocation_));
    using_ring_ = false;
  }
  // NOOVERWRITE / READONLY: return existing pointer unchanged

  *ppbData = static_cast<uint8_t *>(mappedMemory()) + OffsetToLock;
#ifdef DXMT_PERF
  device_->GetQueue().CurrentFrameStatistics().d3d9_lock_ticks += rdtsc() - perf_t0;
#endif
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Unlock() {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  pDesc->Format = D3DFMT_VERTEXDATA;
  pDesc->Type = D3DRTYPE_VERTEXBUFFER;
  pDesc->Usage = usage_;
  pDesc->Pool = pool_;
  pDesc->Size = length_;
  pDesc->FVF = fvf_;
  return S_OK;
}

// D3D9IndexBuffer

D3D9IndexBuffer::D3D9IndexBuffer(
    D3D9Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    Rc<Buffer> buffer, Rc<BufferAllocation> allocation)
    : device_(device), length_(length), usage_(usage), format_(format), pool_(pool),
      buffer_(std::move(buffer)), allocation_(std::move(allocation)) {}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DIndexBuffer9) ||
      riid == __uuidof(IDirect3DResource9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Lock(
    UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
#ifdef DXMT_PERF
  auto perf_t0 = rdtsc();
#endif
  if (!ppbData) return D3DERR_INVALIDCALL;

  if ((Flags & D3DLOCK_DISCARD) && (usage_ & D3DUSAGE_DYNAMIC)) {
    auto &ring = device_->GetDynamicBufferRing();
    auto &queue = device_->GetQueue();
    auto [block, offset] = ring.allocate(
        queue.CurrentSeqId(), queue.CoherentSeqId(), length_, 256);
    ring_mtl_buffer_ = block.buffer;
    ring_gpu_address_ = block.gpu_address + offset;
    ring_mapped_ = static_cast<char *>(block.mapped_address) + offset;
    using_ring_ = true;
  } else if ((usage_ & D3DUSAGE_DYNAMIC) &&
             !(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    void *old_data = mappedMemory();
    auto &ring = device_->GetDynamicBufferRing();
    auto &queue = device_->GetQueue();
    auto [block, offset] = ring.allocate(
        queue.CurrentSeqId(), queue.CoherentSeqId(), length_, 256);
    ring_mtl_buffer_ = block.buffer;
    ring_gpu_address_ = block.gpu_address + offset;
    ring_mapped_ = static_cast<char *>(block.mapped_address) + offset;
    memcpy(ring_mapped_, old_data, length_);
    using_ring_ = true;
  } else if (!(usage_ & D3DUSAGE_DYNAMIC) &&
             !(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
    auto old_alloc = allocation_;
    dxmt::Flags<BufferAllocationFlag> allocFlags;
    allocFlags.set(BufferAllocationFlag::CpuPlaced);
    allocation_ = buffer_->allocate(allocFlags);
    memcpy(allocation_->mappedMemory(0), old_alloc->mappedMemory(0), length_);
    buffer_->rename(Rc(allocation_));
    using_ring_ = false;
  }

  *ppbData = static_cast<uint8_t *>(mappedMemory()) + OffsetToLock;
#ifdef DXMT_PERF
  device_->GetQueue().CurrentFrameStatistics().d3d9_lock_ticks += rdtsc() - perf_t0;
#endif
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Unlock() {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  if (!pDesc) return D3DERR_INVALIDCALL;
  pDesc->Format = format_;
  pDesc->Type = D3DRTYPE_INDEXBUFFER;
  pDesc->Usage = usage_;
  pDesc->Pool = pool_;
  pDesc->Size = length_;
  return S_OK;
}

} // namespace dxmt
