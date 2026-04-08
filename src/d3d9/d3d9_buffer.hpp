#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "dxmt_buffer.hpp"
#include <d3d9.h>
#include <cstring>

namespace dxmt {

class D3D9Device;

class D3D9VertexBuffer final : public ComObjectClamp<IDirect3DVertexBuffer9> {
public:
  D3D9VertexBuffer(D3D9Device *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                   Rc<Buffer> buffer, Rc<BufferAllocation> allocation);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_VERTEXBUFFER; }

  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) final;

  // Accessors for draw path — work for both static and dynamic (ring) buffers
  WMT::Buffer rawBuffer() { return using_ring_ ? ring_mtl_buffer_ : allocation_->buffer(); }
  uint64_t gpuAddress() { return using_ring_ ? ring_gpu_address_ : allocation_->gpuAddress(); }
  void *mappedMemory() { return using_ring_ ? ring_mapped_ : allocation_->mappedMemory(0); }
  bool isDynamic() const { return usage_ & D3DUSAGE_DYNAMIC; }
  UINT length() const { return length_; }

  // Legacy accessor — only valid for static buffers (ring buffers don't use BufferAllocation)
  BufferAllocation *allocation() { return allocation_.ptr(); }
  Rc<Buffer> &buffer() { return buffer_; }

private:
  D3D9Device *device_;
  UINT length_;
  DWORD usage_;
  DWORD fvf_;
  D3DPOOL pool_;

  // Static buffer backing (used when not using ring allocator)
  Rc<Buffer> buffer_;
  Rc<BufferAllocation> allocation_;

  // Ring allocator backing (used for dynamic buffers after first DISCARD lock)
  bool using_ring_ = false;
  WMT::Buffer ring_mtl_buffer_;
  uint64_t ring_gpu_address_ = 0;
  void *ring_mapped_ = nullptr;
};

class D3D9IndexBuffer final : public ComObjectClamp<IDirect3DIndexBuffer9> {
public:
  D3D9IndexBuffer(D3D9Device *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                  Rc<Buffer> buffer, Rc<BufferAllocation> allocation);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_INDEXBUFFER; }

  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) final;

  // Accessors for draw path — work for both static and dynamic (ring) buffers
  WMT::Buffer rawBuffer() { return using_ring_ ? ring_mtl_buffer_ : allocation_->buffer(); }
  uint64_t gpuAddress() { return using_ring_ ? ring_gpu_address_ : allocation_->gpuAddress(); }
  void *mappedMemory() { return using_ring_ ? ring_mapped_ : allocation_->mappedMemory(0); }
  D3DFORMAT format() const { return format_; }
  bool isDynamic() const { return usage_ & D3DUSAGE_DYNAMIC; }
  UINT length() const { return length_; }

  // Legacy accessor — only valid for static buffers
  BufferAllocation *allocation() { return allocation_.ptr(); }
  Rc<Buffer> &buffer() { return buffer_; }

private:
  D3D9Device *device_;
  UINT length_;
  DWORD usage_;
  D3DFORMAT format_;
  D3DPOOL pool_;

  // Static buffer backing
  Rc<Buffer> buffer_;
  Rc<BufferAllocation> allocation_;

  // Ring allocator backing (used for dynamic buffers)
  bool using_ring_ = false;
  WMT::Buffer ring_mtl_buffer_;
  uint64_t ring_gpu_address_ = 0;
  void *ring_mapped_ = nullptr;
};

} // namespace dxmt
