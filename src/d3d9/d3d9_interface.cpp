
#include "d3d9_interface.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace dxmt {

// Guaranteed-flush file logging (survives crashes)
static FILE *rawlog_file() {
  static FILE *f = nullptr;
  if (!f) { f = fopen("C:\\dxmt_raw.log", "a"); }
  return f;
}
static void rawlog(const char *msg) {
  FILE *f = rawlog_file();
  if (f) { fputs(msg, f); fflush(f); }
}
static void rawlogf(const char *fmt, ...) {
  FILE *f = rawlog_file();
  if (f) {
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
  }
}

D3D9Interface::D3D9Interface() {
  Logger::info("D3D9Interface CREATED");
}

D3D9Interface::~D3D9Interface() {
  Logger::info("D3D9Interface DESTROYED");
}

ULONG STDMETHODCALLTYPE D3D9Interface::AddRef() {
  ULONG r = m_refCount.fetch_add(1) + 1;
  Logger::info(str::format("D3D9Interface::AddRef refcount=", r));
  return r;
}

ULONG STDMETHODCALLTYPE D3D9Interface::Release() {
  ULONG before = m_refCount.load();
  if (before <= 1) {
    // Singleton: never self-delete. The D3D9Interface is kept alive by g_d3d9_singleton.
    Logger::info(str::format("D3D9Interface::Release CLAMPED refcount=", before));
    return 1;
  }
  ULONG r = m_refCount.fetch_sub(1) - 1;
  Logger::info(str::format("D3D9Interface::Release refcount ", before, " -> ", r));
  // Never delete — singleton
  return r;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::QueryInterface(REFIID riid, void **ppvObj) {
  if (!ppvObj)
    return E_POINTER;

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
    *ppvObj = ref(this);
    return S_OK;
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::RegisterSoftwareDevice(void *) {
  return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE D3D9Interface::GetAdapterCount() {
  rawlog("GetAdapterCount -> 1\n");
  return 1;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetAdapterIdentifier(
    UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  Logger::info(str::format("GetAdapterIdentifier adapter=", Adapter, " flags=0x", std::hex, Flags));
  rawlogf("GetAdapterIdentifier adapter=%u flags=0x%lx\n", Adapter, (unsigned long)Flags);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  memset(pIdentifier, 0, sizeof(*pIdentifier));
  strcpy(pIdentifier->Driver, "dxmt");

  auto devices = WMT::CopyAllDevices();
  Logger::info(str::format("GetAdapterIdentifier: CopyAllDevices returned ", devices.count(), " devices"));
  if (devices.count() == 0) {
    Logger::err("GetAdapterIdentifier: No Metal devices found!");
    return D3DERR_NOTAVAILABLE;
  }
  auto device = devices.object(0);
  auto name = device.name().getUTF8String();
  strncpy(pIdentifier->Description, name.c_str(), sizeof(pIdentifier->Description) - 1);
  Logger::info(str::format("GetAdapterIdentifier: device='", name.c_str(), "'"));

  pIdentifier->VendorId = 0x8086; // Intel (GTA IV requires Intel/AMD, crashes with unknown vendors)
  pIdentifier->DeviceId = (DWORD)(device.registryID() & 0xFFFF);

  // Fill in realistic values that games may rely on
  pIdentifier->DriverVersion.QuadPart = INT64_MAX; // DXVK pattern: report max version
  pIdentifier->WHQLLevel = 1; // "WHQL certified" — some games require this

  // Generate a stable DeviceIdentifier GUID from the Metal registryID
  // Games may use this as a cache key or device identifier
  uint64_t regId = device.registryID();
  pIdentifier->DeviceIdentifier.Data1 = 0xD3D9D3D9; // recognizable prefix
  pIdentifier->DeviceIdentifier.Data2 = (WORD)(regId & 0xFFFF);
  pIdentifier->DeviceIdentifier.Data3 = (WORD)((regId >> 16) & 0xFFFF);
  memcpy(pIdentifier->DeviceIdentifier.Data4, "DXMT_GPU", 8);

  Logger::info(str::format("GetAdapterIdentifier -> OK vendor=0x", std::hex, pIdentifier->VendorId,
               " device=0x", pIdentifier->DeviceId, " regId=", regId));
  rawlogf("GetAdapterIdentifier -> OK vendor=0x%04lx device=0x%04lx\n",
           (unsigned long)pIdentifier->VendorId, (unsigned long)pIdentifier->DeviceId);
  return S_OK;
}

static uint32_t getFormatBpp(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    return 32;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return 16;
  default:
    return 0;
  }
}

// Deduplicated mode list: macOS reports 100+ modes with HiDPI variants and
// unusual refresh rates. Games expect ~20-30 modes with standard resolutions.
// We keep one entry per unique (width, height) — the highest refresh rate.
struct DeduplicatedMode {
  UINT width, height, refreshRate;
};

static std::vector<DeduplicatedMode> buildDeduplicatedModes(uint32_t bpp) {
  HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  wsi::WsiMode wsiMode;

  // Collect all modes, dedup by (width, height) keeping max refresh rate
  std::vector<DeduplicatedMode> modes;
  for (uint32_t i = 0; wsi::getDisplayMode(monitor, i, &wsiMode); i++) {
    if (wsiMode.bitsPerPixel != bpp)
      continue;
    UINT rate = (wsiMode.refreshRate.denominator != 0)
        ? wsiMode.refreshRate.numerator / wsiMode.refreshRate.denominator
        : 60;
    if (rate == 0) rate = 60;
    // Check if we already have this resolution
    bool found = false;
    for (auto &m : modes) {
      if (m.width == wsiMode.width && m.height == wsiMode.height) {
        if (rate > m.refreshRate)
          m.refreshRate = rate;
        found = true;
        break;
      }
    }
    if (!found) {
      modes.push_back({wsiMode.width, wsiMode.height, rate});
    }
  }
  // Sort by resolution (width * height ascending, then width ascending)
  std::sort(modes.begin(), modes.end(), [](const DeduplicatedMode &a, const DeduplicatedMode &b) {
    uint64_t aPixels = (uint64_t)a.width * a.height;
    uint64_t bPixels = (uint64_t)b.width * b.height;
    if (aPixels != bPixels) return aPixels < bPixels;
    return a.width < b.width;
  });
  return modes;
}

static UINT enumerateMatchingModes(D3DFORMAT Format, UINT targetIndex, D3DDISPLAYMODE *pOut) {
  uint32_t bpp = getFormatBpp(Format);
  if (bpp == 0)
    return 0;

  auto modes = buildDeduplicatedModes(bpp);
  UINT count = (UINT)modes.size();

  if (pOut && targetIndex < count) {
    pOut->Width = modes[targetIndex].width;
    pOut->Height = modes[targetIndex].height;
    pOut->RefreshRate = modes[targetIndex].refreshRate;
    pOut->Format = Format;
  }

  return count;
}

UINT STDMETHODCALLTYPE D3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  if (Adapter != 0)
    return 0;
  UINT count = enumerateMatchingModes(Format, UINT_MAX, nullptr);
  rawlogf("GetAdapterModeCount fmt=%d -> %u\n", (int)Format, count);
  return count;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::EnumAdapterModes(
    UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode)
    return D3DERR_INVALIDCALL;

  UINT count = enumerateMatchingModes(Format, Mode, pMode);
  if (Mode >= count) {
    rawlogf("EnumAdapterModes fmt=%d mode=%u -> INVALIDCALL (count=%u)\n", (int)Format, Mode, count);
    return D3DERR_INVALIDCALL;
  }

  rawlogf("EnumAdapterModes fmt=%d mode=%u -> %ux%u@%u\n",
           (int)Format, Mode, pMode->Width, pMode->Height, pMode->RefreshRate);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  rawlog("GetAdapterDisplayMode\n");
  if (Adapter != 0 || !pMode)
    return D3DERR_INVALIDCALL;

  HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  wsi::WsiMode mode;
  if (wsi::getCurrentDisplayMode(monitor, &mode)) {
    pMode->Width = mode.width;
    pMode->Height = mode.height;
    pMode->RefreshRate = (mode.refreshRate.denominator != 0)
        ? mode.refreshRate.numerator / mode.refreshRate.denominator
        : 60;
  } else {
    pMode->Width = 1920;
    pMode->Height = 1080;
    pMode->RefreshRate = 60;
  }
  pMode->Format = D3DFMT_X8R8G8B8;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) {
  Logger::info(str::format("CheckDeviceType devtype=", (int)DevType, " afmt=", (int)AdapterFormat,
               " bbfmt=", (int)BackBufferFormat, " win=", Windowed));
  rawlogf("CheckDeviceType adapter=%u devtype=%d afmt=%d bbfmt=%d win=%d\n",
           Adapter, (int)DevType, (int)AdapterFormat, (int)BackBufferFormat, Windowed);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  // Only support HAL (hardware) device type — REF/SW rasterizers don't exist
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
  Logger::info(str::format("CheckDeviceFormat fmt=", (int)CheckFormat, " usage=0x", std::hex, Usage, " rtype=", std::dec, (int)RType));
  rawlogf("CheckDeviceFormat fmt=%d usage=0x%lx rtype=%d\n", (int)CheckFormat, (unsigned long)Usage, (int)RType);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;

  // Accept volume textures for capability queries even though we don't fully implement them.
  // Games may take error paths if no volume texture formats are reported.

  // Reject proprietary depth-as-texture formats (DF24, DF16, INTZ, RAWZ, NULL)
  // GTA IV: disabling DF formats forces better mirror render path (DXVK precedent)
  constexpr D3DFORMAT D3DFMT_DF24 = (D3DFORMAT)MAKEFOURCC('D','F','2','4');
  constexpr D3DFORMAT D3DFMT_DF16 = (D3DFORMAT)MAKEFOURCC('D','F','1','6');
  constexpr D3DFORMAT D3DFMT_INTZ = (D3DFORMAT)MAKEFOURCC('I','N','T','Z');
  constexpr D3DFORMAT D3DFMT_RAWZ = (D3DFORMAT)MAKEFOURCC('R','A','W','Z');
  constexpr D3DFORMAT D3DFMT_NULL_RT = (D3DFORMAT)MAKEFOURCC('N','U','L','L');
  if (CheckFormat == D3DFMT_DF24 || CheckFormat == D3DFMT_DF16 ||
      CheckFormat == D3DFMT_INTZ || CheckFormat == D3DFMT_RAWZ ||
      CheckFormat == D3DFMT_NULL_RT)
    return D3DERR_NOTAVAILABLE;

  // For depth/stencil usage, accept known depth formats
  if (Usage & D3DUSAGE_DEPTHSTENCIL) {
    switch (CheckFormat) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
      return S_OK;
    default:
      return D3DERR_NOTAVAILABLE;
    }
  }

  // For render target usage, also accept depth formats
  if (Usage & D3DUSAGE_RENDERTARGET) {
    // Depth formats are valid render targets
    switch (CheckFormat) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
      return S_OK;
    default:
      break;
    }
  }

  // For regular textures/surfaces, check if format is mappable
  if (ConvertD3D9Format(CheckFormat) == WMTPixelFormatInvalid) {
    rawlogf("  -> NOTAVAILABLE (unmapped format %d)\n", (int)CheckFormat);
    return D3DERR_NOTAVAILABLE;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels) {
  rawlogf("CheckDeviceMultiSampleType fmt=%d ms=%d\n", (int)SurfaceFormat, (int)MultiSampleType);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  if (pQualityLevels)
    *pQualityLevels = 1;
  if (MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_NOTAVAILABLE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
  rawlogf("CheckDepthStencilMatch afmt=%d rt=%d ds=%d\n", (int)AdapterFormat, (int)RenderTargetFormat, (int)DepthStencilFormat);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
  rawlogf("CheckDeviceFormatConversion src=%d dst=%d\n", (int)SourceFormat, (int)TargetFormat);
  if (Adapter != 0)
    return D3DERR_INVALIDCALL;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Interface::GetDeviceCaps(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  Logger::info("GetDeviceCaps called");
  rawlog("GetDeviceCaps\n");
  if (Adapter != 0 || !pCaps)
    return D3DERR_INVALIDCALL;

  memset(pCaps, 0, sizeof(*pCaps));
  pCaps->DeviceType = D3DDEVTYPE_HAL;
  pCaps->AdapterOrdinal = 0;

  // -- General device caps --
  pCaps->Caps = 0;
  pCaps->Caps2 = D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
  pCaps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
  pCaps->CursorCaps = D3DCURSORCAPS_COLOR;
  pCaps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                   D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                   D3DDEVCAPS_PUREDEVICE | D3DDEVCAPS_DRAWPRIMITIVES2 |
                   D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY;

  // -- Primitive/raster caps --
  pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                             D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP |
                             D3DPMISCCAPS_MASKZ;
  // Vertex fog only (no table fog); no MSAA
  pCaps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_SCISSORTEST |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS;

  // -- Comparison caps (all 8 funcs implemented via ConvertCompareFunc) --
  pCaps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                    D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL |
                    D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  pCaps->AlphaCmpCaps = pCaps->ZCmpCaps;

  // -- Blend caps (only factors mapped in ConvertBlendFactor) --
  pCaps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR |
                        D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
                        D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR |
                        D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT |
                        D3DPBLENDCAPS_BLENDFACTOR;
  pCaps->DestBlendCaps = pCaps->SrcBlendCaps;

  // -- Shade caps (FF VS generates Gouraud interpolation; specular + fog in FF PS) --
  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

  // -- Texture caps (cube maps supported, no volume textures) --
  pCaps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_POW2 |
                       D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_PERSPECTIVE |
                       D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;

  // -- Texture filter caps (min/mag/mip all mapped in ConvertD3D9SamplerState) --
  pCaps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                             D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFPOINT |
                             D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                             D3DPTFILTERCAPS_MIPFLINEAR;
  // Cube texture filter caps (same as 2D)
  pCaps->CubeTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFPOINT |
                                  D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                                  D3DPTFILTERCAPS_MIPFLINEAR;
  // No volume texture support, leave VolumeTextureFilterCaps at 0

  // -- Texture address caps (MIRRORONCE not mapped in convertAddr, omit it) --
  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
                              D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
                              D3DPTADDRESSCAPS_INDEPENDENTUV;

  // -- Stencil caps (all 8 ops mapped via ConvertStencilOp; no two-sided — back=front always) --
  pCaps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE |
                       D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT |
                       D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;

  // -- Texture stage operation caps (all ops listed in FFPSKey handling) --
  pCaps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
                         D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                         D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                         D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
                         D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
                         D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                         D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA |
                         D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA |
                         D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
                         D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
                         D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                         D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA |
                         D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE |
                         D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD |
                         D3DTEXOPCAPS_LERP;

  // -- Texture stage/sampler limits (8 stages initialized in device, 16 sampler slots bound) --
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;

  // -- Texture dimension limits (Metal supports 16384) --
  pCaps->MaxTextureWidth = 16384;
  pCaps->MaxTextureHeight = 16384;
  pCaps->MaxTextureRepeat = 8192;
  pCaps->MaxTextureAspectRatio = 16384;
  pCaps->MaxAnisotropy = 16;
  pCaps->MaxVertexW = 1e10f;

  // -- Line caps (basic line primitives work with same PSO as triangles) --
  pCaps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND |
                    D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

  // -- FVF caps (8 texture coordinates supported in vertex declaration) --
  pCaps->FVFCaps = (8 & D3DFVFCAPS_TEXCOORDCOUNTMASK) | D3DFVFCAPS_PSIZE;

  // -- Vertex processing (no texgen, no clip planes — both are stubs) --
  pCaps->VertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 |
                                D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS |
                                D3DVTXPCAPS_LOCALVIEWER;
  pCaps->MaxActiveLights = 8;
  pCaps->MaxUserClipPlanes = 0;
  pCaps->MaxVertexBlendMatrices = 4;
  pCaps->MaxVertexBlendMatrixIndex = 0;
  pCaps->MaxPointSize = 1.0f; // no point size control implemented
  pCaps->MaxPrimitiveCount = 0x00555555;

  // -- Shader caps --
  pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  pCaps->MaxVertexShaderConst = 256;
  pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  pCaps->PixelShader1xMaxValue = 65504.0f;

  // -- Draw limits --
  pCaps->MaxVertexIndex = 0x00FFFFFF;
  pCaps->MaxStreams = 16;
  pCaps->MaxStreamStride = 508;
  pCaps->NumSimultaneousRTs = 1; // only backbuffer RT functional

  Logger::info("GetDeviceCaps -> OK");
  rawlog("GetDeviceCaps -> OK\n");
  return S_OK;
}

HMONITOR STDMETHODCALLTYPE D3D9Interface::GetAdapterMonitor(UINT Adapter) {
  rawlogf("GetAdapterMonitor adapter=%u\n", Adapter);
  return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT STDMETHODCALLTYPE D3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice9 **ppReturnedDeviceInterface) {
  Logger::info(str::format("CreateDevice adapter=", Adapter, " devtype=", (int)DeviceType, " flags=0x", std::hex, BehaviorFlags));
  rawlogf("CreateDevice adapter=%u devtype=%d hwnd=%p flags=0x%lx\n",
           Adapter, (int)DeviceType, (void*)hFocusWindow, (unsigned long)BehaviorFlags);
  if (Adapter != 0 || !pPresentationParameters || !ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;

  *ppReturnedDeviceInterface = nullptr;

  try {
    *ppReturnedDeviceInterface = new D3D9Device(this, hFocusWindow, pPresentationParameters);
    return S_OK;
  } catch (...) {
    Logger::err("D3D9: Failed to create device");
    return D3DERR_INVALIDCALL;
  }
}

} // namespace dxmt
