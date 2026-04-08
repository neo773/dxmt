#pragma once

#include "com/com_object.hpp"
#include "dxmt_texture.hpp"
#include "d3d9_format.hpp"
#include <d3d9.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

namespace dxmt {

class D3D9Device;
class D3D9TextureSurface;

class D3D9Texture2D final : public ComObjectClamp<IDirect3DTexture9> {
public:
  D3D9Texture2D(D3D9Device *device, UINT width, UINT height, UINT levels,
                 D3DFORMAT format, Rc<Texture> texture, TextureViewKey viewKey)
      : device_(device), width_(width), height_(height), format_(format),
        texture_(std::move(texture)), viewKey_(viewKey) {
    if (levels == 0)
      levels = (UINT)std::floor(std::log2((double)std::max(width, height))) + 1;
    levelCount_ = levels;

    // Allocate per-level staging data
    UINT mipW = width, mipH = height;
    for (UINT i = 0; i < levelCount_; i++) {
      MipLevel mip;
      mip.width = mipW;
      mip.height = mipH;
      mip.pitch = D3D9FormatPitch(format, mipW);
      mip.dataSize = D3D9FormatMipSize(format, mipW, mipH);
      mip.data = std::malloc(mip.dataSize);
      std::memset(mip.data, 0, mip.dataSize);
      mip.dirty = false;
      mips_.push_back(mip);
      mipW = std::max(1u, mipW / 2);
      mipH = std::max(1u, mipH / 2);
    }
  }

  ~D3D9Texture2D() {
    for (auto &mip : mips_) {
      if (mip.data) std::free(mip.data);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DTexture9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_TEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return 0; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levelCount_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) final { return S_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return D3DTEXF_NONE; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final {}

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final {
    if (Level >= levelCount_ || !pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format = format_;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = 0;
    pDesc->Pool = D3DPOOL_MANAGED;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = mips_[Level].width;
    pDesc->Height = mips_[Level].height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) final;

  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    if (Level >= levelCount_ || !pLockedRect) return D3DERR_INVALIDCALL;
    auto &mip = mips_[Level];
    pLockedRect->Pitch = mip.pitch;
    if (pRect) {
      // Return pointer to the sub-rect origin within the mip buffer
      UINT bytesPerPixel = mip.pitch / mip.width;
      pLockedRect->pBits = static_cast<uint8_t *>(mip.data)
        + pRect->top * mip.pitch + pRect->left * bytesPerPixel;
    } else {
      pLockedRect->pBits = mip.data;
    }
    if (!(Flags & D3DLOCK_NO_DIRTY_UPDATE)) {
      mip.dirty = true;
      anyDirty_ = true;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) final {
    if (Level >= levelCount_) return D3DERR_INVALIDCALL;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) final {
    // Mark level 0 dirty so the next draw uploads the data
    // But skip RT textures — their content lives on the GPU
    if (!isRenderTarget_ && !mips_.empty()) {
      mips_[0].dirty = true;
      anyDirty_ = true;
    }
    return S_OK;
  }

  // Internal accessors
  Rc<Texture> &texture() { return texture_; }
  TextureViewKey viewKey() const { return viewKey_; }
  TextureViewKey srgbViewKey() const { return srgbViewKey_; }
  TextureViewKey rtViewKey() const { return rtViewKey_; }
  bool hasSrgbView() const { return srgbViewKey_ != 0; }
  UINT width() const { return width_; }
  UINT height() const { return height_; }
  D3DFORMAT format() const { return format_; }
  bool isRT() const { return isRenderTarget_; }
  void setRT(TextureViewKey rtKey) { isRenderTarget_ = true; rtViewKey_ = rtKey; }
  void setSrgbView(TextureViewKey key) { srgbViewKey_ = key; }
  UINT levelCount() const { return levelCount_; }

  bool isAnyDirty() const { return anyDirty_; }

  void uploadDirtyLevels(WMT::Texture mtlTexture) {
    bool compressed = IsCompressedFormat(format_);
    for (UINT i = 0; i < levelCount_; i++) {
      auto &mip = mips_[i];
      if (!mip.dirty) continue;

      WMTOrigin origin = {0, 0, 0};
      WMTSize size = {mip.width, mip.height, 1};
      if (compressed) {
        uint32_t rows = (mip.height + 3) / 4;
        mtlTexture.replaceRegion(origin, size, i, 0, mip.data, mip.pitch, mip.pitch * rows);
      } else if (format_ == D3DFMT_A4R4G4B4) {
        // Convert A4R4G4B4 (2bpp) → BGRA8 (4bpp) for upload
        uint32_t gpuPitch = mip.width * 4;
        std::vector<uint32_t> converted(mip.width * mip.height);
        auto *src16 = (const uint16_t *)mip.data;
        for (UINT row = 0; row < mip.height; row++) {
          for (UINT col = 0; col < mip.width; col++) {
            uint16_t px = src16[row * (mip.pitch / 2) + col];
            // D3D9 A4R4G4B4: A(15-12) R(11-8) G(7-4) B(3-0)
            uint8_t a4 = (px >> 12) & 0xF;
            uint8_t r4 = (px >> 8) & 0xF;
            uint8_t g4 = (px >> 4) & 0xF;
            uint8_t b4 = px & 0xF;
            // Expand 4-bit to 8-bit: val * 17 = val * 255 / 15
            // Pack as BGRA8 (little-endian: bytes B, G, R, A)
            converted[row * mip.width + col] =
              (uint32_t)(b4 * 17) |
              ((uint32_t)(g4 * 17) << 8) |
              ((uint32_t)(r4 * 17) << 16) |
              ((uint32_t)(a4 * 17) << 24);
          }
        }
        mtlTexture.replaceRegion(origin, size, i, 0, converted.data(), gpuPitch, 0);
      } else {
        mtlTexture.replaceRegion(origin, size, i, 0, mip.data, mip.pitch, 0);
      }
      mip.dirty = false;
    }
    anyDirty_ = false;
  }

  // Staged upload: copies mip data to a staging buffer and emits a blit command
  // into the current command chunk. This avoids the replaceRegion race where
  // the CPU overwrites shared texture data while the GPU still reads it.
  void uploadDirtyLevelsStaged(Rc<Texture> &gpuTex, dxmt::CommandQueue &queue) {
    bool compressed = IsCompressedFormat(format_);
    for (UINT i = 0; i < levelCount_; i++) {
      auto &mip = mips_[i];
      if (!mip.dirty) continue;

      const void *srcData = mip.data;
      UINT srcPitch = mip.pitch;
      UINT uploadHeight = mip.height;
      UINT uploadPitch = srcPitch;

      // Handle format conversions
      std::vector<uint32_t> convertedBuf;
      if (format_ == D3DFMT_A4R4G4B4) {
        uploadPitch = mip.width * 4;
        convertedBuf.resize(mip.width * mip.height);
        auto *src16 = (const uint16_t *)mip.data;
        for (UINT row = 0; row < mip.height; row++) {
          for (UINT col = 0; col < mip.width; col++) {
            uint16_t px = src16[row * (srcPitch / 2) + col];
            uint8_t a4 = (px >> 12) & 0xF;
            uint8_t r4 = (px >> 8) & 0xF;
            uint8_t g4 = (px >> 4) & 0xF;
            uint8_t b4 = px & 0xF;
            convertedBuf[row * mip.width + col] =
              (uint32_t)(b4 * 17) | ((uint32_t)(g4 * 17) << 8) |
              ((uint32_t)(r4 * 17) << 16) | ((uint32_t)(a4 * 17) << 24);
          }
        }
        srcData = convertedBuf.data();
        srcPitch = uploadPitch;
      }

      if (compressed)
        uploadHeight = (mip.height + 3) / 4;

      size_t uploadSize = (size_t)uploadPitch * uploadHeight;
      auto staging = queue.AllocateTransientBuffer(uploadSize, 16);
      std::memcpy(staging.cpu_ptr, srcData, uploadSize);

      auto chunk = queue.CurrentChunk();
      Rc<TextureAllocation> texAlloc(gpuTex->current());
      chunk->emitcc([
        tex = gpuTex,
        texAlloc = std::move(texAlloc),
        stagingBuf = staging.buffer.handle,
        stagingOff = (uint64_t)staging.offset,
        w = mip.width, h = mip.height,
        pitch = uploadPitch, level = i
      ](ArgumentEncodingContext &ctx) mutable {
        ctx.startBlitPass();
        auto dstHandle = ctx.access(texAlloc.ptr(), DXMT_ENCODER_RESOURCE_ACESS_WRITE);
        auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        blitCmd.type = WMTBlitCommandCopyFromBufferToTexture;
        blitCmd.src = stagingBuf;
        blitCmd.src_offset = stagingOff;
        blitCmd.bytes_per_row = pitch;
        blitCmd.bytes_per_image = 0;
        blitCmd.dst = dstHandle;
        blitCmd.level = level;
        blitCmd.slice = 0;
        blitCmd.origin = {0, 0, 0};
        blitCmd.size = {w, h, 1};
        ctx.endPass();
      });

      mip.dirty = false;
    }
    anyDirty_ = false;
  }

  // Legacy accessors for backward compat with single-level path
  bool isDirty() const { return isAnyDirty(); }
  void clearDirty() {
    for (auto &mip : mips_) mip.dirty = false;
    anyDirty_ = false;
  }
  const void *data() const { return mips_[0].data; }
  UINT pitch() const { return mips_[0].pitch; }

private:
  struct MipLevel {
    UINT width;
    UINT height;
    UINT pitch;
    size_t dataSize;
    void *data = nullptr;
    bool dirty = false;
  };

  D3D9Device *device_;
  UINT width_;
  UINT height_;
  D3DFORMAT format_;
  UINT levelCount_;
  bool isRenderTarget_ = false;
  bool anyDirty_ = false;
  std::vector<MipLevel> mips_;

  Rc<Texture> texture_;
  TextureViewKey viewKey_;
  TextureViewKey srgbViewKey_ = 0;
  TextureViewKey rtViewKey_ = 0;
};

// Lightweight surface wrapper for GetSurfaceLevel — delegates Lock/Unlock to parent texture mip
class D3D9TextureSurface final : public ComObjectClamp<IDirect3DSurface9> {
public:
  D3D9TextureSurface(D3D9Texture2D *parent, UINT level)
      : parent_(parent), level_(level) {
    parent_->AddRef();
  }
  ~D3D9TextureSurface() {
    if (parent_) parent_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DSurface9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_SURFACE; }

  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (!ppContainer) return E_POINTER;
    return parent_->QueryInterface(riid, ppContainer);
  }

  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final {
    return parent_->GetLevelDesc(level_, pDesc);
  }

  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    return parent_->LockRect(level_, pLockedRect, pRect, Flags);
  }

  HRESULT STDMETHODCALLTYPE UnlockRect() final {
    return parent_->UnlockRect(level_);
  }

  HRESULT STDMETHODCALLTYPE GetDC(HDC *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) final { return D3DERR_INVALIDCALL; }

  // Internal accessors for render target usage
  Rc<Texture> &texture() { return parent_->texture(); }
  TextureViewKey viewKey() const { return parent_->isRT() ? parent_->rtViewKey() : parent_->viewKey(); }
  WMTPixelFormat mtlFormat() const { return parent_->texture()->pixelFormat(); }
  D3D9Texture2D *parentTexture() const { return parent_; }

private:
  D3D9Texture2D *parent_;
  UINT level_;
};

// Deferred implementation — needs D3D9TextureSurface to be complete
inline HRESULT STDMETHODCALLTYPE D3D9Texture2D::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  if (Level >= levelCount_ || !ppSurfaceLevel) return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = ref(new D3D9TextureSurface(this, Level));
  return S_OK;
}

// ============================================================================
// Cube texture — 6 faces × N mip levels, backed by a single WMTTextureTypeCube
// ============================================================================

class D3D9TextureCube final : public ComObjectClamp<IDirect3DCubeTexture9> {
public:
  D3D9TextureCube(D3D9Device *device, UINT edgeLength, UINT levels,
                  D3DFORMAT format, Rc<Texture> texture, TextureViewKey viewKey)
      : device_(device), edgeLength_(edgeLength), format_(format),
        texture_(std::move(texture)), viewKey_(viewKey) {
    if (levels == 0)
      levels = (UINT)std::floor(std::log2((double)edgeLength)) + 1;
    levelCount_ = levels;

    for (UINT face = 0; face < 6; face++) {
      UINT mipW = edgeLength;
      for (UINT i = 0; i < levelCount_; i++) {
        MipLevel mip;
        mip.width = mipW;
        mip.height = mipW; // cube faces are square
        mip.pitch = D3D9FormatPitch(format, mipW);
        mip.dataSize = D3D9FormatMipSize(format, mipW, mipW);
        mip.data = std::malloc(mip.dataSize);
        std::memset(mip.data, 0, mip.dataSize);
        mip.dirty = false;
        faceMips_[face].push_back(mip);
        mipW = std::max(1u, mipW / 2);
      }
    }
  }

  ~D3D9TextureCube() {
    for (UINT face = 0; face < 6; face++)
      for (auto &mip : faceMips_[face])
        if (mip.data) std::free(mip.data);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DCubeTexture9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_CUBETEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return 0; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levelCount_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) final { return S_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return D3DTEXF_NONE; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final {}

  // IDirect3DCubeTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final {
    if (Level >= levelCount_ || !pDesc) return D3DERR_INVALIDCALL;
    auto &mip = faceMips_[0][Level];
    pDesc->Format = format_;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = 0;
    pDesc->Pool = D3DPOOL_MANAGED;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = mip.width;
    pDesc->Height = mip.height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level,
                                               IDirect3DSurface9 **ppSurface) final;

  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                      D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    if (FaceType > 5 || Level >= levelCount_ || !pLockedRect) return D3DERR_INVALIDCALL;
    auto &mip = faceMips_[FaceType][Level];
    pLockedRect->Pitch = mip.pitch;
    if (pRect) {
      UINT bytesPerPixel = mip.pitch / mip.width;
      pLockedRect->pBits = static_cast<uint8_t *>(mip.data)
        + pRect->top * mip.pitch + pRect->left * bytesPerPixel;
    } else {
      pLockedRect->pBits = mip.data;
    }
    if (!(Flags & D3DLOCK_NO_DIRTY_UPDATE)) {
      mip.dirty = true;
      anyDirty_ = true;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) final {
    if (FaceType > 5 || Level >= levelCount_) return D3DERR_INVALIDCALL;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *) final {
    if (FaceType > 5) return D3DERR_INVALIDCALL;
    if (!faceMips_[FaceType].empty()) {
      faceMips_[FaceType][0].dirty = true;
      anyDirty_ = true;
    }
    return S_OK;
  }

  // Internal accessors — same interface as D3D9Texture2D for texture binding
  Rc<Texture> &texture() { return texture_; }
  TextureViewKey viewKey() const { return viewKey_; }
  TextureViewKey srgbViewKey() const { return srgbViewKey_; }
  bool hasSrgbView() const { return srgbViewKey_ != 0; }
  void setSrgbView(TextureViewKey key) { srgbViewKey_ = key; }
  D3DFORMAT format() const { return format_; }
  UINT edgeLength() const { return edgeLength_; }
  UINT levelCount() const { return levelCount_; }
  bool isAnyDirty() const { return anyDirty_; }
  bool isRT() const { return isRenderTarget_; }
  void setRT() { isRenderTarget_ = true; }
  TextureViewKey faceRTViewKey(UINT face) const { return faceRTViewKeys_[face]; }
  void setFaceRTViewKey(UINT face, TextureViewKey key) { faceRTViewKeys_[face] = key; }

  void uploadDirtyLevelsStaged(Rc<Texture> &gpuTex, dxmt::CommandQueue &queue) {
    bool compressed = IsCompressedFormat(format_);
    for (UINT face = 0; face < 6; face++) {
      for (UINT i = 0; i < levelCount_; i++) {
        auto &mip = faceMips_[face][i];
        if (!mip.dirty) continue;

        const void *srcData = mip.data;
        UINT srcPitch = mip.pitch;
        UINT uploadHeight = mip.height;
        UINT uploadPitch = srcPitch;

        std::vector<uint32_t> convertedBuf;
        if (format_ == D3DFMT_A4R4G4B4) {
          uploadPitch = mip.width * 4;
          convertedBuf.resize(mip.width * mip.height);
          auto *src16 = (const uint16_t *)mip.data;
          for (UINT row = 0; row < mip.height; row++) {
            for (UINT col = 0; col < mip.width; col++) {
              uint16_t px = src16[row * (srcPitch / 2) + col];
              uint8_t a4 = (px >> 12) & 0xF;
              uint8_t r4 = (px >> 8) & 0xF;
              uint8_t g4 = (px >> 4) & 0xF;
              uint8_t b4 = px & 0xF;
              convertedBuf[row * mip.width + col] =
                (uint32_t)(b4 * 17) | ((uint32_t)(g4 * 17) << 8) |
                ((uint32_t)(r4 * 17) << 16) | ((uint32_t)(a4 * 17) << 24);
            }
          }
          srcData = convertedBuf.data();
          srcPitch = uploadPitch;
        }

        if (compressed)
          uploadHeight = (mip.height + 3) / 4;

        size_t uploadSize = (size_t)uploadPitch * uploadHeight;
        auto staging = queue.AllocateTransientBuffer(uploadSize, 16);
        std::memcpy(staging.cpu_ptr, srcData, uploadSize);

        auto chunk = queue.CurrentChunk();
        Rc<TextureAllocation> texAlloc(gpuTex->current());
        chunk->emitcc([
          tex = gpuTex,
          texAlloc = std::move(texAlloc),
          stagingBuf = staging.buffer.handle,
          stagingOff = (uint64_t)staging.offset,
          w = mip.width, h = mip.height,
          pitch = uploadPitch, level = i, slice = face
        ](ArgumentEncodingContext &ctx) mutable {
          ctx.startBlitPass();
          auto dstHandle = ctx.access(texAlloc.ptr(), DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          blitCmd.type = WMTBlitCommandCopyFromBufferToTexture;
          blitCmd.src = stagingBuf;
          blitCmd.src_offset = stagingOff;
          blitCmd.bytes_per_row = pitch;
          blitCmd.bytes_per_image = 0;
          blitCmd.dst = dstHandle;
          blitCmd.level = level;
          blitCmd.slice = slice;
          blitCmd.origin = {0, 0, 0};
          blitCmd.size = {w, h, 1};
          ctx.endPass();
        });

        mip.dirty = false;
      }
    }
    anyDirty_ = false;
  }

private:
  struct MipLevel {
    UINT width;
    UINT height;
    UINT pitch;
    size_t dataSize;
    void *data = nullptr;
    bool dirty = false;
  };

  D3D9Device *device_;
  UINT edgeLength_;
  D3DFORMAT format_;
  UINT levelCount_;
  bool anyDirty_ = false;
  std::vector<MipLevel> faceMips_[6]; // one vector per face

  bool isRenderTarget_ = false;
  Rc<Texture> texture_;
  TextureViewKey viewKey_;
  TextureViewKey srgbViewKey_ = 0;
  TextureViewKey faceRTViewKeys_[6] = {}; // per-face RT views (2DArray slice views)
};

// Lightweight surface wrapper for GetCubeMapSurface — delegates Lock/Unlock to parent cube texture face
class D3D9CubeMapSurface final : public ComObjectClamp<IDirect3DSurface9> {
public:
  D3D9CubeMapSurface(D3D9TextureCube *parent, D3DCUBEMAP_FACES face, UINT level)
      : parent_(parent), face_(face), level_(level) {
    parent_->AddRef();
  }
  ~D3D9CubeMapSurface() {
    if (parent_) parent_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DSurface9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3DERR_INVALIDCALL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) final { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() final { return 0; }
  void STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_SURFACE; }

  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (!ppContainer) return E_POINTER;
    return parent_->QueryInterface(riid, ppContainer);
  }

  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final {
    return parent_->GetLevelDesc(level_, pDesc);
  }

  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final {
    return parent_->LockRect(face_, level_, pLockedRect, pRect, Flags);
  }

  HRESULT STDMETHODCALLTYPE UnlockRect() final {
    return parent_->UnlockRect(face_, level_);
  }

  HRESULT STDMETHODCALLTYPE GetDC(HDC *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) final { return D3DERR_INVALIDCALL; }

  // Internal accessors for render target usage
  Rc<Texture> &texture() { return parent_->texture(); }
  TextureViewKey viewKey() const { return parent_->faceRTViewKey(face_); }
  WMTPixelFormat mtlFormat() const { return parent_->texture()->pixelFormat(); }
  D3D9TextureCube *parentTexture() const { return parent_; }
  D3DCUBEMAP_FACES face() const { return face_; }

private:
  D3D9TextureCube *parent_;
  D3DCUBEMAP_FACES face_;
  UINT level_;
};

// Deferred implementation — needs D3D9CubeMapSurface to be complete
inline HRESULT STDMETHODCALLTYPE D3D9TextureCube::GetCubeMapSurface(
    D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppSurface) {
  if (FaceType > 5 || Level >= levelCount_ || !ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = ref(new D3D9CubeMapSurface(this, FaceType, Level));
  return S_OK;
}

// ============================================================================
// Helpers — type-dispatch for bound texture access (2D or Cube)
// ============================================================================

inline Rc<Texture> &D3D9GetTexture(IDirect3DBaseTexture9 *base) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE)
    return static_cast<D3D9TextureCube *>(base)->texture();
  return static_cast<D3D9Texture2D *>(base)->texture();
}

inline TextureViewKey D3D9GetViewKey(IDirect3DBaseTexture9 *base) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE)
    return static_cast<D3D9TextureCube *>(base)->viewKey();
  return static_cast<D3D9Texture2D *>(base)->viewKey();
}

inline TextureViewKey D3D9GetSrgbViewKey(IDirect3DBaseTexture9 *base) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE)
    return static_cast<D3D9TextureCube *>(base)->srgbViewKey();
  return static_cast<D3D9Texture2D *>(base)->srgbViewKey();
}

inline bool D3D9HasSrgbView(IDirect3DBaseTexture9 *base) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE)
    return static_cast<D3D9TextureCube *>(base)->hasSrgbView();
  return static_cast<D3D9Texture2D *>(base)->hasSrgbView();
}

inline bool D3D9IsAnyDirty(IDirect3DBaseTexture9 *base) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE)
    return static_cast<D3D9TextureCube *>(base)->isAnyDirty();
  return static_cast<D3D9Texture2D *>(base)->isAnyDirty();
}

inline void D3D9UploadDirtyStaged(IDirect3DBaseTexture9 *base, dxmt::CommandQueue &queue) {
  if (base->GetType() == D3DRTYPE_CUBETEXTURE) {
    auto *cube = static_cast<D3D9TextureCube *>(base);
    cube->uploadDirtyLevelsStaged(cube->texture(), queue);
  } else {
    auto *tex = static_cast<D3D9Texture2D *>(base);
    tex->uploadDirtyLevelsStaged(tex->texture(), queue);
  }
}

} // namespace dxmt
