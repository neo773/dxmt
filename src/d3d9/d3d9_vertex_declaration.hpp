#pragma once

#include "com/com_object.hpp"
#include <d3d9.h>
#include <vector>

namespace dxmt {

inline uint32_t GetDecltypeSize(D3DDECLTYPE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:    return 4;
  case D3DDECLTYPE_FLOAT2:    return 8;
  case D3DDECLTYPE_FLOAT3:    return 12;
  case D3DDECLTYPE_FLOAT4:    return 16;
  case D3DDECLTYPE_D3DCOLOR:  return 4;
  case D3DDECLTYPE_UBYTE4:    return 4;
  case D3DDECLTYPE_SHORT2:    return 4;
  case D3DDECLTYPE_SHORT4:    return 8;
  case D3DDECLTYPE_UBYTE4N:   return 4;
  case D3DDECLTYPE_SHORT2N:   return 4;
  case D3DDECLTYPE_SHORT4N:   return 8;
  case D3DDECLTYPE_USHORT2N:  return 4;
  case D3DDECLTYPE_USHORT4N:  return 8;
  case D3DDECLTYPE_FLOAT16_2: return 4;
  case D3DDECLTYPE_FLOAT16_4: return 8;
  default:                    return 0;
  }
}

class D3D9VertexDeclaration final : public ComObjectClamp<IDirect3DVertexDeclaration9> {
public:
  D3D9VertexDeclaration(const D3DVERTEXELEMENT9 *pElements) : fvf_(0), slot_mask_(0) {
    const D3DVERTEXELEMENT9 *elem = pElements;
    while (elem->Stream != 0xFF) {
      elements_.push_back(*elem);
      slot_mask_ |= (1u << elem->Stream);
      elem++;
    }
    // Add the end sentinel
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    elements_.push_back(end);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
      *ppvObj = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **) final { return D3DERR_INVALIDCALL; }

  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) final {
    if (!pNumElements) return D3DERR_INVALIDCALL;
    *pNumElements = (UINT)elements_.size();
    if (pElement)
      memcpy(pElement, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
    return S_OK;
  }

  const std::vector<D3DVERTEXELEMENT9> &elements() const { return elements_; }
  uint32_t slotMask() const { return slot_mask_; }
  DWORD fvf() const { return fvf_; }
  void setFVF(DWORD fvf) { fvf_ = fvf; }

  static Com<D3D9VertexDeclaration> CreateFromFVF(DWORD FVF) {
    D3DVERTEXELEMENT9 elements[16];
    uint32_t elemCount = 0;

    switch (FVF & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ:
      elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
      elements[elemCount].Usage = D3DDECLUSAGE_POSITION;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
      break;
    case D3DFVF_XYZB1:
    case D3DFVF_XYZB2:
    case D3DFVF_XYZB3:
    case D3DFVF_XYZB4:
    case D3DFVF_XYZB5: {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
      elements[elemCount].Usage = D3DDECLUSAGE_POSITION;
      elements[elemCount].UsageIndex = 0;
      elemCount++;

      uint32_t betas = (((FVF & D3DFVF_XYZB5) - D3DFVF_XYZB1) >> 1) + 1;
      uint8_t betaIdx = 0xFF;
      if (FVF & D3DFVF_LASTBETA_D3DCOLOR)
        betaIdx = D3DDECLTYPE_D3DCOLOR;
      else if (FVF & D3DFVF_LASTBETA_UBYTE4)
        betaIdx = D3DDECLTYPE_UBYTE4;
      else if ((FVF & D3DFVF_XYZB5) == D3DFVF_XYZB5)
        betaIdx = D3DDECLTYPE_FLOAT1;

      if (betaIdx != 0xFF)
        betas--;

      if (betas > 0) {
        switch (betas) {
        case 1: elements[elemCount].Type = D3DDECLTYPE_FLOAT1; break;
        case 2: elements[elemCount].Type = D3DDECLTYPE_FLOAT2; break;
        case 3: elements[elemCount].Type = D3DDECLTYPE_FLOAT3; break;
        case 4: elements[elemCount].Type = D3DDECLTYPE_FLOAT4; break;
        default: break;
        }
        elements[elemCount].Usage = D3DDECLUSAGE_BLENDWEIGHT;
        elements[elemCount].UsageIndex = 0;
        elemCount++;
      }

      if (betaIdx != 0xFF) {
        elements[elemCount].Type = betaIdx;
        elements[elemCount].Usage = D3DDECLUSAGE_BLENDINDICES;
        elements[elemCount].UsageIndex = 0;
        elemCount++;
      }
      break;
    }
    case D3DFVF_XYZW:
    case D3DFVF_XYZRHW:
      elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
      elements[elemCount].Usage =
          ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZW)
              ? D3DDECLUSAGE_POSITION
              : D3DDECLUSAGE_POSITIONT;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
      break;
    default:
      break;
    }

    if (FVF & D3DFVF_NORMAL) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
      elements[elemCount].Usage = D3DDECLUSAGE_NORMAL;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_PSIZE) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
      elements[elemCount].Usage = D3DDECLUSAGE_PSIZE;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_DIFFUSE) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_SPECULAR) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 1;
      elemCount++;
    }

    uint32_t texCount = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (texCount > 8) texCount = 8;

    for (uint32_t i = 0; i < texCount; i++) {
      switch ((FVF >> (16 + i * 2)) & 0x3) {
      case D3DFVF_TEXTUREFORMAT1:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
        break;
      case D3DFVF_TEXTUREFORMAT2:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT2;
        break;
      case D3DFVF_TEXTUREFORMAT3:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
        break;
      case D3DFVF_TEXTUREFORMAT4:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
        break;
      default:
        break;
      }
      elements[elemCount].Usage = D3DDECLUSAGE_TEXCOORD;
      elements[elemCount].UsageIndex = i;
      elemCount++;
    }

    // Fill in stream, offset, method
    for (uint32_t i = 0; i < elemCount; i++) {
      elements[i].Stream = 0;
      elements[i].Offset = (i == 0)
          ? 0
          : (elements[i - 1].Offset + GetDecltypeSize((D3DDECLTYPE)elements[i - 1].Type));
      elements[i].Method = D3DDECLMETHOD_DEFAULT;
    }

    // Add end sentinel
    elements[elemCount] = {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};

    auto decl = new D3D9VertexDeclaration(elements);
    decl->setFVF(FVF);
    return Com(decl);
  }

private:
  std::vector<D3DVERTEXELEMENT9> elements_;
  DWORD fvf_;
  uint32_t slot_mask_;
};

} // namespace dxmt
