#pragma once

#include "com/com_object.hpp"
#include "dxmt_occlusion_query.hpp"
#include <d3d9.h>

namespace dxmt {

class D3D9Device;

class D3D9Query final : public ComObjectClamp<IDirect3DQuery9> {
public:
  D3D9Query(D3DQUERYTYPE type, D3D9Device *device)
      : type_(type), device_(device) {
    if (type_ == D3DQUERYTYPE_OCCLUSION)
      query_ = new VisibilityResultQuery();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() final { return type_; }
  DWORD STDMETHODCALLTYPE GetDataSize() final {
    switch (type_) {
    case D3DQUERYTYPE_EVENT: return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    default: return 0;
    }
  }

  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) final;
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) final;

private:
  D3DQUERYTYPE type_;
  D3D9Device *device_;
  Rc<VisibilityResultQuery> query_;
  bool building_ = false;
  bool issued_ = false;
};

} // namespace dxmt
