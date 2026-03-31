#pragma once

#include "Metal.hpp"
#include "airconv_public.h"
#include "com/com_object.hpp"
#include "sha1/sha1_util.hpp"
#include <d3d9.h>
#include <map>
#include <unordered_map>
#include <vector>

namespace dxmt {

class D3D9VertexDeclaration;

class D3D9VertexShader final : public ComObjectClamp<IDirect3DVertexShader9> {
public:
  D3D9VertexShader(sm30_shader_t shader,
                   std::vector<uint8_t> bytecode)
      : shader_(shader), bytecode_(std::move(bytecode)),
        sha1_(Sha1HashState::compute(bytecode_.data(), bytecode_.size())),
        maxConstantReg_(SM30GetMaxConstantRegister(shader)) {}

  ~D3D9VertexShader() {
    if (shader_)
      SM30Destroy(shader_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexShader9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetFunction(void *, UINT *) final { return D3DERR_INVALIDCALL; }

  sm30_shader_t handle() const { return shader_; }
  const uint8_t *bytecode() const { return bytecode_.data(); }
  uint32_t bytecodeSize() const { return (uint32_t)bytecode_.size(); }
  uint32_t maxConstantReg() const { return maxConstantReg_; }
  const Sha1Digest &sha1() const { return sha1_; }

  WMT::Function getLayoutVariant(D3D9VertexDeclaration *vdecl) const {
    auto it = layout_variants_.find(vdecl);
    if (it != layout_variants_.end()) return it->second;
    return {};
  }

  void addLayoutVariant(D3D9VertexDeclaration *vdecl, WMT::Reference<WMT::Function> func) {
    layout_variants_[vdecl] = std::move(func);
  }

private:
  sm30_shader_t shader_;
  std::vector<uint8_t> bytecode_;
  Sha1Digest sha1_;
  uint32_t maxConstantReg_;
  std::unordered_map<D3D9VertexDeclaration*, WMT::Reference<WMT::Function>> layout_variants_;
};

class D3D9PixelShader final : public ComObjectClamp<IDirect3DPixelShader9> {
public:
  D3D9PixelShader(sm30_shader_t shader, WMT::Reference<WMT::Function> function,
                  uint8_t maxTexcoordCount, const Sha1Digest &sha1)
      : shader_(shader), sha1_(sha1), maxTexcoordCount_(maxTexcoordCount),
        maxConstantReg_(SM30GetMaxConstantRegister(shader)) {
    variants_[0] = std::move(function);
  }

  ~D3D9PixelShader() {
    if (shader_)
      SM30Destroy(shader_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DPixelShader9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE GetFunction(void *, UINT *) final { return D3DERR_INVALIDCALL; }

  // Variant key encodes alpha test func (bits 0-3) and fog mode (bits 4-6)
  // fogMode: 0=none, 1=vertex, 2=table EXP, 3=table EXP2, 4=table LINEAR
  static uint8_t makeVariantKey(uint8_t alphaFunc, uint8_t fogMode) {
    return (alphaFunc & 0xF) | ((fogMode & 0x7) << 4);
  }

  WMT::Function getVariant(uint8_t key) const {
    auto it = variants_.find(key);
    if (it != variants_.end()) return it->second;
    return {};
  }

  void addVariant(uint8_t key, WMT::Reference<WMT::Function> func) {
    variants_[key] = std::move(func);
  }

  sm30_shader_t handle() const { return shader_; }
  uint8_t maxTexcoordCount() const { return maxTexcoordCount_; }
  uint32_t maxConstantReg() const { return maxConstantReg_; }
  const Sha1Digest &sha1() const { return sha1_; }

private:
  sm30_shader_t shader_;
  Sha1Digest sha1_;
  uint8_t maxTexcoordCount_ = 0;
  uint32_t maxConstantReg_;
  std::map<uint8_t, WMT::Reference<WMT::Function>> variants_;
};

} // namespace dxmt
