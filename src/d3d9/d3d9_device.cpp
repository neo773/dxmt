#include <cstdarg>
#include "d3d9_device.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_format.hpp"
#include "d3d9_query.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_stateblock.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "airconv_public.h"
#include "config/config.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_shader_cache.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "util_error.hpp"
#include "wsi_platform.hpp"
#include "wsi_monitor.hpp"
#include "wsi_window.hpp"

static void trace_file(const char* msg) {
  HANDLE h = CreateFileA("C:\\windows\\temp\\d3d9_trace.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written;
    WriteFile(h, msg, strlen(msg), &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
  }
}

namespace dxmt {

static D3DMATRIX IdentityMatrix() {
  D3DMATRIX m = {};
  m._11 = m._22 = m._33 = m._44 = 1.0f;
  return m;
}

static D3DMATRIX MultiplyMatrices(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX r = {};
  const float *A = &a._11;
  const float *B = &b._11;
  float *R = &r._11;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++)
        R[i * 4 + j] += A[i * 4 + k] * B[k * 4 + j];
  return r;
}

uint32_t D3D9Device::TransformIndex(D3DTRANSFORMSTATETYPE state) {
  // D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0-7 = 16-23
  // D3DTS_WORLD = 256 (D3DTS_WORLDMATRIX(0))
  // D3DTS_WORLDMATRIX(n) = 256 + n
  if (state < 512) return (uint32_t)state;
  return 0;
}

D3D9Device::D3D9Device(IDirect3D9 *pD3D9, HWND hFocusWindow, D3DPRESENT_PARAMETERS *pParams)
    : d3d9_(pD3D9), hwnd_(hFocusWindow), present_params_(*pParams),
      hud_(WMT::DeveloperHUDProperties::instance()) {

  // Use the device window if focus window is not set
  if (!hwnd_)
    hwnd_ = pParams->hDeviceWindow;

  Logger::info("D3D9Device: creating Metal device");

  // Create DXMT device (wraps MTLDevice + CommandQueue)
  auto devices = WMT::CopyAllDevices();
  auto mtl_device = devices.object(0);
  dxmt_device_ = CreateDXMTDevice({.device = mtl_device});

  // Initialize dynamic buffer ring allocator (4MB blocks, shared storage)
  dynamic_buffer_ring_.emplace(StagingBufferBlockAllocator{
      mtl_device, WMTResourceOptionCPUCacheModeWriteCombined |
                  WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModeShared});

  // Initialize async PSO compilation scheduler
  pso_scheduler_.emplace();

  // Preload: compile all cached shaders to MTLLibrary ahead of first use
  ShaderCache::getInstance(dxmt_device_->metalVersion())
      .preload(dxmt_device_->device());

  // Create Metal view + layer from HWND
  native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hwnd_, mtl_device, layer_weak_);
  if (!native_view_) {
    Logger::err("D3D9: Failed to create metal view");
    throw MTLD3DError("Failed to create metal view");
  }

  // Create presenter for swap chain
  presenter_ = Rc(new Presenter(
      mtl_device, layer_weak_,
      dxmt_device_->queue().cmd_library,
      1.0f, 1));

#ifdef DXMT_PERF
  // 15 lines: 4 DXMT shared + 1 D3D9 header + 10 D3D9 specific
  hud_.initialize("------- DXMT -------", 14, "com.github.3shain.dxmt-d3d9");
#endif

  // Query display refresh rate
  HMONITOR monitor = wsi::getWindowMonitor(hwnd_);
  wsi::WsiMode current_mode;
  if (wsi::getCurrentDisplayMode(monitor, &current_mode) &&
      current_mode.refreshRate.denominator != 0 &&
      current_mode.refreshRate.numerator != 0) {
    init_refresh_rate_ = (double)current_mode.refreshRate.numerator /
                         (double)current_mode.refreshRate.denominator;
  }

  // Get window size if not specified
  if (present_params_.BackBufferWidth == 0 || present_params_.BackBufferHeight == 0)
    wsi::getWindowSize(hwnd_, &present_params_.BackBufferWidth, &present_params_.BackBufferHeight);

  Logger::info(str::format("D3D9Device: backbuffer ",
      present_params_.BackBufferWidth, "x", present_params_.BackBufferHeight,
      " ", D3D9FormatName(present_params_.BackBufferFormat),
      " depth=", D3D9FormatName(present_params_.AutoDepthStencilFormat),
      " windowed=", present_params_.Windowed,
      " buffers=", present_params_.BackBufferCount,
      " swap=", present_params_.SwapEffect,
      " msaa=", (int)present_params_.MultiSampleType));

  // Configure layer properties for presentation
  presenter_->changeLayerProperties(
      WMTPixelFormatBGRA8Unorm, WMTColorSpaceSRGB,
      present_params_.BackBufferWidth, present_params_.BackBufferHeight, 1);

  // Create backbuffer
  CreateBackbuffer(present_params_.BackBufferWidth, present_params_.BackBufferHeight);

  // (snapshot ring removed — emit-on-draw copies constants directly)

  // Initialize current render target to backbuffer
  current_rt_iface_[0] = Com<IDirect3DSurface9>(backbuffer_surface_.ptr());
  current_rt_[0] = backbuffer_;
  current_rt_view_[0] = backbuffer_view_;
  current_rt_format_[0] = WMTPixelFormatBGRA8Unorm;

  // Auto-create depth stencil if requested
  if (present_params_.EnableAutoDepthStencil) {
    IDirect3DSurface9 *depthSurf = nullptr;
    CreateDepthStencilSurface(
        present_params_.BackBufferWidth, present_params_.BackBufferHeight,
        present_params_.AutoDepthStencilFormat,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depthSurf, nullptr);
    if (depthSurf) {
      SetDepthStencilSurface(depthSurf);
      depthSurf->Release();
    }
  }

  // Initialize default render states
  render_states_[D3DRS_ZENABLE] = present_params_.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
  render_states_[D3DRS_ZWRITEENABLE] = TRUE;
  render_states_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  render_states_[D3DRS_ALPHABLENDENABLE] = FALSE;
  render_states_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  render_states_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  render_states_[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  render_states_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  render_states_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  render_states_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  render_states_[D3DRS_CULLMODE] = D3DCULL_CCW;
  render_states_[D3DRS_FILLMODE] = D3DFILL_SOLID;
  render_states_[D3DRS_COLORWRITEENABLE] = 0xF;
  render_states_[D3DRS_LIGHTING] = TRUE;
  render_states_[D3DRS_SPECULARENABLE] = FALSE;
  render_states_[D3DRS_AMBIENT] = 0;
  render_states_[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  render_states_[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  render_states_[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_states_[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_states_[D3DRS_COLORVERTEX] = TRUE;
  render_states_[D3DRS_NORMALIZENORMALS] = FALSE;
  render_states_[D3DRS_FOGCOLOR] = 0;
  render_states_[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  render_states_[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  render_states_[D3DRS_FOGENABLE] = FALSE;
  render_states_[D3DRS_ALPHATESTENABLE] = FALSE;
  render_states_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  render_states_[D3DRS_ALPHAREF] = 0;
  render_states_[D3DRS_STENCILENABLE] = FALSE;
  render_states_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  render_states_[D3DRS_STENCILREF] = 0;
  render_states_[D3DRS_STENCILMASK] = 0xFFFFFFFF;
  render_states_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  render_states_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  render_states_[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  render_states_[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render_states_[D3DRS_SCISSORTESTENABLE] = FALSE;
  render_states_[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;

  // Set default viewport
  viewport_.X = 0;
  viewport_.Y = 0;
  viewport_.Width = present_params_.BackBufferWidth;
  viewport_.Height = present_params_.BackBufferHeight;
  viewport_.MinZ = 0.0f;
  viewport_.MaxZ = 1.0f;

  // Initialize transforms to identity
  D3DMATRIX identity = IdentityMatrix();
  for (auto &t : transforms_)
    t = identity;

  // Initialize default texture stage states
  for (DWORD stage = 0; stage < 8; stage++) {
    texture_stage_states_[stage][D3DTSS_COLOROP] = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    texture_stage_states_[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    texture_stage_states_[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_ALPHAOP] = (stage == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    texture_stage_states_[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    texture_stage_states_[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_TEXCOORDINDEX] = stage;
    texture_stage_states_[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    texture_stage_states_[stage][D3DTSS_COLORARG0] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    texture_stage_states_[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
  }

  // Create default 1x1 white texture for unbound sampler fallback
  {
    WMTTextureInfo whiteInfo = {};
    whiteInfo.pixel_format = WMTPixelFormatBGRA8Unorm;
    whiteInfo.width = 1;
    whiteInfo.height = 1;
    whiteInfo.depth = 1;
    whiteInfo.array_length = 1;
    whiteInfo.type = WMTTextureType2D;
    whiteInfo.mipmap_level_count = 1;
    whiteInfo.sample_count = 1;
    whiteInfo.usage = WMTTextureUsageShaderRead;
    whiteInfo.options = WMTResourceStorageModeShared;
    default_white_tex_ = Rc(new Texture(whiteInfo, dxmt_device_->device()));
    default_white_tex_->rename(default_white_tex_->allocate({}));
    TextureViewDescriptor whiteViewDesc = {
        .format = WMTPixelFormatBGRA8Unorm,
        .type = WMTTextureType2D,
        .firstMiplevel = 0,
        .miplevelCount = 1,
        .firstArraySlice = 0,
        .arraySize = 1,
    };
    default_white_view_ = default_white_tex_->createView(whiteViewDesc);
    // Fill with opaque white (BGRA)
    auto whiteTex = default_white_tex_->current()->texture();
    uint32_t whitePixel = 0xFFFFFFFF;
    WMTOrigin origin = {0, 0, 0};
    WMTSize size = {1, 1, 1};
    whiteTex.replaceRegion(origin, size, 0, 0, &whitePixel, 4, 0);
    // Create a default sampler state for unbound slots
    WMTSamplerInfo defaultSampInfo = {};
    defaultSampInfo.min_filter = WMTSamplerMinMagFilterNearest;
    defaultSampInfo.mag_filter = WMTSamplerMinMagFilterNearest;
    defaultSampInfo.mip_filter = WMTSamplerMipFilterNotMipmapped;
    defaultSampInfo.s_address_mode = WMTSamplerAddressModeClampToEdge;
    defaultSampInfo.t_address_mode = WMTSamplerAddressModeClampToEdge;
    defaultSampInfo.r_address_mode = WMTSamplerAddressModeClampToEdge;
    defaultSampInfo.max_anisotroy = 1;
    defaultSampInfo.normalized_coords = true;
    defaultSampInfo.support_argument_buffers = true;
    default_sampler_ = MTLDevice_newSamplerState(dxmt_device_->device().handle, &defaultSampInfo);
    default_sampler_gpu_id_ = defaultSampInfo.gpu_resource_id;
  }

  // Initialize default material
  material_.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  material_.Ambient = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Specular = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Emissive = {0.0f, 0.0f, 0.0f, 0.0f};
  material_.Power = 0.0f;

  cursor_scale_ = Config::getInstance().getOption<int>("d3d9.cursorScale", 1);
  if (cursor_scale_ < 1) cursor_scale_ = 1;
  if (cursor_scale_ > 8) cursor_scale_ = 8;

  Logger::info("D3D9Device: BUILD_V2_GETDEVICE_FIX created successfully");
  trace_file("DEVICE CREATED");
}

D3D9Device::~D3D9Device() {
  Logger::info("TRACE: destructor called");
  for (auto &[_, hCursor] : cursor_cache_)
    DestroyCursor(hCursor);
  if (native_view_)
    WMT::ReleaseMetalView(native_view_);
  Logger::info("D3D9Device: destroyed");
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  if (!pCursorBitmap)
    return D3DERR_INVALIDCALL;

  D3DSURFACE_DESC desc;
  if (FAILED(pCursorBitmap->GetDesc(&desc)))
    return D3DERR_INVALIDCALL;

  D3DLOCKED_RECT lr;
  if (FAILED(pCursorBitmap->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    return D3DERR_INVALIDCALL;

  UINT w = desc.Width;
  UINT h = desc.Height;

  // FNV-1a hash to skip redundant cursor updates
  uint64_t hash = 14695981039346656037ull;
  auto fnv_mix = [&](const void *data, size_t len) {
    auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++) {
      hash ^= p[i];
      hash *= 1099511628211ull;
    }
  };
  fnv_mix(&XHotSpot, sizeof(XHotSpot));
  fnv_mix(&YHotSpot, sizeof(YHotSpot));
  fnv_mix(&w, sizeof(w));
  fnv_mix(&h, sizeof(h));
  auto *src = static_cast<const uint8_t *>(lr.pBits);
  for (UINT y = 0; y < h; y++)
    fnv_mix(src + y * lr.Pitch, w * 4);

  if (hash == cursor_hash_) {
    pCursorBitmap->UnlockRect();
    return S_OK;
  }
  cursor_hash_ = hash;

  // Check cursor cache — avoids recreating HCURSOR for previously seen icons
  auto it = cursor_cache_.find(hash);
  if (it != cursor_cache_.end()) {
    pCursorBitmap->UnlockRect();
    cursor_handle_ = it->second;
    if (cursor_visible_)
      SetCursor(cursor_handle_);
    return S_OK;
  }

  int scale = cursor_scale_;
  UINT sw = w * scale;
  UINT sh = h * scale;

  // Build BGRA pixel buffer at final size
  std::vector<uint32_t> pixels(sw * sh);

  if (scale == 1) {
    auto *src = static_cast<const uint8_t *>(lr.pBits);
    for (UINT y = 0; y < h; y++) {
      memcpy(&pixels[y * sw], src + y * lr.Pitch, w * 4);
    }
  } else {
    auto *src = static_cast<const uint8_t *>(lr.pBits);
    for (UINT y = 0; y < sh; y++) {
      auto *srcRow = reinterpret_cast<const uint32_t *>(src + (y / scale) * lr.Pitch);
      for (UINT x = 0; x < sw; x++) {
        pixels[y * sw + x] = srcRow[x / scale];
      }
    }
  }

  pCursorBitmap->UnlockRect();

  // AND mask: all zeros = entire image is used (no transparency from mask)
  UINT maskSize = ((sw + 31) / 32) * 4 * sh;
  std::vector<uint8_t> andMask(maskSize, 0);

  HBITMAP hColor = CreateBitmap(sw, sh, 1, 32, pixels.data());
  HBITMAP hMask = CreateBitmap(sw, sh, 1, 1, andMask.data());

  ICONINFO ii = {};
  ii.fIcon = FALSE;
  ii.xHotspot = XHotSpot * scale;
  ii.yHotspot = YHotSpot * scale;
  ii.hbmMask = hMask;
  ii.hbmColor = hColor;

  HCURSOR newCursor = (HCURSOR)CreateIconIndirect(&ii);

  DeleteObject(hColor);
  DeleteObject(hMask);

  if (!newCursor)
    return D3DERR_INVALIDCALL;

  cursor_cache_[hash] = newCursor;
  cursor_handle_ = newCursor;

  if (cursor_visible_)
    SetCursor(cursor_handle_);

  return S_OK;
}

void STDMETHODCALLTYPE D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  POINT currentPos = {};
  if (GetCursorPos(&currentPos) && currentPos.x == X && currentPos.y == Y)
    return;
  SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE D3D9Device::ShowCursor(BOOL bShow) {
  BOOL prev = cursor_visible_;
  cursor_visible_ = bShow;
  SetCursor(cursor_visible_ ? cursor_handle_ : nullptr);
  return prev;
}

HRESULT D3D9Device::CreateBackbuffer(UINT width, UINT height) {
  WMTTextureInfo info = {};
  info.pixel_format = WMTPixelFormatBGRA8Unorm;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  info.options = WMTResourceStorageModePrivate;

  backbuffer_ = Rc(new Texture(info, dxmt_device_->device()));
  backbuffer_->rename(backbuffer_->allocate({}));

  backbuffer_view_ = backbuffer_->createView({
      .format = WMTPixelFormatBGRA8Unorm,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  backbuffer_surface_ = new D3D9Surface(this, backbuffer_, backbuffer_view_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::QueryInterface(REFIID riid, void **ppvObj) {
  Logger::err(str::format("TRACE: QueryInterface riid=", riid.Data1));
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
    *ppvObj = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Device::TestCooperativeLevel() { return S_OK; }
UINT STDMETHODCALLTYPE D3D9Device::GetAvailableTextureMem() { return 128 * 1024 * 1024; }
HRESULT STDMETHODCALLTYPE D3D9Device::EvictManagedResources() { return S_OK; }

HRESULT STDMETHODCALLTYPE D3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  *ppD3D9 = d3d9_.ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  Logger::info("TRACE: GetDeviceCaps called");
  return d3d9_->GetDeviceCaps(0, D3DDEVTYPE_HAL, pCaps);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDisplayMode(UINT, D3DDISPLAYMODE *pMode) {
  if (!pMode) return D3DERR_INVALIDCALL;
  pMode->Width = present_params_.BackBufferWidth;
  pMode->Height = present_params_.BackBufferHeight;
  pMode->RefreshRate = 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  if (!pParameters) return D3DERR_INVALIDCALL;
  pParameters->AdapterOrdinal = 0;
  pParameters->DeviceType = D3DDEVTYPE_HAL;
  pParameters->hFocusWindow = hwnd_;
  pParameters->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) {
  Logger::info("TRACE: Reset called");
  if (!pPresentationParameters) return D3DERR_INVALIDCALL;

  // Flush all pending GPU work before destroying resources
  InvalidateCurrentPass();
  dxmt_device_->queue().CommitCurrentChunk();
  dxmt_device_->queue().WaitForIdle();

  present_params_ = *pPresentationParameters;
  if (present_params_.BackBufferWidth == 0 || present_params_.BackBufferHeight == 0)
    wsi::getWindowSize(hwnd_, &present_params_.BackBufferWidth, &present_params_.BackBufferHeight);

  presenter_->changeLayerProperties(
      WMTPixelFormatBGRA8Unorm, WMTColorSpaceSRGB,
      present_params_.BackBufferWidth, present_params_.BackBufferHeight, 1);

  CreateBackbuffer(present_params_.BackBufferWidth, present_params_.BackBufferHeight);

  // Reset current RT to new backbuffer
  current_rt_iface_[0] = Com<IDirect3DSurface9>(backbuffer_surface_.ptr());
  current_rt_[0] = backbuffer_;
  current_rt_view_[0] = backbuffer_view_;
  current_rt_format_[0] = WMTPixelFormatBGRA8Unorm;

  Logger::info(str::format("D3D9Device: Reset backbuffer ",
      present_params_.BackBufferWidth, "x", present_params_.BackBufferHeight,
      " ", D3D9FormatName(present_params_.BackBufferFormat),
      " depth=", D3D9FormatName(present_params_.AutoDepthStencilFormat),
      " windowed=", present_params_.Windowed,
      " msaa=", (int)present_params_.MultiSampleType));

  viewport_.X = 0;
  viewport_.Y = 0;
  viewport_.Width = present_params_.BackBufferWidth;
  viewport_.Height = present_params_.BackBufferHeight;
  viewport_.MinZ = 0.0f;
  viewport_.MaxZ = 1.0f;

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
  Logger::info(str::format("D3D9: GetBackBuffer sc=", iSwapChain, " bb=", iBackBuffer,
      " ptr=", (void*)backbuffer_surface_.ptr()));
  if (iSwapChain != 0 || iBackBuffer != 0 || !ppBackBuffer)
    return D3DERR_INVALIDCALL;
  *ppBackBuffer = ref(backbuffer_surface_.ptr());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  InvalidateCurrentPass();
  if (RenderTargetIndex >= kMaxRenderTargets)
    return D3DERR_INVALIDCALL;
  if (RenderTargetIndex == 0 && !pRenderTarget)
    return D3DERR_INVALIDCALL; // RT0 cannot be null

  if (!pRenderTarget) {
    // Unbind secondary render target
    current_rt_[RenderTargetIndex] = nullptr;
    current_rt_view_[RenderTargetIndex] = 0;
    current_rt_format_[RenderTargetIndex] = WMTPixelFormatInvalid;
    current_rt_iface_[RenderTargetIndex] = nullptr;
    // Recount active RTs
    current_rt_count_ = 1;
    for (uint32_t i = 1; i < kMaxRenderTargets; i++)
      if (current_rt_[i]) current_rt_count_ = i + 1;
    pso_dirty_ = true;
    return S_OK;
  }

  // Determine surface source: 2D texture surface, cube face surface, or standalone surface
  IDirect3DTexture9 *containerTex = nullptr;
  IDirect3DCubeTexture9 *containerCube = nullptr;
  bool isTexSurface = SUCCEEDED(pRenderTarget->GetContainer(__uuidof(IDirect3DTexture9), (void **)&containerTex));
  if (containerTex) containerTex->Release();
  bool isCubeSurface = !isTexSurface && SUCCEEDED(pRenderTarget->GetContainer(__uuidof(IDirect3DCubeTexture9), (void **)&containerCube));
  if (containerCube) containerCube->Release();

  Rc<Texture> rtTex;
  TextureViewKey rtView;
  WMTPixelFormat rtFormat;

  if (isTexSurface) {
    auto *texSurf = static_cast<D3D9TextureSurface *>(pRenderTarget);
    rtTex = texSurf->texture();
    rtView = texSurf->viewKey();
    rtFormat = texSurf->mtlFormat();
  } else if (isCubeSurface) {
    auto *cubeSurf = static_cast<D3D9CubeMapSurface *>(pRenderTarget);
    rtTex = cubeSurf->texture();
    rtView = cubeSurf->viewKey();
    rtFormat = cubeSurf->mtlFormat();
  } else {
    auto *surf = static_cast<D3D9Surface *>(pRenderTarget);
    rtTex = surf->texture();
    rtView = surf->viewKey();
    rtFormat = surf->mtlFormat();
  }

  current_rt_[RenderTargetIndex] = rtTex;
  current_rt_view_[RenderTargetIndex] = rtView;
  current_rt_format_[RenderTargetIndex] = rtFormat;
  current_rt_iface_[RenderTargetIndex] = Com<IDirect3DSurface9>(pRenderTarget);

  // Recount active RTs
  current_rt_count_ = 1;
  for (uint32_t i = 1; i < kMaxRenderTargets; i++)
    if (current_rt_[i]) current_rt_count_ = i + 1;
  pso_dirty_ = true;

  // Only update viewport for RT0
  if (RenderTargetIndex == 0) {
    viewport_.X = 0;
    viewport_.Y = 0;
    viewport_.Width = current_rt_[0]->width();
    viewport_.Height = current_rt_[0]->height();
    viewport_.MinZ = 0.0f;
    viewport_.MaxZ = 1.0f;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
  Logger::info(str::format("D3D9: GetRenderTarget idx=", RenderTargetIndex));
  if (RenderTargetIndex >= kMaxRenderTargets || !ppRenderTarget)
    return D3DERR_INVALIDCALL;
  if (current_rt_iface_[RenderTargetIndex]) {
    current_rt_iface_[RenderTargetIndex]->AddRef();
    *ppRenderTarget = current_rt_iface_[RenderTargetIndex].ptr();
  } else if (RenderTargetIndex == 0) {
    *ppRenderTarget = ref(backbuffer_surface_.ptr());
  } else {
    *ppRenderTarget = nullptr;
    return D3DERR_NOTFOUND;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::BeginScene() { trace_file("BeginScene"); return S_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::EndScene() { Logger::info("TRACE: EndScene"); return S_OK; }

HRESULT STDMETHODCALLTYPE D3D9Device::Clear(
    DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  if (!(Flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
    return S_OK;

  if (Count > 0 && pRects) {
    static bool warned = false;
    if (!warned) {
      Logger::warn("D3D9: Clear with sub-rects not implemented, clearing full surface");
      warned = true;
    }
  }

  InvalidateCurrentPass();

  bool clearColor = (Flags & D3DCLEAR_TARGET) != 0;
  bool clearDepth = (Flags & D3DCLEAR_ZBUFFER) != 0 && depth_stencil_;

  float r = ((Color >> 16) & 0xFF) / 255.0f;
  float g = ((Color >> 8) & 0xFF) / 255.0f;
  float b = ((Color >> 0) & 0xFF) / 255.0f;
  float a = ((Color >> 24) & 0xFF) / 255.0f;

  bool clearStencil = (Flags & D3DCLEAR_STENCIL) != 0 && depth_stencil_;

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  if (clearColor) {
    chunk->emitcc([
      r, g, b, a,
      rt = current_rt_[0],
      rt_alloc = Rc<TextureAllocation>(current_rt_[0]->current()),
      rt_view = current_rt_view_[0]
    ](ArgumentEncodingContext &ctx) mutable {
      ctx.clearColor(std::move(rt), rt_alloc.ptr(), rt_view, 1, {r, g, b, a});
    });
  }

  if ((clearDepth || clearStencil) && depth_stencil_) {
    unsigned dsFlags = (clearDepth ? 1 : 0) | (clearStencil ? 2 : 0);
    chunk->emitcc([
      Z, Stencil, dsFlags,
      depth = depth_stencil_,
      ds_alloc = Rc<TextureAllocation>(depth_stencil_->current()),
      depth_view = depth_stencil_view_
    ](ArgumentEncodingContext &ctx) mutable {
      ctx.clearDepthStencil(std::move(depth), ds_alloc.ptr(), depth_view, 1, dsFlags, Z, (uint8_t)Stencil);
    });
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  viewport_ = *pViewport;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyViewport;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  *pViewport = viewport_;
  return S_OK;
}

// Render state
HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (State >= 256) return D3DERR_INVALIDCALL;
  if (render_states_[State] == Value) return S_OK; // no-op: skip dirty tracking
  render_states_[State] = Value;

  // Bump constant versions for render states that get packed into shader constants
  switch (State) {
  case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC: case D3DRS_FOGENABLE:
    pso_dirty_ = true; // these affect PSO variant key
    [[fallthrough]];
  case D3DRS_ALPHAREF:
  case D3DRS_FOGCOLOR: case D3DRS_FOGSTART: case D3DRS_FOGEND: case D3DRS_FOGDENSITY:
    ps_const_version_++;
    ff_const_version_++;
    ff_dirty_ |= kFFDirtyFog | kFFDirtyPS;
    break;
  case D3DRS_AMBIENT:
    ff_const_version_++;
    ff_dirty_ |= kFFDirtyAmbient;
    break;
  default:
    break;
  }

  // Mark PSO dirty for blend/depth/write/FF state changes
  switch (State) {
  case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
  case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
  case D3DRS_SRGBWRITEENABLE: case D3DRS_COLORWRITEENABLE:
  case D3DRS_FOGTABLEMODE: case D3DRS_FOGVERTEXMODE:
  case D3DRS_LIGHTING: case D3DRS_NORMALIZENORMALS: case D3DRS_SPECULARENABLE:
  case D3DRS_DIFFUSEMATERIALSOURCE: case D3DRS_AMBIENTMATERIALSOURCE:
  case D3DRS_SPECULARMATERIALSOURCE: case D3DRS_EMISSIVEMATERIALSOURCE:
  case D3DRS_COLORVERTEX:
    pso_dirty_ = true;
    break;
  default:
    break;
  }

  // Mark DSSO dirty for depth/stencil state changes
  switch (State) {
  case D3DRS_ZENABLE:
    dsso_dirty_ = true;
    pso_dirty_ = true;
    InvalidateCurrentPass(); // depth attachment presence depends on ZENABLE
    break;
  case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC:
  case D3DRS_STENCILENABLE: case D3DRS_STENCILFUNC:
  case D3DRS_STENCILPASS: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL:
  case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_STENCILREF:
  case D3DRS_TWOSIDEDSTENCILMODE:
  case D3DRS_CCW_STENCILFUNC: case D3DRS_CCW_STENCILPASS:
  case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL:
    dsso_dirty_ = true;
    break;
  case D3DRS_CULLMODE:
    break;
  default:
    break;
  }

  // Warn once for render states we store but don't act on
  static bool warned[256] = {};
  if (!warned[State] && Value != 0) {
    switch (State) {
    case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_WRAP0: case D3DRS_WRAP1: case D3DRS_WRAP2: case D3DRS_WRAP3:
    case D3DRS_WRAP4: case D3DRS_WRAP5: case D3DRS_WRAP6: case D3DRS_WRAP7:
    case D3DRS_CLIPPING:
    case D3DRS_POINTSIZE:
      Logger::warn(str::format("D3D9: unhandled render state ", State, " = ", Value));
      warned[State] = true;
      break;
    default:
      break;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  if (!pValue || State >= 256) return D3DERR_INVALIDCALL;
  *pValue = render_states_[State];
  return S_OK;
}

// Transform state
HRESULT STDMETHODCALLTYPE D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  transforms_[idx] = *pMatrix;
  ff_const_version_++;
  if (State == D3DTS_WORLD)
    ff_dirty_ |= kFFDirtyWorld;
  else if (State == D3DTS_VIEW)
    ff_dirty_ |= kFFDirtyView | kFFDirtyLights;
  else if (State == D3DTS_PROJECTION)
    ff_dirty_ |= kFFDirtyProj;
  else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    ff_dirty_ |= kFFDirtyTexMat;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  *pMatrix = transforms_[idx];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= 512) return D3DERR_INVALIDCALL;
  transforms_[idx] = MultiplyMatrices(transforms_[idx], *pMatrix);
  ff_const_version_++;
  if (State == D3DTS_WORLD)
    ff_dirty_ |= kFFDirtyWorld;
  else if (State == D3DTS_VIEW)
    ff_dirty_ |= kFFDirtyView | kFFDirtyLights;
  else if (State == D3DTS_PROJECTION)
    ff_dirty_ |= kFFDirtyProj;
  else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    ff_dirty_ |= kFFDirtyTexMat;
  return S_OK;
}

// Material
HRESULT STDMETHODCALLTYPE D3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  material_ = *pMaterial;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyMaterial;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetMaterial(D3DMATERIAL9 *pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  *pMaterial = material_;
  return S_OK;
}

// Lights
HRESULT STDMETHODCALLTYPE D3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  if (!pLight || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  if (pLight->Type != lights_[Index].Type)
    pso_dirty_ = true; // only light Type affects FF VS key
  lights_[Index] = *pLight;
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyLights;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  if (!pLight || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  *pLight = lights_[Index];
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  if (Index >= kMaxLights) return D3DERR_INVALIDCALL;
  light_enabled_[Index] = Enable;
  pso_dirty_ = true; // light enable affects FF VS key
  ff_const_version_++;
  ff_dirty_ |= kFFDirtyLights;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  if (!pEnable || Index >= kMaxLights) return D3DERR_INVALIDCALL;
  *pEnable = light_enabled_[Index];
  return S_OK;
}

// Texture stage states
HRESULT STDMETHODCALLTYPE D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  if (Stage >= 8 || Type >= 33) return D3DERR_INVALIDCALL;
  if (texture_stage_states_[Stage][Type] == Value) return S_OK;
  texture_stage_states_[Stage][Type] = Value;
  // Only states that affect FF shader keys need to dirty PSO
  switch (Type) {
  case D3DTSS_COLOROP: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2:
  case D3DTSS_ALPHAOP: case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2:
  case D3DTSS_TEXCOORDINDEX: case D3DTSS_TEXTURETRANSFORMFLAGS:
    pso_dirty_ = true;
    break;
  default:
    break;
  }
  // Only COLOROP affects which stages get white texture fallback
  if (Type == D3DTSS_COLOROP)
    tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  if (Stage >= 8 || Type >= 33 || !pValue) return D3DERR_INVALIDCALL;
  *pValue = texture_stage_states_[Stage][Type];
  return S_OK;
}

// Texture creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) {
  Logger::err(str::format("TRACE: CreateTexture ", Width, "x", Height, " lvl=", Levels, " usage=", (int)Usage, " fmt=", (int)Format, " pool=", (int)Pool));
  Logger::info(str::format("D3D9: CreateTexture ", Width, "x", Height,
      " lvl=", Levels, " usage=", Usage, " fmt=", (int)Format, " (", D3D9FormatName(Format), ")"));
  if (!ppTexture) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9Format(Format);
  if (mtlFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("D3D9: CreateTexture unsupported format ", (int)Format));
    return D3DERR_INVALIDCALL;
  }

  // Compute mip levels
  UINT mipLevels = Levels;
  if (mipLevels == 0)
    mipLevels = (UINT)std::floor(std::log2((double)std::max(Width, Height))) + 1;

  bool isRenderTarget = (Usage & D3DUSAGE_RENDERTARGET) != 0;

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = mipLevels;
  info.sample_count = 1;
  if (isRenderTarget) {
    info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
    info.options = WMTResourceStorageModePrivate;
  } else {
    info.usage = WMTTextureUsageShaderRead;
    info.options = WMTResourceStorageModeShared;
  }

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  TextureViewDescriptor viewDesc = {
      .format = mtlFormat,
      .type = WMTTextureType2D,
      .firstMiplevel = 0,
      .miplevelCount = mipLevels,
      .firstArraySlice = 0,
      .arraySize = 1,
  };

  TextureViewKey viewKey;
  if (Format == D3DFMT_A8L8) {
    // A8L8 maps to RG8Unorm. D3D9 expects RGB=luminance, A=alpha.
    // Metal sees R=luminance, G=alpha. Apply .rrrg swizzle.
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleGreen
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else if (Format == D3DFMT_L8 || Format == D3DFMT_L16) {
    // L8/L16 maps to R8/R16Unorm. D3D9 expects luminance replicated to RGB, alpha=1.
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else {
    viewKey = texture->createView(viewDesc);
  }

  auto *tex2d = new D3D9Texture2D(this, Width, Height, Levels, Format, std::move(texture), viewKey);
  if (isRenderTarget) {
    auto rtKey = tex2d->texture()->createView({
        .format = mtlFormat,
        .type = WMTTextureType2DArray,
        .firstMiplevel = 0,
        .miplevelCount = mipLevels,
        .firstArraySlice = 0,
        .arraySize = 1,
    });
    tex2d->setRT(rtKey);
  }

  // Create sRGB view for formats that support it (for D3DSAMP_SRGBTEXTURE)
  WMTPixelFormat srgbFormat = WMTPixelFormatInvalid;
  if (mtlFormat == WMTPixelFormatBGRA8Unorm) srgbFormat = WMTPixelFormatBGRA8Unorm_sRGB;
  else if (mtlFormat == WMTPixelFormatRGBA8Unorm) srgbFormat = WMTPixelFormatRGBA8Unorm_sRGB;
  else if (mtlFormat == WMTPixelFormatBC1_RGBA) srgbFormat = WMTPixelFormatBC1_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatBC2_RGBA) srgbFormat = WMTPixelFormatBC2_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatBC3_RGBA) srgbFormat = WMTPixelFormatBC3_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatR8Unorm) srgbFormat = WMTPixelFormatR8Unorm_sRGB;
  if (srgbFormat != WMTPixelFormatInvalid) {
    auto srgbKey = tex2d->texture()->createView({
        .format = srgbFormat,
        .type = WMTTextureType2DArray,
        .firstMiplevel = 0,
        .miplevelCount = mipLevels,
        .firstArraySlice = 0,
        .arraySize = 1,
    });
    tex2d->setSrgbView(srgbKey);
  }

  *ppTexture = ref(tex2d);
  trace_file("CreateTexture OK");
  return S_OK;
}

// Cube texture creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) {
  if (!ppCubeTexture) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9Format(Format);
  if (mtlFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("D3D9: CreateCubeTexture unsupported format ", (int)Format));
    return D3DERR_INVALIDCALL;
  }

  UINT mipLevels = Levels;
  if (mipLevels == 0)
    mipLevels = (UINT)std::floor(std::log2((double)EdgeLength)) + 1;

  bool isRenderTarget = (Usage & D3DUSAGE_RENDERTARGET) != 0;

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = EdgeLength;
  info.height = EdgeLength;
  info.depth = 1;
  info.array_length = 1; // Metal handles 6 faces internally for cube type
  info.type = WMTTextureTypeCube;
  info.mipmap_level_count = mipLevels;
  info.sample_count = 1;
  if (isRenderTarget) {
    info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView);
    info.options = WMTResourceStorageModePrivate;
  } else {
    info.usage = (WMTTextureUsage)(WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView);
    info.options = WMTResourceStorageModeShared;
  }

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  TextureViewDescriptor viewDesc = {
      .format = mtlFormat,
      .type = WMTTextureTypeCube,
      .firstMiplevel = 0,
      .miplevelCount = mipLevels,
      .firstArraySlice = 0,
      .arraySize = 6,
  };

  TextureViewKey viewKey;
  if (Format == D3DFMT_A8L8) {
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleGreen
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else if (Format == D3DFMT_L8 || Format == D3DFMT_L16) {
    WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne
    };
    viewKey = texture->createViewWithSwizzle(viewDesc, swizzle);
  } else {
    viewKey = texture->createView(viewDesc);
  }

  auto *cubeTex = new D3D9TextureCube(this, EdgeLength, Levels, Format, std::move(texture), viewKey);

  if (isRenderTarget) {
    cubeTex->setRT();
    // Create per-face RT views as 2DArray slices (Metal renders to individual slices)
    for (UINT face = 0; face < 6; face++) {
      auto faceKey = cubeTex->texture()->createView({
          .format = mtlFormat,
          .type = WMTTextureType2DArray,
          .firstMiplevel = 0,
          .miplevelCount = mipLevels,
          .firstArraySlice = (uint16_t)face,
          .arraySize = 1,
      });
      cubeTex->setFaceRTViewKey(face, faceKey);
    }
  }

  // Create sRGB view for formats that support it
  WMTPixelFormat srgbFormat = WMTPixelFormatInvalid;
  if (mtlFormat == WMTPixelFormatBGRA8Unorm) srgbFormat = WMTPixelFormatBGRA8Unorm_sRGB;
  else if (mtlFormat == WMTPixelFormatRGBA8Unorm) srgbFormat = WMTPixelFormatRGBA8Unorm_sRGB;
  else if (mtlFormat == WMTPixelFormatBC1_RGBA) srgbFormat = WMTPixelFormatBC1_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatBC2_RGBA) srgbFormat = WMTPixelFormatBC2_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatBC3_RGBA) srgbFormat = WMTPixelFormatBC3_RGBA_sRGB;
  else if (mtlFormat == WMTPixelFormatR8Unorm) srgbFormat = WMTPixelFormatR8Unorm_sRGB;
  if (srgbFormat != WMTPixelFormatInvalid) {
    auto srgbKey = cubeTex->texture()->createView({
        .format = srgbFormat,
        .type = WMTTextureTypeCube,
        .firstMiplevel = 0,
        .miplevelCount = mipLevels,
        .firstArraySlice = 0,
        .arraySize = 6,
    });
    cubeTex->setSrgbView(srgbKey);
  }

  *ppCubeTexture = ref(cubeTex);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (Stage >= 16) return D3DERR_INVALIDCALL;
  if (bound_textures_[Stage].ptr() == pTexture) return S_OK; // no-op
  bound_textures_[Stage] = pTexture;
  if (pTexture)
    tex_bound_mask_ |= (1u << Stage);
  else
    tex_bound_mask_ &= ~(1u << Stage);
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  if (Stage >= 16 || !ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = bound_textures_[Stage].ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_state_change_count++;
#endif
  if (Sampler >= 16 || Type >= 14) return D3DERR_INVALIDCALL;
  if (sampler_states_[Sampler][Type] == Value) return S_OK;
  sampler_states_[Sampler][Type] = Value;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  if (Sampler >= 16 || Type >= 14 || !pValue) return D3DERR_INVALIDCALL;
  *pValue = sampler_states_[Sampler][Type];
  return S_OK;
}

// Depth/stencil surface
HRESULT STDMETHODCALLTYPE D3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  InvalidateCurrentPass();
  if (pNewZStencil) {
    auto *surf = static_cast<D3D9Surface *>(pNewZStencil);
    depth_stencil_ = surf->texture();
    depth_stencil_view_ = surf->viewKey();
    depth_stencil_surface_ = surf;
    depth_stencil_format_ = surf->mtlFormat();
  } else {
    depth_stencil_ = nullptr;
    depth_stencil_view_ = 0;
    depth_stencil_surface_ = nullptr;
    depth_stencil_format_ = WMTPixelFormatInvalid;
  }
  pso_dirty_ = true;
  dsso_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  Logger::info(str::format("D3D9: GetDepthStencilSurface hasDS=", (bool)depth_stencil_surface_));
  if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
  if (!depth_stencil_surface_) {
    // GTA IV workaround: game ignores D3DERR_NOTFOUND and writes to the null pointer.
    // Create a dummy 1x1 depth surface to absorb the write.
    if (!dummy_depth_surface_) {
      Logger::info("D3D9: Creating dummy 1x1 depth surface for null-deref protection");
      IDirect3DSurface9 *dummy = nullptr;
      CreateDepthStencilSurface(1, 1, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &dummy, nullptr);
      dummy_depth_surface_ = Com<D3D9Surface>(static_cast<D3D9Surface *>(dummy));
    }
    *ppZStencilSurface = ref(dummy_depth_surface_.ptr());
    return D3DERR_NOTFOUND;
  }
  *ppZStencilSurface = ref(depth_stencil_surface_.ptr());
  return S_OK;
}

static WMTPixelFormat ConvertD3D9DepthFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return WMTPixelFormatDepth16Unorm;
  case D3DFMT_D24S8:
  case D3DFMT_D24X4S4:
    return WMTPixelFormatDepth32Float_Stencil8; // Use D32F+S8 for stencil support
  case D3DFMT_D24X8:
    return WMTPixelFormatDepth32Float; // No stencil needed
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
    return WMTPixelFormatDepth32Float;
  case D3DFMT_D24FS8:
    return WMTPixelFormatDepth32Float_Stencil8;
  default:
    return WMTPixelFormatDepth32Float;
  }
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  Logger::info(str::format("D3D9: CreateDepthStencilSurface ", Width, "x", Height,
      " fmt=", (int)Format, " (", D3D9FormatName(Format), ")"));
  if (!ppSurface) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9DepthFormat(Format);

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageRenderTarget;
  info.options = WMTResourceStorageModePrivate;

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  auto viewKey = texture->createView({
      .format = mtlFormat,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  *ppSurface = ref(new D3D9Surface(this, texture, viewKey));
  return S_OK;
}

// Buffer creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) {
  Logger::err(str::format("TRACE: CreateVertexBuffer len=", Length, " usage=", (int)Usage, " fvf=", (int)FVF, " pool=", (int)Pool));
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;

  auto buffer = Rc(new Buffer(Length, dxmt_device_->device()));
  Flags<BufferAllocationFlag> bufFlags;
  bufFlags.set(BufferAllocationFlag::CpuPlaced);
  auto allocation = buffer->allocate(bufFlags);
  memset(allocation->mappedMemory(0), 0, Length);
  buffer->rename(Rc(allocation));

  *ppVertexBuffer = ref(new D3D9VertexBuffer(this, Length, Usage, FVF, Pool,
                                              std::move(buffer), std::move(allocation)));
  return S_OK;
}

// Index buffer creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) {
  Logger::err(str::format("TRACE: CreateIndexBuffer len=", Length, " fmt=", (int)Format, " pool=", (int)Pool));
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;

  auto buffer = Rc(new Buffer(Length, dxmt_device_->device()));
  Flags<BufferAllocationFlag> bufFlags;
  bufFlags.set(BufferAllocationFlag::CpuPlaced);
  auto allocation = buffer->allocate(bufFlags);
  memset(allocation->mappedMemory(0), 0, Length);
  buffer->rename(Rc(allocation));

  *ppIndexBuffer = ref(new D3D9IndexBuffer(this, Length, Usage, Format, Pool,
                                            std::move(buffer), std::move(allocation)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  auto *ib = static_cast<D3D9IndexBuffer *>(pIndexData);
  if (current_ib_.ptr() == ib) return S_OK;
  current_ib_ = ib;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  if (!ppIndexData) return D3DERR_INVALIDCALL;
  *ppIndexData = current_ib_.ref();
  return S_OK;
}

// FVF
HRESULT STDMETHODCALLTYPE D3D9Device::SetFVF(DWORD FVF) {
  if (FVF == 0) return S_OK;

  if (current_fvf_ == FVF) return S_OK;
  current_fvf_ = FVF;
  pso_dirty_ = true;

  auto it = fvf_cache_.find(FVF);
  if (it != fvf_cache_.end()) {
    current_vdecl_ = it->second;
    return S_OK;
  }

  auto decl = D3D9VertexDeclaration::CreateFromFVF(FVF);
  fvf_cache_.emplace(FVF, decl);
  current_vdecl_ = std::move(decl);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetFVF(DWORD *pFVF) {
  if (!pFVF) return D3DERR_INVALIDCALL;
  *pFVF = current_fvf_;
  return S_OK;
}

// Vertex declaration
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
  Logger::info("TRACE: CreateVertexDeclaration");
  if (!pVertexElements || !ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = ref(new D3D9VertexDeclaration(pVertexElements));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  auto *decl = static_cast<D3D9VertexDeclaration *>(pDecl);
  if (current_vdecl_.ptr() == decl) return S_OK;
  current_vdecl_ = decl;
  current_fvf_ = 0;
  pso_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = current_vdecl_.ref();
  return S_OK;
}

// Compile a SM30 pixel shader to Metal function
static WMT::Reference<WMT::Function> CompileSm30PixelShader(WMT::Device device, sm30_shader_t shader,
                                                              const Sha1Digest &shaderSha1,
                                                              const char *funcName, WMTMetalVersion metalVersion,
                                                              uint8_t alphaTestFunc = 0,
                                                              uint8_t fogMode = 0) {
  auto start_ns = ShaderCache::nowNs();
  // Cache key: shader SHA1 + variant params
  Sha1HashState keyHash;
  keyHash.update(shaderSha1);
  keyHash.update(alphaTestFunc);
  keyHash.update(fogMode);
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries (all disk cache entries are preloaded at startup)
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction(funcName);
      if (function) return function;
    }
  }

  sm50_error_t err = nullptr;

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  SM30_SHADER_FOG_DATA fogData;
  fogData.type = SM30_SHADER_FOG;
  fogData.fog_mode = fogMode;
  fogData.next = &common;

  SM30_SHADER_ALPHA_TEST_DATA alphaTest;
  alphaTest.type = SM30_SHADER_ALPHA_TEST;
  alphaTest.alpha_test_func = alphaTestFunc;
  alphaTest.next = fogMode ? (void *)&fogData : (void *)&common;

  SM50_SHADER_COMPILATION_ARGUMENT_DATA *firstArg;
  if (alphaTestFunc != 0) {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&alphaTest;
  } else if (fogMode) {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&fogData;
  } else {
    firstArg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common;
  }

  sm50_bitcode_t bitcode = nullptr;
  if (SM30Compile(shader, firstArg, funcName, &bitcode, &err)) {
    Logger::err("D3D9: SM30Compile failed for pixel shader");
    if (err) { SM50FreeError(err); }
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for pixel shader");
    if (mtl_err) Logger::err(mtl_err.description().getUTF8String());
    return {};
  }

  auto function = library.newFunction(funcName);
  if (!function) {
    Logger::err("D3D9: Failed to find MTLFunction for pixel shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

// Map D3D9 decl type to Metal attribute format
static uint32_t D3D9DeclTypeToAttributeFormat(D3DDECLTYPE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:    return WMTAttributeFormatFloat;
  case D3DDECLTYPE_FLOAT2:    return WMTAttributeFormatFloat2;
  case D3DDECLTYPE_FLOAT3:    return WMTAttributeFormatFloat3;
  case D3DDECLTYPE_FLOAT4:    return WMTAttributeFormatFloat4;
  case D3DDECLTYPE_D3DCOLOR:  return WMTAttributeFormatUChar4Normalized_BGRA;
  case D3DDECLTYPE_UBYTE4:    return WMTAttributeFormatUChar4;
  case D3DDECLTYPE_SHORT2:    return WMTAttributeFormatShort2;
  case D3DDECLTYPE_SHORT4:    return WMTAttributeFormatShort4;
  case D3DDECLTYPE_UBYTE4N:   return WMTAttributeFormatUChar4Normalized;
  case D3DDECLTYPE_SHORT2N:   return WMTAttributeFormatShort2Normalized;
  case D3DDECLTYPE_SHORT4N:   return WMTAttributeFormatShort4Normalized;
  case D3DDECLTYPE_USHORT2N:  return WMTAttributeFormatUShort2Normalized;
  case D3DDECLTYPE_USHORT4N:  return WMTAttributeFormatUShort4Normalized;
  case D3DDECLTYPE_FLOAT16_2: return WMTAttributeFormatHalf2;
  case D3DDECLTYPE_FLOAT16_4: return WMTAttributeFormatHalf4;
  default: return 0;
  }
}

// Compile a SM30 vertex shader with input layout from vertex declaration
static WMT::Reference<WMT::Function> CompileSm30VertexShaderWithLayout(
    WMT::Device device, D3D9VertexShader *vs, D3D9VertexDeclaration *vdecl,
    const char *funcName, WMTMetalVersion metalVersion) {
  auto start_ns = ShaderCache::nowNs();

  // Query the shader's input declarations to map vertex elements by usage/usageIndex
  uint32_t numInputDecls = SM30GetInputDeclCount(vs->handle());
  std::vector<SM30_INPUT_DECL> inputDecls(numInputDecls);
  if (numInputDecls > 0)
    SM30GetInputDecls(vs->handle(), inputDecls.data());

  // Build SM50_IA_INPUT_ELEMENT array by matching D3D9 vertex elements to SM30 input DCLs
  auto &elements = vdecl->elements();
  std::vector<SM50_IA_INPUT_ELEMENT> ia_elements;
  uint32_t slot_mask = 0;

  for (auto &elem : elements) {
    if (elem.Stream == 0xFF) break;

    uint32_t format = D3D9DeclTypeToAttributeFormat((D3DDECLTYPE)elem.Type);
    if (!format) continue;

    // Find the shader input DCL that matches this vertex element's usage/usageIndex
    for (auto &dcl : inputDecls) {
      if (dcl.usage == elem.Usage && dcl.usageIndex == elem.UsageIndex) {
        SM50_IA_INPUT_ELEMENT ia_elem = {};
        ia_elem.reg = dcl.reg;
        ia_elem.slot = elem.Stream;
        ia_elem.aligned_byte_offset = elem.Offset;
        ia_elem.format = format;
        ia_elem.step_function = 0;
        ia_elem.step_rate = 1;
        ia_elements.push_back(ia_elem);
        slot_mask |= (1 << elem.Stream);
        break;
      }
    }
  }

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  // Cache key: shader SHA1 + layout elements
  Sha1HashState keyHash;
  keyHash.update(vs->sha1());
  keyHash.update(ia_elements.data(), ia_elements.size() * sizeof(SM50_IA_INPUT_ELEMENT));
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction(funcName);
      if (function) return function;
    }
  }

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout;
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = &common;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = slot_mask;
  ia_layout.num_elements = (uint32_t)ia_elements.size();
  ia_layout.elements = ia_elements.data();

  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t err = nullptr;
  if (SM30Compile(vs->handle(), (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia_layout, funcName, &bitcode, &err)) {
    Logger::err("D3D9: SM30Compile failed for vertex shader with input layout");
    if (err) { SM50FreeError(err); }
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for vertex shader");
    return {};
  }

  auto function = library.newFunction(funcName);
  if (!function) {
    Logger::err("D3D9: Failed to create MTLFunction for vertex shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  Logger::info("TRACE: CreateVertexShader");
  trace_file("CreateVertexShader");
  if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;

  // Scan for END token to determine SM30 bytecode size
  const uint32_t *p = (const uint32_t *)pFunction;
  while ((*p & 0xffff) != 0xffff) p++;
  p++; // include END token
  size_t bytecodeSize = (size_t)((const uint8_t *)p - (const uint8_t *)pFunction);

  sm30_shader_t shader = nullptr;
  sm50_error_t err = nullptr;

  if (SM30Initialize(pFunction, bytecodeSize, &shader, &err)) {
    Logger::err("D3D9: SM30Initialize failed for vertex shader");
    if (err) { SM50FreeError(err); }
    return D3DERR_INVALIDCALL;
  }

  std::vector<uint8_t> bytecodeVec((const uint8_t *)pFunction, (const uint8_t *)pFunction + bytecodeSize);
  *ppShader = ref(new D3D9VertexShader(shader, std::move(bytecodeVec)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  auto *vs = static_cast<D3D9VertexShader *>(pShader);
  if (current_vs_.ptr() == vs) return S_OK;
  current_vs_ = vs;
  pso_dirty_ = true;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = current_vs_.ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  Logger::info("TRACE: CreatePixelShader");
  trace_file("CreatePixelShader");
  if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;

  // Scan for END token
  const uint32_t *p = (const uint32_t *)pFunction;
  while ((*p & 0xffff) != 0xffff) p++;
  p++;
  size_t bytecodeSize = (size_t)((const uint8_t *)p - (const uint8_t *)pFunction);

  sm30_shader_t shader = nullptr;
  sm50_error_t err = nullptr;

  if (SM30Initialize(pFunction, bytecodeSize, &shader, &err)) {
    Logger::err("D3D9: SM30Initialize failed for pixel shader");
    if (err) { SM50FreeError(err); }
    return D3DERR_INVALIDCALL;
  }

  uint8_t maxTexcoordCount = (uint8_t)SM30GetPSMaxTexcoordCount(shader);
  auto sha1 = Sha1HashState::compute(pFunction, bytecodeSize);

  auto function = CompileSm30PixelShader(dxmt_device_->device(), shader, sha1,
                                          "shader_main", dxmt_device_->metalVersion());
  if (!function) {
    SM30Destroy(shader);
    return D3DERR_INVALIDCALL;
  }

  *ppShader = ref(new D3D9PixelShader(shader, std::move(function), maxTexcoordCount, sha1));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  auto *ps = static_cast<D3D9PixelShader *>(pShader);
  if (current_ps_.ptr() == ps) return S_OK;
  current_ps_ = ps;
  pso_dirty_ = true;
  tex_dirty_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = current_ps_.ref();
  return S_OK;
}

// Shader constants
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(&vsConstants_[StartRegister], pConstantData, Vector4fCount * 4 * sizeof(float));
  vs_const_version_++;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantF(
    UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(&psConstants_[StartRegister], pConstantData, Vector4fCount * 4 * sizeof(float));
  ps_const_version_++;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantI(
    UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  if (!pConstantData || StartRegister + Vector4iCount > 16) return D3DERR_INVALIDCALL;
  memcpy(&vsConstantsI_[StartRegister], pConstantData, Vector4iCount * 4 * sizeof(int));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantI(
    UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  if (!pConstantData || StartRegister + Vector4iCount > 16) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vsConstantsI_[StartRegister], Vector4iCount * 4 * sizeof(int));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantB(
    UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  if (!pConstantData || StartRegister + BoolCount > 16) return D3DERR_INVALIDCALL;
  memcpy(&vsConstantsB_[StartRegister], pConstantData, BoolCount * sizeof(BOOL));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantB(
    UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  if (!pConstantData || StartRegister + BoolCount > 16) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vsConstantsB_[StartRegister], BoolCount * sizeof(BOOL));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantI(
    UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  if (!pConstantData || StartRegister + Vector4iCount > 16) return D3DERR_INVALIDCALL;
  memcpy(&psConstantsI_[StartRegister], pConstantData, Vector4iCount * 4 * sizeof(int));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantI(
    UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  if (!pConstantData || StartRegister + Vector4iCount > 16) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &psConstantsI_[StartRegister], Vector4iCount * 4 * sizeof(int));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantB(
    UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  if (!pConstantData || StartRegister + BoolCount > 16) return D3DERR_INVALIDCALL;
  memcpy(&psConstantsB_[StartRegister], pConstantData, BoolCount * sizeof(BOOL));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantB(
    UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  if (!pConstantData || StartRegister + BoolCount > 16) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &psConstantsB_[StartRegister], BoolCount * sizeof(BOOL));
  return S_OK;
}

// Clip planes
HRESULT STDMETHODCALLTYPE D3D9Device::SetClipPlane(DWORD Index, const float *pPlane) {
  if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
  memcpy(clip_planes_[Index], pPlane, 4 * sizeof(float));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetClipPlane(DWORD Index, float *pPlane) {
  if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
  memcpy(pPlane, clip_planes_[Index], 4 * sizeof(float));
  return S_OK;
}

// Stream source
HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
  if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
  auto *vb = static_cast<D3D9VertexBuffer *>(pStreamData);
  if (stream_sources_[StreamNumber].ptr() == vb &&
      stream_offsets_[StreamNumber] == OffsetInBytes &&
      stream_strides_[StreamNumber] == Stride)
    return S_OK;
  stream_sources_[StreamNumber] = vb;
  stream_offsets_[StreamNumber] = OffsetInBytes;
  stream_strides_[StreamNumber] = Stride;
  vb_dirty_ = true;
  return S_OK;
}

// Queries
HRESULT STDMETHODCALLTYPE D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
  Logger::err(str::format("TRACE: CreateQuery type=", (int)Type, " ppQuery=", (void*)ppQuery));
  if (Type != D3DQUERYTYPE_EVENT && Type != D3DQUERYTYPE_OCCLUSION)
    return D3DERR_NOTAVAILABLE;
  if (!ppQuery) return S_OK; // Just checking support
  *ppQuery = ref(new D3D9Query(Type, this));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Query::Issue(DWORD dwIssueFlags) {
  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    if (dwIssueFlags & D3DISSUE_BEGIN) {
      query_ = new VisibilityResultQuery();
      device_->BeginOcclusionQuery(query_);
      building_ = true;
      issued_ = false;
    }
    if (dwIssueFlags & D3DISSUE_END) {
      if (building_) {
        device_->EndOcclusionQuery(query_);
        building_ = false;
        issued_ = true;
      }
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  if (type_ == D3DQUERYTYPE_EVENT) {
    if (pData) {
      BOOL result = TRUE;
      memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(BOOL)));
    }
    return S_OK;
  }

  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    if (!issued_) {
      // Never issued — return immediately with dummy result (same as old stub)
      if (pData) {
        DWORD result = 0;
        memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(DWORD)));
      }
      return S_OK;
    }
    uint64_t value;
    if (query_->getValue(&value)) {
      if (pData) {
        DWORD result = (DWORD)std::min(value, (uint64_t)UINT32_MAX);
        memcpy(pData, &result, std::min(dwSize, (DWORD)sizeof(DWORD)));
      }
      return S_OK;
    }
    if (dwGetDataFlags & D3DGETDATA_FLUSH)
      device_->FlushForQuery();
    return S_FALSE;
  }

  return S_OK;
}

// D3D9 blend factor → Metal blend factor
static WMTBlendFactor ConvertBlendFactor(DWORD d3dBlend) {
  switch (d3dBlend) {
  case D3DBLEND_ZERO:          return WMTBlendFactorZero;
  case D3DBLEND_ONE:           return WMTBlendFactorOne;
  case D3DBLEND_SRCCOLOR:      return WMTBlendFactorSourceColor;
  case D3DBLEND_INVSRCCOLOR:   return WMTBlendFactorOneMinusSourceColor;
  case D3DBLEND_SRCALPHA:      return WMTBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:   return WMTBlendFactorOneMinusSourceAlpha;
  case D3DBLEND_DESTALPHA:     return WMTBlendFactorDestinationAlpha;
  case D3DBLEND_INVDESTALPHA:  return WMTBlendFactorOneMinusDestinationAlpha;
  case D3DBLEND_DESTCOLOR:     return WMTBlendFactorDestinationColor;
  case D3DBLEND_INVDESTCOLOR:  return WMTBlendFactorOneMinusDestinationColor;
  case D3DBLEND_SRCALPHASAT:   return WMTBlendFactorSourceAlphaSaturated;
  case D3DBLEND_BLENDFACTOR:   return WMTBlendFactorBlendColor;
  case D3DBLEND_INVBLENDFACTOR:return WMTBlendFactorOneMinusBlendColor;
  default:                     return WMTBlendFactorOne;
  }
}

static WMTBlendOperation ConvertBlendOp(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DBLENDOP_ADD:         return WMTBlendOperationAdd;
  case D3DBLENDOP_SUBTRACT:    return WMTBlendOperationSubtract;
  case D3DBLENDOP_REVSUBTRACT: return WMTBlendOperationReverseSubtract;
  case D3DBLENDOP_MIN:         return WMTBlendOperationMin;
  case D3DBLENDOP_MAX:         return WMTBlendOperationMax;
  default:                     return WMTBlendOperationAdd;
  }
}

// PSO creation — compiles vertex shader with current vertex declaration's input layout
D3D9CompiledPipeline *D3D9Device::CreatePSO() {
  if (!current_vdecl_) return nullptr;

  // Fast path: reuse cached PSO if no relevant state changed
  if (!pso_dirty_ && cached_pso_)
    return cached_pso_;


  // Lazy-evaluated FF key builders (avoid redundant BuildFFVSKey/BuildFFPSKey calls)
  FFVSKey ffvskey_val;
  FFPSKey ffpskey_val;
  bool have_ffvs = false, have_ffps = false;
  auto getFFVSKey = [&]() -> FFVSKey& {
    if (!have_ffvs) { ffvskey_val = BuildFFVSKey(); have_ffvs = true; }
    return ffvskey_val;
  };
  auto getFFPSKey = [&]() -> FFPSKey& {
    if (!have_ffps) { ffpskey_val = BuildFFPSKey(); have_ffps = true; }
    return ffpskey_val;
  };

  // Resolve VS function handle (avoid WMT::Reference retain/release on hot path)
  obj_handle_t vs_handle = 0;
  if (current_vs_) {
    auto existing = current_vs_->getLayoutVariant(current_vdecl_.ptr());
    if (existing) {
      vs_handle = existing.handle;
    } else {
      auto vs_func = CompileSm30VertexShaderWithLayout(
          dxmt_device_->device(), current_vs_.ptr(), current_vdecl_.ptr(),
          "shader_main", dxmt_device_->metalVersion());
      if (vs_func)
        current_vs_->addLayoutVariant(current_vdecl_.ptr(), vs_func);
      vs_handle = vs_func.handle;
    }
  } else {
    auto &ffkey = getFFVSKey();
    if (current_ps_) {
      // Custom PS: bump VS texcoord count to satisfy PS inputs
      if (current_ps_->maxTexcoordCount() > ffkey.tex_coord_count)
        ffkey.tex_coord_count = current_ps_->maxTexcoordCount();
      // Re-populate TCI/TTF modes now that tex_coord_count is bumped
      for (uint8_t i = 0; i < ffkey.tex_coord_count && i < 8; i++) {
        DWORD tci = texture_stage_states_[i][D3DTSS_TEXCOORDINDEX];
        ffkey.tci_modes[i] = (uint8_t)((tci >> 16) & 0x3);
        ffkey.tci_coord_indices[i] = (uint8_t)(tci & 0x7);
        ffkey.ttf_modes[i] = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 0x7);
      }
    } else {
      // FF PS: bump VS texcoord count if any PS stage uses a higher texcoord_index
      auto &ffpskey = getFFPSKey();
      for (int i = 0; i < 8; i++) {
        if (ffpskey.stages[i].color_op == D3DTOP_DISABLE)
          break;
        if (ffpskey.stages[i].has_texture) {
          uint8_t need = ffpskey.stages[i].texcoord_index + 1;
          if (need > ffkey.tex_coord_count)
            ffkey.tex_coord_count = need;
        }
      }
      // Re-populate TCI/TTF modes now that tex_coord_count is bumped
      for (uint8_t i = 0; i < ffkey.tex_coord_count && i < 8; i++) {
        DWORD tci = texture_stage_states_[i][D3DTSS_TEXCOORDINDEX];
        ffkey.tci_modes[i] = (uint8_t)((tci >> 16) & 0x3);
        ffkey.tci_coord_indices[i] = (uint8_t)(tci & 0x7);
        ffkey.ttf_modes[i] = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 0x7);
      }
    }
    auto ff_vs = GetOrCreateFFVS(ffkey, current_vdecl_.ptr());
    vs_handle = ff_vs.handle;
    if (!vs_handle) {
      Logger::err("D3D9: FF VS compilation failed");
    }
  }
  if (!vs_handle) return nullptr;

  // Determine alpha test and fog state for custom PS
  uint8_t alphaFunc = 0;
  if (current_ps_ && render_states_[D3DRS_ALPHATESTENABLE] &&
      render_states_[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS) {
    alphaFunc = (uint8_t)render_states_[D3DRS_ALPHAFUNC];
  }
  // Determine fog mode for custom PS:
  // 0=none, 1=vertex (FOG0 from VS), 2=table EXP, 3=table EXP2, 4=table LINEAR
  uint8_t psFogMode = 0;
  if (current_ps_ && render_states_[D3DRS_FOGENABLE]) {
    if (current_vs_) {
      // Custom VS: use vertex fog if VS outputs FOG0, else table fog from depth
      if (SM30GetVSHasFogOutput(current_vs_->handle())) {
        psFogMode = 1; // vertex fog
      } else {
        // Fall back to table fog computed in PS from fragment depth
        DWORD tableFog = render_states_[D3DRS_FOGTABLEMODE];
        switch (tableFog) {
        case D3DFOG_EXP:    psFogMode = 2; break;
        case D3DFOG_EXP2:   psFogMode = 3; break;
        case D3DFOG_LINEAR: psFogMode = 4; break;
        }
      }
    } else {
      // FF VS: outputs FOG0 when effective fog mode != NONE
      DWORD effFog = render_states_[D3DRS_FOGVERTEXMODE];
      if (effFog == D3DFOG_NONE)
        effFog = render_states_[D3DRS_FOGTABLEMODE];
      if (effFog != D3DFOG_NONE)
        psFogMode = 1; // vertex fog (FF VS computes it)
    }
  }

  // Resolve PS function handle (avoid WMT::Reference retain/release on hot path)
  obj_handle_t ps_handle = 0;
  if (current_ps_) {
    uint8_t variantKey = D3D9PixelShader::makeVariantKey(alphaFunc, psFogMode);
    auto existing = current_ps_->getVariant(variantKey);
    if (existing) {
      ps_handle = existing.handle;
    } else {
      // Compile new variant with alpha test and fog baked in
      char funcName[64];
      snprintf(funcName, sizeof(funcName), "shader_main_v%u", variantKey);
      auto ps_func = CompileSm30PixelShader(
          dxmt_device_->device(), current_ps_->handle(), current_ps_->sha1(),
          funcName, dxmt_device_->metalVersion(), alphaFunc, psFogMode);
      if (ps_func)
        current_ps_->addVariant(variantKey, ps_func);
      ps_handle = ps_func.handle;
    }
    if (!ps_handle) Logger::err("D3D9: PS function is null");
  } else {
    auto &ffkey = getFFPSKey();
    // Sync PS fog_enable with VS fog output to prevent stage-in mismatch
    if (current_vs_ && ffkey.fog_enable) {
      if (!SM30GetVSHasFogOutput(current_vs_->handle()))
        ffkey.fog_enable = 0;
    }
    // Compute effective texcoord count: max of vdecl count and PS stage needs.
    // This ensures the PS declares the same TEXCOORD inputs the VS outputs.
    {
      auto &vsKey = getFFVSKey();
      // FF VS only outputs FOG0 when fog_mode != 0; sync PS to match
      if (!current_vs_ && vsKey.fog_mode == 0)
        ffkey.fog_enable = 0;
      uint8_t effective = vsKey.tex_coord_count;
      for (int i = 0; i < 8; i++) {
        if (ffkey.stages[i].color_op == D3DTOP_DISABLE)
          break;
        if (ffkey.stages[i].has_texture) {
          uint8_t need = ffkey.stages[i].texcoord_index + 1;
          if (need > effective) effective = need;
        }
      }
      ffkey.tex_coord_count = effective;
    }
    auto ff_ps = GetOrCreateFFPS(ffkey);
    ps_handle = ff_ps.handle;
    if (!ps_handle) {
      Logger::err("D3D9: FF PS compilation failed");
    }
  }
  if (!ps_handle) return nullptr;

  DWORD blendEnable = render_states_[D3DRS_ALPHABLENDENABLE];
  DWORD srcBlend = render_states_[D3DRS_SRCBLEND];
  DWORD destBlend = render_states_[D3DRS_DESTBLEND];
  DWORD blendOp = render_states_[D3DRS_BLENDOP];
  DWORD srcBlendAlpha = render_states_[D3DRS_SRCBLENDALPHA];
  DWORD destBlendAlpha = render_states_[D3DRS_DESTBLENDALPHA];
  DWORD blendOpAlpha = render_states_[D3DRS_BLENDOPALPHA];
  bool zEnable = render_states_[D3DRS_ZENABLE] != D3DZB_FALSE;
  DWORD depthFmt = (depth_stencil_ && zEnable) ? (DWORD)depth_stencil_format_ : 0;
  bool srgbWrite = render_states_[D3DRS_SRGBWRITEENABLE] != 0;
  uint8_t colorWriteMask = (uint8_t)(render_states_[D3DRS_COLORWRITEENABLE] & 0xF);

  PSOKey key = {vs_handle, ps_handle, current_vdecl_.ptr(),
                blendEnable, srcBlend, destBlend, blendOp, srcBlendAlpha,
                destBlendAlpha, blendOpAlpha, depthFmt, alphaFunc,
                srgbWrite, colorWriteMask};
  if (key == cached_pso_key_) {
    pso_dirty_ = false;
    return cached_pso_;
  }
  auto it = pso_cache_.find(key);
  if (it != pso_cache_.end()) {
    cached_pso_ = it->second.get();
    cached_pso_key_ = key;
    pso_dirty_ = false;
    return cached_pso_;
  }
  WMTRenderPipelineInfo pipeline_info;
  WMT::InitializeRenderPipelineInfo(pipeline_info);

  pipeline_info.vertex_function = vs_handle;
  pipeline_info.fragment_function = ps_handle;

  // Color write mask: D3D9 (R=1,G=2,B=4,A=8) → Metal (R=8,G=4,B=2,A=1)
  uint8_t mtlMask = 0;
  if (colorWriteMask & 0x1) mtlMask |= WMTColorWriteMaskRed;
  if (colorWriteMask & 0x2) mtlMask |= WMTColorWriteMaskGreen;
  if (colorWriteMask & 0x4) mtlMask |= WMTColorWriteMaskBlue;
  if (colorWriteMask & 0x8) mtlMask |= WMTColorWriteMaskAlpha;

  // Set up color attachments for all active render targets
  for (uint32_t i = 0; i < current_rt_count_; i++) {
    if (!current_rt_[i]) continue;
    WMTPixelFormat fmt = current_rt_format_[i];
    if (i == 0 && srgbWrite && fmt == WMTPixelFormatBGRA8Unorm)
      fmt = WMTPixelFormatBGRA8Unorm_sRGB;
    pipeline_info.colors[i].pixel_format = fmt;
    pipeline_info.colors[i].write_mask = (WMTColorWriteMask)mtlMask;
  }

  pipeline_info.rasterization_enabled = true;
  pipeline_info.raster_sample_count = 1;
  pipeline_info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  pipeline_info.immutable_vertex_buffers = 0;
  pipeline_info.immutable_fragment_buffers = 0;

  // Blend state (applies to all active RTs)
  if (blendEnable) {
    for (uint32_t i = 0; i < current_rt_count_; i++) {
      if (!current_rt_[i]) continue;
      pipeline_info.colors[i].blending_enabled = true;
      pipeline_info.colors[i].src_rgb_blend_factor = ConvertBlendFactor(srcBlend);
      pipeline_info.colors[i].dst_rgb_blend_factor = ConvertBlendFactor(destBlend);
      pipeline_info.colors[i].rgb_blend_operation = ConvertBlendOp(render_states_[D3DRS_BLENDOP]);
      pipeline_info.colors[i].src_alpha_blend_factor = ConvertBlendFactor(render_states_[D3DRS_SRCBLENDALPHA]);
      pipeline_info.colors[i].dst_alpha_blend_factor = ConvertBlendFactor(render_states_[D3DRS_DESTBLENDALPHA]);
      pipeline_info.colors[i].alpha_blend_operation = ConvertBlendOp(render_states_[D3DRS_BLENDOPALPHA]);
    }
  }

  // Depth format
  if (depthFmt) {
    pipeline_info.depth_pixel_format = (WMTPixelFormat)depthFmt;
    // Set stencil format for combined depth+stencil formats
    if (depthFmt == (DWORD)WMTPixelFormatDepth32Float_Stencil8)
      pipeline_info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
  }

  // Async PSO creation: submit to background thread, resolve on encode thread
  auto pipeline = std::make_unique<D3D9CompiledPipeline>(dxmt_device_->device(), pipeline_info);
  auto *pipeline_ptr = pipeline.get();
  pso_scheduler_->submit(pipeline_ptr);

#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_pso_miss_count++;
#endif
  pso_cache_.emplace(key, std::move(pipeline));
  cached_pso_ = pipeline_ptr;
  cached_pso_key_ = key;
  pso_dirty_ = false;
  return pipeline_ptr;
}

// D3D9 sampler state → WMTSamplerInfo
static WMTSamplerInfo ConvertD3D9SamplerState(const DWORD state[14]) {
  WMTSamplerInfo info = {};

  // Filter
  DWORD minFilter = state[D3DSAMP_MINFILTER];
  DWORD magFilter = state[D3DSAMP_MAGFILTER];
  DWORD mipFilter = state[D3DSAMP_MIPFILTER];
  info.min_filter = (minFilter == D3DTEXF_POINT) ? WMTSamplerMinMagFilterNearest : WMTSamplerMinMagFilterLinear;
  info.mag_filter = (magFilter == D3DTEXF_POINT) ? WMTSamplerMinMagFilterNearest : WMTSamplerMinMagFilterLinear;

  // Mip filter
  switch (mipFilter) {
  case D3DTEXF_NONE:
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    break;
  case D3DTEXF_POINT:
    info.mip_filter = WMTSamplerMipFilterNearest;
    break;
  case D3DTEXF_LINEAR:
    info.mip_filter = WMTSamplerMipFilterLinear;
    break;
  default:
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    break;
  }

  // Address modes
  auto convertAddr = [](DWORD mode) -> WMTSamplerAddressMode {
    switch (mode) {
    case D3DTADDRESS_WRAP:   return WMTSamplerAddressModeRepeat;
    case D3DTADDRESS_MIRROR: return WMTSamplerAddressModeMirrorRepeat;
    case D3DTADDRESS_CLAMP:  return WMTSamplerAddressModeClampToEdge;
    case D3DTADDRESS_BORDER: return WMTSamplerAddressModeClampToZero;
    default:                 return WMTSamplerAddressModeRepeat;
    }
  };
  info.s_address_mode = convertAddr(state[D3DSAMP_ADDRESSU]);
  info.t_address_mode = convertAddr(state[D3DSAMP_ADDRESSV]);
  info.r_address_mode = convertAddr(state[D3DSAMP_ADDRESSU]); // W not commonly used for 2D

  info.normalized_coords = true;
  info.lod_min_clamp = 0.0f;
  info.lod_max_clamp = 1000.0f;
  DWORD maxAniso = state[D3DSAMP_MAXANISOTROPY];
  info.max_anisotroy = (maxAniso > 1) ? maxAniso : 1;
  info.support_argument_buffers = true;
  return info;
}

// D3D stencil op → Metal stencil op
static WMTStencilOperation ConvertStencilOp(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DSTENCILOP_KEEP:    return WMTStencilOperationKeep;
  case D3DSTENCILOP_ZERO:    return WMTStencilOperationZero;
  case D3DSTENCILOP_REPLACE: return WMTStencilOperationReplace;
  case D3DSTENCILOP_INCRSAT: return WMTStencilOperationIncrementClamp;
  case D3DSTENCILOP_DECRSAT: return WMTStencilOperationDecrementClamp;
  case D3DSTENCILOP_INVERT:  return WMTStencilOperationInvert;
  case D3DSTENCILOP_INCR:    return WMTStencilOperationIncrementWrap;
  case D3DSTENCILOP_DECR:    return WMTStencilOperationDecrementWrap;
  default:                   return WMTStencilOperationKeep;
  }
}

// D3D cull mode → Metal cull mode
static WMTCullMode ConvertCullMode(DWORD d3dCull) {
  switch (d3dCull) {
  case D3DCULL_NONE: return WMTCullModeNone;
  case D3DCULL_CW:   return WMTCullModeFront; // D3D CW cull = Metal front cull (D3D is left-handed)
  case D3DCULL_CCW:  return WMTCullModeBack;
  default:           return WMTCullModeBack;
  }
}

// === Emit-on-draw infrastructure ===
using TexCapture = D3D9Device::TexCapture;
// (EmitState removed — state tracked in device members)
// ============================================================
// Helper functions
// ============================================================

// Compute primitive count → vertex/index count and Metal primitive type
static bool GetPrimitiveInfo(D3DPRIMITIVETYPE type, UINT primCount,
                             UINT &outCount, WMTPrimitiveType &outMtl) {
  switch (type) {
  case D3DPT_TRIANGLELIST:  outCount = primCount * 3;  outMtl = WMTPrimitiveTypeTriangle;      return true;
  case D3DPT_TRIANGLESTRIP: outCount = primCount + 2;  outMtl = WMTPrimitiveTypeTriangleStrip;  return true;
  case D3DPT_LINELIST:      outCount = primCount * 2;  outMtl = WMTPrimitiveTypeLine;           return true;
  case D3DPT_LINESTRIP:     outCount = primCount + 1;  outMtl = WMTPrimitiveTypeLineStrip;      return true;
  case D3DPT_POINTLIST:     outCount = primCount;       outMtl = WMTPrimitiveTypePoint;          return true;
  case D3DPT_TRIANGLEFAN:   outCount = primCount * 3;  outMtl = WMTPrimitiveTypeTriangle;       return true;
  default: return false;
  }
}

// Generate triangle fan → triangle list index buffer (transient ring allocation)
static CommandQueue::TransientAllocation GenerateFanIndices(CommandQueue &queue, UINT primCount, UINT startVertex) {
  UINT indexCount = primCount * 3;
  UINT size = indexCount * sizeof(uint16_t);
  auto alloc = queue.AllocateTransientBuffer(size, 2);
  auto *indices = static_cast<uint16_t*>(alloc.cpu_ptr);
  for (UINT i = 0; i < primCount; i++) {
    indices[i * 3 + 0] = (uint16_t)(startVertex + 0);
    indices[i * 3 + 1] = (uint16_t)(startVertex + i + 1);
    indices[i * 3 + 2] = (uint16_t)(startVertex + i + 2);
  }
  return alloc;
}

// ============================================================
// Emit-on-draw infrastructure (replaces batching layer)
// ============================================================

bool D3D9Device::EnsureRenderEncoder() {
  Texture *rt = current_rt_[0].ptr();
  Texture *depth = depth_stencil_.ptr();

  // Close existing pass if RT/depth changed
  if (encoder_state_ == EncoderState::RenderActive &&
      (rt != encoder_rt_ || depth != encoder_depth_))
    InvalidateCurrentPass();

  if (encoder_state_ == EncoderState::Idle) {
    // Pre-reserve space for the fixed VS/PS struct region at offset 0
    constexpr uint64_t FIXED_REGION = 3 * 8 + 18 * 8; // VS_ARGBUF_STRUCT_SIZE + PS_ARGBUF_STRUCT_SIZE
    auto argbuf_size_owner = std::make_unique<uint64_t>(FIXED_REGION);
    argbuf_size_ptr_ = argbuf_size_owner.get();

    auto &queue = dxmt_device_->queue();
    auto chunk = queue.CurrentChunk();

    bool depth_enable = render_states_[D3DRS_ZENABLE] != D3DZB_FALSE && depth;
    uint8_t dsv_flags = 0;
    if (depth_enable)
      dsv_flags = DepthStencilPlanarFlags(depth_stencil_format_);

    struct RTCapture {
      Rc<Texture> tex;
      Rc<TextureAllocation> alloc;
      TextureViewKey view;
    };
    uint32_t rt_count = current_rt_count_;
    std::array<RTCapture, kMaxRenderTargets> rt_caps;
    for (uint32_t i = 0; i < rt_count; i++) {
      if (current_rt_[i]) {
        rt_caps[i] = {current_rt_[i], Rc<TextureAllocation>(current_rt_[i]->current()), current_rt_view_[i]};
      }
    }
    Rc<Texture> depth_rc = depth_stencil_;
    TextureViewKey depth_view = depth_stencil_view_;
    Rc<TextureAllocation> ds_alloc(depth_enable && depth_rc ? depth_rc->current() : nullptr);

    chunk->emitcc([argbuf_size = std::move(argbuf_size_owner),
                   rt_caps = std::move(rt_caps), rt_count,
                   depth_rc = std::move(depth_rc), ds_alloc = std::move(ds_alloc), depth_view,
                   depth_enable, dsv_flags](ArgumentEncodingContext &ctx) {
      auto &pass = *ctx.startRenderPass(dsv_flags, 0, rt_count, *argbuf_size);
      for (uint32_t i = 0; i < rt_count; i++) {
        if (!rt_caps[i].tex) continue;
        auto &color = pass.colors[i];
        color.attachment = ctx.access(rt_caps[i].tex, rt_caps[i].alloc.ptr(), rt_caps[i].view, DXMT_ENCODER_RESOURCE_ACESS_READWRITE);
        color.depth_plane = 0;
        color.load_action = WMTLoadActionLoad;
        color.store_action = WMTStoreActionStore;
      }
      if (depth_enable && depth_rc) {
        pass.depth.attachment = ctx.access(depth_rc, ds_alloc.ptr(), depth_view, DXMT_ENCODER_RESOURCE_ACESS_READWRITE);
        pass.depth.load_action = WMTLoadActionLoad;
        pass.depth.store_action = WMTStoreActionStore;
        if (dsv_flags & 2) {
          pass.stencil.attachment = pass.depth.attachment;
          pass.stencil.load_action = WMTLoadActionLoad;
          pass.stencil.store_action = WMTStoreActionStore;
        }
      }
      pass.render_target_width = rt_caps[0].tex->width();
      pass.render_target_height = rt_caps[0].tex->height();
      pass.render_target_array_length = 1;
      pass.default_raster_sample_count = 1;
      ctx.bumpVisibilityResultOffset();
    });

    encoder_rt_ = rt;
    encoder_depth_ = depth;
    encoder_state_ = EncoderState::RenderActive;

    // Reset per-pass tracking to force re-emit on first draw
    last_emitted_pso_ = nullptr;
    last_emitted_dsso_ = 0;
    last_emitted_stencil_ref_ = ~0u;
    last_emitted_cull_ = (WMTCullMode)~0u;
    last_emitted_vp_ = {};
    last_emitted_scissor_enable_ = false;
    last_emitted_scissor_ = {};
    last_tex_fingerprint_ = ~0ULL;
    last_ps_tex_argbuf_off_ = 0;
    last_vs_const_version_ = ~0ULL;
    last_vs_const_argbuf_off_ = 0;
    last_vs_const_count_ = 0;
    last_ps_const_version_ = ~0ULL;
    last_ps_const_argbuf_off_ = 0;
    last_ps_const_count_ = 0;
    last_vb_fingerprint_ = ~0ULL;
    last_vb_argbuf_off_ = 0;
    last_vb_num_slots_ = 0;
  }
  return true;
}

void D3D9Device::InvalidateCurrentPass() {
  if (encoder_state_ == EncoderState::Idle) return;

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();
  chunk->emitcc([](ArgumentEncodingContext &ctx) { ctx.endPass(); });

  encoder_state_ = EncoderState::Idle;
  encoder_rt_ = nullptr;
  encoder_depth_ = nullptr;
  argbuf_size_ptr_ = nullptr;
}

void D3D9Device::BeginOcclusionQuery(Rc<VisibilityResultQuery> query) {
  InvalidateCurrentPass();
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();
  chunk->emitcc([q = std::move(query)](ArgumentEncodingContext &ctx) mutable {
    ctx.beginVisibilityResultQuery(std::move(q));
  });
}

void D3D9Device::EndOcclusionQuery(Rc<VisibilityResultQuery> query) {
  InvalidateCurrentPass();
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();
  chunk->emitcc([q = std::move(query)](ArgumentEncodingContext &ctx) mutable {
    ctx.endVisibilityResultQuery(std::move(q));
  });
  needs_query_flush_ = true;
}

void D3D9Device::FlushForQuery() {
  if (needs_query_flush_) {
    needs_query_flush_ = false;
    InvalidateCurrentPass();
    auto &queue = dxmt_device_->queue();
    queue.CommitCurrentChunk();
  }
}

uint64_t D3D9Device::PreAllocateArgumentBuffer(size_t size, size_t alignment) {
  assert(argbuf_size_ptr_);
  size_t current = *argbuf_size_ptr_;
  size_t adj = (alignment - (current % alignment)) % alignment;
  uint64_t aligned = current + adj;
  *argbuf_size_ptr_ = aligned + size;
  return aligned;
}

bool D3D9Device::PreDraw(WMTPrimitiveType mtlPrimType) {
#ifdef DXMT_PERF
  bool perf_sample = (perf_draw_counter_++ & 31) == 0;
  uint64_t perf_t;
  if (perf_sample) perf_t = rdtsc();
#endif

  // 1. PSO
  if (pso_dirty_) {
    cached_pso_ = CreatePSO();
    if (!cached_pso_) {
      return false;
    }
    pso_dirty_ = false;
  }

  // 2. Upload dirty textures (must happen before opening render encoder — uses blit commands)
  for (uint32_t mask = tex_bound_mask_; mask; mask &= mask - 1) {
    uint32_t stage = __builtin_ctz(mask);
    auto *base = bound_textures_[stage].ptr();
    if (base && D3D9IsAnyDirty(base)) {
      InvalidateCurrentPass(); // close render pass if open before blit
      auto &queue = dxmt_device_->queue();
      D3D9UploadDirtyStaged(base, queue);
    }
  }

  // 3. Ensure render encoder is open
  if (!EnsureRenderEncoder()) return false;

  // 4. Rebuild texture/sampler bindings if dirty
  if (tex_dirty_) {
    tex_capture_count_ = 0;
    memset(sampler_gpu_ids_, 0, sizeof(sampler_gpu_ids_));

    for (uint32_t mask = tex_bound_mask_; mask; mask &= mask - 1) {
      uint32_t stage = __builtin_ctz(mask);
      auto *base = bound_textures_[stage].ptr();
      bool wantSrgb = sampler_states_[stage][D3DSAMP_SRGBTEXTURE] != 0;
      TextureViewKey texView = (wantSrgb && D3D9HasSrgbView(base)) ? D3D9GetSrgbViewKey(base) : D3D9GetViewKey(base);
      tex_captures_[tex_capture_count_++] = {D3D9GetTexture(base).ptr(), texView, stage};

      SamplerKey samplerKey;
      memcpy(samplerKey.state, sampler_states_[stage], sizeof(samplerKey.state));
      auto sampIt = sampler_cache_.find(samplerKey);
      if (sampIt != sampler_cache_.end()) {
        sampler_gpu_ids_[stage] = sampIt->second.gpu_resource_id;
      } else {
        auto samplerInfo = ConvertD3D9SamplerState(sampler_states_[stage]);
        auto samplerHandle = MTLDevice_newSamplerState(dxmt_device_->device().handle, &samplerInfo);
        sampler_cache_[samplerKey] = {samplerHandle, samplerInfo.gpu_resource_id};
        sampler_gpu_ids_[stage] = samplerInfo.gpu_resource_id;
      }
    }

    // White texture fallback for unbound sampler slots
    if (current_ps_ && default_white_tex_) {
      uint32_t samplerCount = SM30GetSamplerDeclCount(current_ps_->handle());
      if (samplerCount > 0) {
        SM30_SAMPLER_DECL samplerDecls[16];
        SM30GetSamplerDecls(current_ps_->handle(), samplerDecls);
        for (uint32_t i = 0; i < samplerCount; i++) {
          uint32_t reg = samplerDecls[i].reg;
          if (!(tex_bound_mask_ & (1 << reg))) {
            tex_captures_[tex_capture_count_++] = {default_white_tex_.ptr(), default_white_view_, reg};
            sampler_gpu_ids_[reg] = default_sampler_gpu_id_;
          }
        }
      }
    }
    // FF PS: fill unbound stages with white texture
    if (!current_ps_ && default_white_tex_) {
      for (uint32_t stage = 0; stage < 8; stage++) {
        if (texture_stage_states_[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        if (!(tex_bound_mask_ & (1 << stage))) {
          tex_captures_[tex_capture_count_++] = {default_white_tex_.ptr(), default_white_view_, stage};
          sampler_gpu_ids_[stage] = default_sampler_gpu_id_;
        }
      }
    }

    // Compute fingerprint
    uint64_t h = tex_capture_count_;
    for (uint8_t i = 0; i < tex_capture_count_; i++) {
      h ^= reinterpret_cast<uint64_t>(tex_captures_[i].texture) * 0x9E3779B97F4A7C15ULL;
      h ^= (uint64_t)tex_captures_[i].viewKey * 0x517CC1B727220A95ULL;
      h ^= (uint64_t)tex_captures_[i].stage << 48;
      h ^= sampler_gpu_ids_[tex_captures_[i].stage] * 0x6C62272E07BB0142ULL;
    }
    tex_sampler_fingerprint_ = h;
    tex_dirty_ = false;
  }

  // 5. Compute argument buffer size for this draw
  uint32_t slot_mask = current_vdecl_ ? current_vdecl_->slotMask() : 0;
  uint32_t num_vb_slots = __builtin_popcount(slot_mask);
  constexpr uint64_t VB_ENTRY_SIZE = 16;
  uint16_t vs_const_count = current_vs_ ? (uint16_t)std::min(current_vs_->maxConstantReg(), 256u) : 100;
  uint64_t vs_const_buf_size = vs_const_count * 4 * sizeof(float);
  // Right-size PS constants: shader's max register, but at least 256 if alpha test or fog
  // need special registers at 254-255. For FF PS, only 3 registers needed.
  uint16_t ps_const_count_base = current_ps_ ? (uint16_t)std::min(current_ps_->maxConstantReg(), 256u) : 3;
  bool ps_needs_special_regs = current_ps_ && (
    (render_states_[D3DRS_ALPHATESTENABLE] && render_states_[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS) ||
    render_states_[D3DRS_FOGENABLE]);
  uint16_t ps_const_count = ps_needs_special_regs ? 256 : ps_const_count_base;
  uint64_t ps_const_buf_size = ps_const_count * 4 * sizeof(float);
  uint64_t vb_region_size = num_vb_slots * VB_ENTRY_SIZE;
  // VS struct: {vs_constants_ptr, vs_const_buf_size, half_pixel_offset} = 3 fields
  constexpr uint64_t VS_ARGBUF_STRUCT_SIZE = 3 * 8;
  constexpr uint64_t PS_ARGBUF_STRUCT_SIZE = 18 * 8;

  // Update FF constants before version check
  if (ff_dirty_ && !current_vs_) UpdateFFConstants();

  // Check constant version for reuse
  uint64_t vs_ver = current_vs_ ? vs_const_version_ : ff_const_version_;
  bool vs_const_reuse = (vs_ver == last_vs_const_version_) && (vs_const_count == last_vs_const_count_);
  uint64_t ps_ver = current_ps_ ? ps_const_version_ : ff_const_version_;
  bool ps_const_reuse = (ps_ver == last_ps_const_version_) && (ps_const_count == last_ps_const_count_);

  // Compute VB fingerprint — allocation pointer changes on Lock rename
  uint64_t vb_fingerprint = slot_mask;
  for (uint32_t mask = slot_mask; mask; mask &= mask - 1) {
    uint32_t slot = __builtin_ctz(mask);
    if (slot == 0 && transient_vb_override_.active) {
      vb_fingerprint ^= transient_vb_override_.gpu_address * 0x9E3779B97F4A7C15ULL;
      vb_fingerprint ^= (uint64_t)transient_vb_override_.stride << 32;
    } else if (stream_sources_[slot]) {
      vb_fingerprint ^= stream_sources_[slot]->gpuAddress() * 0x9E3779B97F4A7C15ULL;
      vb_fingerprint ^= (uint64_t)stream_strides_[slot] << 32;
      vb_fingerprint ^= (uint64_t)stream_offsets_[slot];
    }
  }
  bool vb_reuse = (vb_fingerprint == last_vb_fingerprint_) && last_vb_argbuf_off_
                   && (num_vb_slots == last_vb_num_slots_);

  // Only allocate argbuf space for state that needs to be written
  uint64_t vs_alloc_size = vs_const_reuse ? 0 : vs_const_buf_size;
  uint64_t ps_alloc_size = ps_const_reuse ? 0 : ps_const_buf_size;
  uint64_t vb_alloc_size = vb_reuse ? 0 : vb_region_size;
  uint64_t draw_argbuf_size = vb_alloc_size + vs_alloc_size + ps_alloc_size
                             + VS_ARGBUF_STRUCT_SIZE + PS_ARGBUF_STRUCT_SIZE;

  // 6. Check argbuf overflow → close and reopen pass
  if (*argbuf_size_ptr_ + draw_argbuf_size > kCommandChunkGPUHeapSize) {
    InvalidateCurrentPass();
    EnsureRenderEncoder();
  }

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  // Resolve DSSO on API thread
  if (dsso_dirty_) {
    bool depth_enable = render_states_[D3DRS_ZENABLE] != D3DZB_FALSE && depth_stencil_;
    obj_handle_t dsso = 0;
    if (depth_enable) {
      bool stencilEnabled = render_states_[D3DRS_STENCILENABLE] != 0;
      bool twoSided = render_states_[D3DRS_TWOSIDEDSTENCILMODE] != 0;
      DSKey dsKey{
        .depth_func = (WMTCompareFunction)(render_states_[D3DRS_ZFUNC] - 1),
        .depth_write = render_states_[D3DRS_ZWRITEENABLE] != 0,
        .stencil_enabled = stencilEnabled,
        .stencil_func = (WMTCompareFunction)(render_states_[D3DRS_STENCILFUNC] - 1),
        .stencil_pass = ConvertStencilOp(render_states_[D3DRS_STENCILPASS]),
        .stencil_fail = ConvertStencilOp(render_states_[D3DRS_STENCILFAIL]),
        .depth_fail = ConvertStencilOp(render_states_[D3DRS_STENCILZFAIL]),
        .stencil_read_mask = (uint8_t)render_states_[D3DRS_STENCILMASK],
        .stencil_write_mask = (uint8_t)render_states_[D3DRS_STENCILWRITEMASK],
        .two_sided = twoSided && stencilEnabled,
        .back_stencil_func = (WMTCompareFunction)(render_states_[D3DRS_CCW_STENCILFUNC] - 1),
        .back_stencil_pass = ConvertStencilOp(render_states_[D3DRS_CCW_STENCILPASS]),
        .back_stencil_fail = ConvertStencilOp(render_states_[D3DRS_CCW_STENCILFAIL]),
        .back_depth_fail = ConvertStencilOp(render_states_[D3DRS_CCW_STENCILZFAIL]),
      };
      auto it = ds_cache_.find(dsKey);
      if (it != ds_cache_.end()) {
        dsso = it->second;
      } else {
        WMTDepthStencilInfo dsInfo = {};
        dsInfo.depth_compare_function = dsKey.depth_func;
        dsInfo.depth_write_enabled = dsKey.depth_write;
        if (stencilEnabled) {
          dsInfo.front_stencil.enabled = true;
          dsInfo.front_stencil.stencil_compare_function = dsKey.stencil_func;
          dsInfo.front_stencil.depth_stencil_pass_op = dsKey.stencil_pass;
          dsInfo.front_stencil.stencil_fail_op = dsKey.stencil_fail;
          dsInfo.front_stencil.depth_fail_op = dsKey.depth_fail;
          dsInfo.front_stencil.read_mask = dsKey.stencil_read_mask;
          dsInfo.front_stencil.write_mask = dsKey.stencil_write_mask;
          if (dsKey.two_sided) {
            dsInfo.back_stencil.enabled = true;
            dsInfo.back_stencil.stencil_compare_function = dsKey.back_stencil_func;
            dsInfo.back_stencil.depth_stencil_pass_op = dsKey.back_stencil_pass;
            dsInfo.back_stencil.stencil_fail_op = dsKey.back_stencil_fail;
            dsInfo.back_stencil.depth_fail_op = dsKey.back_depth_fail;
            dsInfo.back_stencil.read_mask = dsKey.stencil_read_mask;
            dsInfo.back_stencil.write_mask = dsKey.stencil_write_mask;
          } else {
            dsInfo.back_stencil = dsInfo.front_stencil;
          }
        }
        dsso = MTLDevice_newDepthStencilState(dxmt_device_->device().handle, &dsInfo);
        ds_cache_[dsKey] = dsso;
      }
    }
    cached_dsso_ = dsso;
    cached_stencil_ref_ = render_states_[D3DRS_STENCILREF];
    dsso_dirty_ = false;
  }

  // Resolve rasterizer state (always recompute — must re-emit after pass reopen)
  WMTCullMode cull_mode = ConvertCullMode(render_states_[D3DRS_CULLMODE]);

  // VS/PS structs are always at fixed offsets 0 and VS_ARGBUF_STRUCT_SIZE.
  // Only VB entries and constants get new offsets per draw.
  // This avoids changing buffer[29]/[30] offsets between draws, which doesn't
  // work correctly with Metal argument buffer indirection.
  constexpr uint64_t FIXED_REGION = VS_ARGBUF_STRUCT_SIZE + PS_ARGBUF_STRUCT_SIZE;
  uint64_t vs_struct_off = 0;
  uint64_t ps_struct_off = VS_ARGBUF_STRUCT_SIZE;

  // Allocate space for variable-size data (VB entries + constants) AFTER the fixed region
  uint64_t var_alloc_size = vb_alloc_size + vs_alloc_size + ps_alloc_size;
  uint64_t var_base;
  if (var_alloc_size > 0) {
    var_base = PreAllocateArgumentBuffer(var_alloc_size, 16);
    // Ensure variable data starts after the fixed region
    if (var_base < FIXED_REGION) {
      // First draw: skip past fixed region
      (void)PreAllocateArgumentBuffer(FIXED_REGION - var_base, 1);
      var_base = PreAllocateArgumentBuffer(var_alloc_size, 16);
    }
  } else {
    var_base = FIXED_REGION; // dummy
  }

  uint64_t vb_off;
  uint64_t next_off = var_base;
  if (vb_reuse) {
    vb_off = last_vb_argbuf_off_;
  } else {
    vb_off = next_off;
    next_off += vb_region_size;
  }
  uint64_t vs_const_off_local;
  if (vs_const_reuse) {
    vs_const_off_local = last_vs_const_argbuf_off_;
  } else {
    vs_const_off_local = next_off;
    next_off += vs_const_buf_size;
  }
  uint64_t ps_const_off_local;
  if (ps_const_reuse) {
    ps_const_off_local = last_ps_const_argbuf_off_;
  } else {
    ps_const_off_local = next_off;
    next_off += ps_const_buf_size;
  }

  // Copy constants to command heap — skip when version unchanged (reuse argbuf offset)
  void *vs_const_data = nullptr;
  void *ps_const_data = nullptr;
  if (!vs_const_reuse || !ps_const_reuse) {
    uint64_t alloc_size = vs_alloc_size + ps_alloc_size;
    void *const_base = alloc_size ? queue.AllocateCommandData(alloc_size, 16) : nullptr;
    char *p = (char *)const_base;
    if (!vs_const_reuse) {
      vs_const_data = p;
      memcpy(vs_const_data, current_vs_ ? (const void *)vsConstants_ : (const void *)cached_ff_vs_, vs_const_buf_size);
      p += vs_const_buf_size;
    }
    if (!ps_const_reuse) {
      ps_const_data = p;
      memset(ps_const_data, 0, ps_const_buf_size);
      if (current_ps_) {
        uint16_t psc = (uint16_t)std::min(current_ps_->maxConstantReg(), 256u);
        memcpy(ps_const_data, psConstants_, psc * 4 * sizeof(float));
        float *ps_out = (float *)ps_const_data;
        if (render_states_[D3DRS_ALPHATESTENABLE] && render_states_[D3DRS_ALPHAFUNC] != D3DCMP_ALWAYS)
          ps_out[255 * 4 + 0] = (float)(render_states_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
        if (render_states_[D3DRS_FOGENABLE]) {
          DWORD fc = render_states_[D3DRS_FOGCOLOR];
          ps_out[255 * 4 + 1] = (float)((fc >> 16) & 0xFF) / 255.0f;
          ps_out[255 * 4 + 2] = (float)((fc >> 8) & 0xFF) / 255.0f;
          ps_out[255 * 4 + 3] = (float)(fc & 0xFF) / 255.0f;
          float fogStart, fogEnd, fogDensity;
          memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
          memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
          memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
          ps_out[254 * 4 + 0] = fogStart;
          ps_out[254 * 4 + 1] = fogEnd;
          ps_out[254 * 4 + 2] = fogDensity;
        }
      } else {
        memcpy(ps_const_data, cached_ff_ps_, 3 * 4 * sizeof(float));
      }
    }
  }

  // Capture texture state (allocation captured at API time to avoid encoding-thread race)
  // Skip when fingerprint unchanged — textures already resident from previous draw in this pass
  bool tex_reuse = (tex_sampler_fingerprint_ == last_tex_fingerprint_) && last_ps_tex_argbuf_off_;

  // State change tracking
  D3D9CompiledPipeline *pso = cached_pso_;
  bool emit_pso = (pso != last_emitted_pso_);
  obj_handle_t dsso = cached_dsso_;
  uint32_t stencil_ref = cached_stencil_ref_;
  bool emit_dsso = dsso && (dsso != last_emitted_dsso_ || stencil_ref != last_emitted_stencil_ref_);
  bool emit_cull = (cull_mode != last_emitted_cull_);
  D3DVIEWPORT9 vp = viewport_;
  bool emit_vp = vp_dirty_ || memcmp(&vp, &last_emitted_vp_, sizeof(D3DVIEWPORT9)) != 0;
  bool scissor_enable = render_states_[D3DRS_SCISSORTESTENABLE] != 0;
  RECT scissor = scissor_rect_;
  bool emit_scissor = false;
  if (scissor_enable) {
    emit_scissor = !last_emitted_scissor_enable_ ||
                   memcmp(&scissor, &last_emitted_scissor_, sizeof(RECT)) != 0;
  } else if (last_emitted_scissor_enable_) {
    emit_scissor = true;
    scissor = {(LONG)vp.X, (LONG)vp.Y,
               (LONG)(vp.X + vp.Width), (LONG)(vp.Y + vp.Height)};
  }

  if (emit_pso) last_emitted_pso_ = pso;
  if (emit_dsso) { last_emitted_dsso_ = dsso; last_emitted_stencil_ref_ = stencil_ref; }
  if (emit_cull) last_emitted_cull_ = cull_mode;
  if (emit_vp) last_emitted_vp_ = vp;
  last_emitted_scissor_enable_ = scissor_enable;
  if (emit_scissor) last_emitted_scissor_ = scissor;
  vp_dirty_ = false;
  scissor_dirty_ = false;

  // Emit render state + constants + VS struct
  chunk->emitcc([=](ArgumentEncodingContext &ctx) {
    if (emit_pso) {
      auto pso_handle = pso->GetPipeline();
      if (!pso_handle) return; // PSO compilation failed — skip this draw
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO; cmd.pso = pso_handle;
    }
    if (emit_dsso) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
      cmd.type = WMTRenderCommandSetDSSO; cmd.dsso = dsso; cmd.stencil_ref = stencil_ref;
    }
    if (emit_cull) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd.type = WMTRenderCommandSetRasterizerState;
      cmd.fill_mode = WMTTriangleFillModeFill; cmd.cull_mode = cull_mode;
      cmd.depth_clip_mode = WMTDepthClipModeClip; cmd.winding = WMTWindingClockwise;
      cmd.depth_bias = 0.0f; cmd.scole_scale = 0.0f; cmd.depth_bias_clamp = 0.0f;
    }
    if (emit_vp) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
      cmd.type = WMTRenderCommandSetViewport;
      cmd.viewport = {(double)vp.X, (double)vp.Y, (double)vp.Width, (double)vp.Height,
                      (double)vp.MinZ, (double)vp.MaxZ};
    }
    if (emit_scissor) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setscissorrect>();
      cmd.type = WMTRenderCommandSetScissorRect;
      LONG left = scissor.left < 0 ? 0 : scissor.left;
      LONG top = scissor.top < 0 ? 0 : scissor.top;
      cmd.scissor_rect = {(uint64_t)left, (uint64_t)top,
                           (uint64_t)(scissor.right > left ? scissor.right - left : 0),
                           (uint64_t)(scissor.bottom > top ? scissor.bottom - top : 0)};
    }

    if (vs_const_data)
      memcpy(ctx.getMappedArgumentBuffer<char>(vs_const_off_local), vs_const_data, vs_const_buf_size);
    if (ps_const_data)
      memcpy(ctx.getMappedArgumentBuffer<char>(ps_const_off_local), ps_const_data, ps_const_buf_size);

    // VS argument struct at fixed offset 0 (never changes between draws)
    auto *vs_s = ctx.getMappedArgumentBuffer<uint64_t>(vs_struct_off);
    vs_s[0] = ctx.getArgumentBufferGPUAddress(vs_const_off_local);
    vs_s[1] = (uint64_t)vs_const_count;
    // Pack half-pixel offset as two floats in a uint64: float2(1/w, 1/h)
    {
      float hpx = 1.0f / (float)vp.Width;
      float hpy = 1.0f / (float)vp.Height;
      uint32_t hpx_bits, hpy_bits;
      memcpy(&hpx_bits, &hpx, 4);
      memcpy(&hpy_bits, &hpy, 4);
      vs_s[2] = (uint64_t)hpx_bits | ((uint64_t)hpy_bits << 32);
    }
    // VB entries at index 16 (direct buffer, supports offset changes between draws)
    { auto &c = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
      c.type = WMTRenderCommandSetVertexBufferOffset;
      c.offset = ctx.getFinalArgumentBufferOffset(vb_off); c.index = 16; }
  });

  // Emit VB entries — skip entirely when fingerprint unchanged (same argbuf offset reused)
  if (!vb_reuse && slot_mask) {
    // Full path: capture and write VB entries
    struct VBCapture {
      WMT::Buffer raw_buffer;
      uint64_t gpu_address = 0;
      UINT offset = 0;
      UINT stride = 0;
    };
    std::array<VBCapture, 16> vb_captures;
    for (uint32_t mask = slot_mask, idx = 0; mask; mask &= mask - 1) {
      uint32_t slot = __builtin_ctz(mask);
      auto &vc = vb_captures[idx++];
      if (slot == 0 && transient_vb_override_.active) {
        vc.raw_buffer = transient_vb_override_.buffer;
        vc.gpu_address = transient_vb_override_.gpu_address;
        vc.offset = 0;
        vc.stride = transient_vb_override_.stride;
      } else {
        vc.stride = stream_strides_[slot];
        vc.offset = stream_offsets_[slot];
        if (stream_sources_[slot]) {
          vc.raw_buffer = stream_sources_[slot]->rawBuffer();
          vc.gpu_address = stream_sources_[slot]->gpuAddress();
        }
      }
    }
    chunk->emitcc([=, vb_captures = std::move(vb_captures)](ArgumentEncodingContext &ctx) mutable {
      struct VERTEX_BUFFER_ENTRY { uint64_t buffer_handle; uint32_t stride; uint32_t length; };
      auto *entries = ctx.getMappedArgumentBuffer<VERTEX_BUFFER_ENTRY>(vb_off);
      for (uint32_t mask = slot_mask, idx = 0; mask; mask &= mask - 1, idx++) {
        auto &vc = vb_captures[idx];
        if (vc.gpu_address) {
          entries[idx].buffer_handle = vc.gpu_address + vc.offset;
          entries[idx].stride = vc.stride;
          entries[idx].length = vc.stride ? 0xFFFFFFFF : 0;
          ctx.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(
              vc.raw_buffer, DXMT_RESOURCE_RESIDENCY_VERTEX_READ);
        } else {
          entries[idx] = {0, 0, 0};
        }
      }
    });
  }

  // Emit PS argument buffer: constants + texture bindings
  if (tex_reuse) {
    // Fast path: reuse previous texture bindings from argbuf
    uint64_t prev_ps_tex_off = last_ps_tex_argbuf_off_;
    chunk->emitcc([=](ArgumentEncodingContext &ctx) {
      auto *ps_s = ctx.getMappedArgumentBuffer<uint64_t>(ps_struct_off);
      ps_s[0] = ctx.getArgumentBufferGPUAddress(ps_const_off_local);
      ps_s[1] = (uint64_t)ps_const_count;
      memcpy(&ps_s[2], ctx.getMappedArgumentBuffer<uint64_t>(prev_ps_tex_off), 16 * sizeof(uint64_t));
      { auto &c = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
        c.type = WMTRenderCommandSetFragmentBufferOffset;
        c.offset = ctx.getFinalArgumentBufferOffset(ps_struct_off); c.index = 30; }
    });
  } else {
    struct TexBindCapture {
      Rc<Texture> texture;
      Rc<TextureAllocation> alloc;
      TextureViewKey viewKey;
      uint32_t stage;
      uint64_t sampler_gpu_id;
    };
    uint8_t tex_count = tex_capture_count_;
    std::array<TexBindCapture, 16> tex_binds;
    for (uint8_t i = 0; i < tex_count; i++) {
      auto &tc = tex_captures_[i];
      tex_binds[i] = {Rc<Texture>(tc.texture), Rc<TextureAllocation>(tc.texture->current()),
                      tc.viewKey, tc.stage, sampler_gpu_ids_[tc.stage]};
    }
    chunk->emitcc([=, tex_binds = std::move(tex_binds)](ArgumentEncodingContext &ctx) mutable {
      auto *ps_s = ctx.getMappedArgumentBuffer<uint64_t>(ps_struct_off);
      ps_s[0] = ctx.getArgumentBufferGPUAddress(ps_const_off_local);
      ps_s[1] = (uint64_t)ps_const_count;
      for (uint32_t i = 0; i < 16; i++) ps_s[2 + i] = 0;
      for (uint8_t i = 0; i < tex_count; i++) {
        auto &tb = tex_binds[i];
        auto &view = ctx.access(tb.texture, tb.alloc.ptr(), tb.viewKey, DXMT_ENCODER_RESOURCE_ACESS_READ);
        ps_s[2 + tb.stage * 2] = view.gpuResourceID;
        ps_s[3 + tb.stage * 2] = tb.sampler_gpu_id;
        ctx.makeResident<PipelineStage::Pixel, PipelineKind::Ordinary>(view);
      }
      { auto &c = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
        c.type = WMTRenderCommandSetFragmentBufferOffset;
        c.offset = ctx.getFinalArgumentBufferOffset(ps_struct_off); c.index = 30; }
    });
  }

  // Track argbuf offsets for reuse
  last_tex_fingerprint_ = tex_sampler_fingerprint_;
  last_ps_tex_argbuf_off_ = ps_struct_off + 2 * sizeof(uint64_t);
  last_vb_fingerprint_ = vb_fingerprint;
  last_vb_argbuf_off_ = vb_off;
  last_vb_num_slots_ = num_vb_slots;
  last_vs_const_version_ = vs_ver;
  last_vs_const_argbuf_off_ = vs_const_off_local;
  last_vs_const_count_ = vs_const_count;
  last_ps_const_version_ = ps_ver;
  last_ps_const_argbuf_off_ = ps_const_off_local;
  last_ps_const_count_ = ps_const_count;

#ifdef DXMT_PERF
  if (perf_sample) dxmt_device_->queue().CurrentFrameStatistics().d3d9_capture_ticks += rdtsc() - perf_t;
#endif

  return true;
}

void D3D9Device::EmitDrawCommand(WMTPrimitiveType primType, UINT vertexStart, UINT vertexCount) {
  auto chunk = dxmt_device_->queue().CurrentChunk();
  chunk->emitcc([primType, vertexStart, vertexCount](ArgumentEncodingContext &ctx) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw>();
    cmd.type = WMTRenderCommandDraw;
    cmd.primitive_type = primType;
    cmd.vertex_start = vertexStart;
    cmd.vertex_count = vertexCount;
    cmd.instance_count = 1;
    cmd.base_instance = 0;
  });
}

void D3D9Device::EmitDrawIndexedCommand(WMTPrimitiveType primType, UINT indexCount,
                                         WMTIndexType indexType,
                                         WMT::Buffer ibRawBuffer, uint64_t ibOffset, INT baseVertex,
                                         Rc<BufferAllocation> ibAllocKeepAlive) {
  auto chunk = dxmt_device_->queue().CurrentChunk();
  chunk->emitcc([=, ibAllocKeepAlive = std::move(ibAllocKeepAlive)](ArgumentEncodingContext &ctx) {
    ctx.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(
        ibRawBuffer, DXMT_RESOURCE_RESIDENCY_VERTEX_READ);
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd.type = WMTRenderCommandDrawIndexed;
    cmd.primitive_type = primType;
    cmd.index_count = indexCount;
    cmd.index_type = indexType;
    cmd.index_buffer = ibRawBuffer;
    cmd.index_buffer_offset = ibOffset;
    cmd.instance_count = 1;
    cmd.base_vertex = baseVertex;
    cmd.base_instance = 0;
  });
}

// ============================================================
// END: New emit-on-draw infrastructure
// ============================================================

// Kept from old code:
// (EmitCommonRenderSetup removed — logic inlined into PreDraw above)
// (EmitDrawCommand removed — replaced by EmitDrawCommand/EmitDrawIndexedCommand above)
// (FlushDrawBatch removed — replaced by InvalidateCurrentPass above)
// (QueueBatchedDraw removed — no batching layer)
// (BuildDrawCapture removed — logic inlined into PreDraw above)

// DrawPrimitive
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_count++;
#endif
  if (!current_vdecl_) return D3DERR_INVALIDCALL;

  UINT vertexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, vertexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto &queue = dxmt_device_->queue();
    auto fanIB = GenerateFanIndices(queue, PrimitiveCount, StartVertex);
    if (!PreDraw(mtlPrimType)) return D3DERR_INVALIDCALL;
    EmitDrawIndexedCommand(mtlPrimType, PrimitiveCount * 3, WMTIndexTypeUInt16,
                           fanIB.buffer, fanIB.offset, 0);
    return S_OK;
  }

  if (!PreDraw(mtlPrimType)) return D3DERR_INVALIDCALL;
  EmitDrawCommand(mtlPrimType, StartVertex, vertexCount);
  return S_OK;
}

// DrawIndexedPrimitive
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
    UINT MinVertexIndex, UINT NumVertices,
    UINT StartIndex, UINT PrimitiveCount) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_count++;
#endif
  if (!current_vdecl_ || !current_ib_) return D3DERR_INVALIDCALL;

  UINT indexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, indexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  // Triangle fan with index buffer: expand fan indices to triangle list
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    bool is32 = (current_ib_->format() == D3DFMT_INDEX32);
    UINT srcStride = is32 ? 4 : 2;
    auto *srcData = (const uint8_t *)current_ib_->mappedMemory()
                    + StartIndex * srcStride;

    std::vector<uint16_t> expandedIndices(indexCount);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      uint32_t i0, i1, i2;
      if (is32) {
        i0 = ((const uint32_t *)srcData)[0];
        i1 = ((const uint32_t *)srcData)[i + 1];
        i2 = ((const uint32_t *)srcData)[i + 2];
      } else {
        i0 = ((const uint16_t *)srcData)[0];
        i1 = ((const uint16_t *)srcData)[i + 1];
        i2 = ((const uint16_t *)srcData)[i + 2];
      }
      expandedIndices[i * 3 + 0] = (uint16_t)i0;
      expandedIndices[i * 3 + 1] = (uint16_t)i1;
      expandedIndices[i * 3 + 2] = (uint16_t)i2;
    }

    auto &queue = dxmt_device_->queue();
    UINT ibSize = indexCount * sizeof(uint16_t);
    auto fanIB = queue.AllocateTransientBuffer(ibSize, 2);
    memcpy(fanIB.cpu_ptr, expandedIndices.data(), ibSize);

    if (!PreDraw(mtlPrimType)) return D3DERR_INVALIDCALL;
    EmitDrawIndexedCommand(mtlPrimType, indexCount, WMTIndexTypeUInt16,
                           fanIB.buffer, fanIB.offset, BaseVertexIndex);
    return S_OK;
  }

  if (!PreDraw(mtlPrimType)) return D3DERR_INVALIDCALL;

  WMTIndexType indexType = (current_ib_->format() == D3DFMT_INDEX32)
      ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
  UINT indexStride = (indexType == WMTIndexTypeUInt32) ? 4 : 2;
  uint64_t indexBufferOffset = StartIndex * indexStride;

  // Capture buffer state at draw time — Lock may change backing storage
  EmitDrawIndexedCommand(mtlPrimType, indexCount, indexType,
                         current_ib_->rawBuffer(), indexBufferOffset, BaseVertexIndex);
  return S_OK;
}

// DrawPrimitiveUP
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_up_count++;
#endif
  if (!current_vdecl_ || !pVertexStreamZeroData)
    return D3DERR_INVALIDCALL;

  UINT vertexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, vertexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  auto &queue = dxmt_device_->queue();

  // For fan, we need all fan vertices (primCount + 2), not the expanded count
  UINT sourceVertexCount = (PrimitiveType == D3DPT_TRIANGLEFAN)
    ? PrimitiveCount + 2 : vertexCount;
  UINT dataSize = sourceVertexCount * VertexStreamZeroStride;
  auto transientVB = queue.AllocateTransientBuffer(dataSize, 16);
  memcpy(transientVB.cpu_ptr, pVertexStreamZeroData, dataSize);

  // Save and set stream source
  auto savedVB = stream_sources_[0];
  auto savedOffset = stream_offsets_[0];
  auto savedStride = stream_strides_[0];

  // Set transient VB override for PreDraw to use
  transient_vb_override_ = {true, transientVB.buffer, transientVB.gpu_address, VertexStreamZeroStride};
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = VertexStreamZeroStride;
  vb_dirty_ = true;

  if (!PreDraw(mtlPrimType)) {
    transient_vb_override_.active = false;
    stream_sources_[0] = savedVB;
    stream_offsets_[0] = savedOffset;
    stream_strides_[0] = savedStride;
    return D3DERR_INVALIDCALL;
  }

  transient_vb_override_.active = false;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto fanIB = GenerateFanIndices(queue, PrimitiveCount, 0);
    EmitDrawIndexedCommand(mtlPrimType, PrimitiveCount * 3, WMTIndexTypeUInt16,
                           fanIB.buffer, fanIB.offset, 0);
  } else {
    EmitDrawCommand(mtlPrimType, 0, vertexCount);
  }

  // Restore stream source
  stream_sources_[0] = savedVB;
  stream_offsets_[0] = savedOffset;
  stream_strides_[0] = savedStride;
  vb_dirty_ = true;

  // Per D3D9 spec: clear stream source and index buffer after UP draws
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = 0;
  current_ib_ = nullptr;

  return S_OK;
}

// DrawIndexedPrimitiveUP
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
    UINT NumVertices, UINT PrimitiveCount,
    const void *pIndexData, D3DFORMAT IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
#ifdef DXMT_PERF
  dxmt_device_->queue().CurrentFrameStatistics().d3d9_draw_up_count++;
#endif
  if (!current_vdecl_ || !pIndexData || !pVertexStreamZeroData)
    return D3DERR_INVALIDCALL;

  UINT indexCount = 0;
  WMTPrimitiveType mtlPrimType;
  if (!GetPrimitiveInfo(PrimitiveType, PrimitiveCount, indexCount, mtlPrimType))
    return D3DERR_INVALIDCALL;

  auto &queue = dxmt_device_->queue();

  // Create transient vertex buffer from ring
  UINT vbDataSize = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;
  auto transientVB = queue.AllocateTransientBuffer(vbDataSize, 16);
  memcpy(transientVB.cpu_ptr, pVertexStreamZeroData, vbDataSize);

  // Create transient index buffer — expand fan if needed
  CommandQueue::TransientAllocation transientIB{};
  UINT indexStride = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    // Expand fan → triangle list
    bool is32 = (IndexDataFormat == D3DFMT_INDEX32);
    const uint8_t *srcData = (const uint8_t *)pIndexData;
    UINT ibSize = indexCount * sizeof(uint16_t);
    transientIB = queue.AllocateTransientBuffer(ibSize, 2);
    auto *expandedIndices = static_cast<uint16_t*>(transientIB.cpu_ptr);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      uint32_t i0, i1, i2;
      if (is32) {
        i0 = ((const uint32_t *)srcData)[0];
        i1 = ((const uint32_t *)srcData)[i + 1];
        i2 = ((const uint32_t *)srcData)[i + 2];
      } else {
        i0 = ((const uint16_t *)srcData)[0];
        i1 = ((const uint16_t *)srcData)[i + 1];
        i2 = ((const uint16_t *)srcData)[i + 2];
      }
      expandedIndices[i * 3 + 0] = (uint16_t)i0;
      expandedIndices[i * 3 + 1] = (uint16_t)i1;
      expandedIndices[i * 3 + 2] = (uint16_t)i2;
    }
    indexStride = 2;
  } else {
    UINT ibDataSize = indexCount * indexStride;
    transientIB = queue.AllocateTransientBuffer(ibDataSize, indexStride);
    memcpy(transientIB.cpu_ptr, pIndexData, ibDataSize);
  }

  WMTIndexType indexType = (PrimitiveType == D3DPT_TRIANGLEFAN)
      ? WMTIndexTypeUInt16
      : ((IndexDataFormat == D3DFMT_INDEX32) ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16);

  // Set transient VB override and draw
  auto savedVB = stream_sources_[0];
  auto savedOffset = stream_offsets_[0];
  auto savedStride = stream_strides_[0];

  transient_vb_override_ = {true, transientVB.buffer, transientVB.gpu_address, VertexStreamZeroStride};
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = VertexStreamZeroStride;
  vb_dirty_ = true;

  if (!PreDraw(mtlPrimType)) {
    transient_vb_override_.active = false;
    stream_sources_[0] = savedVB;
    stream_offsets_[0] = savedOffset;
    stream_strides_[0] = savedStride;
    return D3DERR_INVALIDCALL;
  }

  transient_vb_override_.active = false;
  EmitDrawIndexedCommand(mtlPrimType, indexCount, indexType,
                         transientIB.buffer, transientIB.offset, 0);

  stream_sources_[0] = savedVB;
  stream_offsets_[0] = savedOffset;
  stream_strides_[0] = savedStride;
  vb_dirty_ = true;

  // Per D3D9 spec: clear stream source and index buffer after UP draws
  stream_sources_[0] = nullptr;
  stream_offsets_[0] = 0;
  stream_strides_[0] = 0;
  current_ib_ = nullptr;

  return S_OK;
}

// Present
HRESULT STDMETHODCALLTYPE D3D9Device::Present(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *) {
  if (pSourceRect || pDestRect || hDestWindowOverride) {
    static bool warned = false;
    if (!warned) {
      Logger::warn("D3D9: Present with source/dest rects or window override not implemented");
      warned = true;
    }
  }

#ifdef DXMT_PERF
  auto &perf_stats = dxmt_device_->queue().CurrentFrameStatistics();
  auto now = rdtsc();
  perf_stats.d3d9_frame_ticks = now - frame_start_tsc_;
  frame_start_tsc_ = now;
  perf_draw_counter_ = 0;
#endif

  dynamic_buffer_ring_->free_blocks(dxmt_device_->queue().CoherentSeqId());

  InvalidateCurrentPass();

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  double vsync = (present_params_.PresentationInterval & D3DPRESENT_INTERVAL_IMMEDIATE)
      ? 0.0 : 1.0 / init_refresh_rate_;

  chunk->emitcc([
    this,
    state = presenter_->synchronizeLayerProperties(),
    backbuffer = backbuffer_,
    bb_alloc = Rc<TextureAllocation>(backbuffer_->current()),
    presenter = presenter_,
    vsync
  ](ArgumentEncodingContext &ctx) mutable {
    ctx.present(backbuffer, bb_alloc.ptr(), presenter, vsync, state.metadata);
    UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
  });

  chunk->signal_frame_latency_fence_ = queue.CurrentFrameSeq();
  queue.CommitCurrentChunk();
  queue.PresentBoundary();
  return S_OK;
}

void D3D9Device::UpdateStatistics(const FrameStatisticsContainer &statistics, uint64_t frame_id) {
#ifdef DXMT_PERF
  if (frame_id % 15 != 0) return;

  hud_.beginUpdate();
  auto &avg = statistics.average();
  auto ms = [](clock::duration d) -> double { return d.count() / 1000000.0; };
  char buf[128];
  int line = 0;

  // Capture time sampled every 32nd draw — scale up
  constexpr double S = 32.0;
  auto tms = [this](uint64_t ticks) -> double { return (double)ticks / tsc_per_ms_; };
  double f = tms(avg.d3d9_frame_ticks);
  double cap = tms(avg.d3d9_capture_ticks) * S;
  double lock = tms(avg.d3d9_lock_ticks);
  double d3d9_pct = f > 0 ? (cap + lock) / f * 100.0 : 0;
  double enc_total = ms(avg.encode_prepare_interval) + ms(avg.encode_flush_interval);

  // Encoder thread
  snprintf(buf, sizeof(buf), "-- Encoder %.1fms --", enc_total);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Prepare  %.1fms", ms(avg.encode_prepare_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Flush    %.1fms", ms(avg.encode_flush_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Commit   %.1fms", ms(avg.commit_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  CmdBuf   %u", avg.command_buffer_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Render   %u(%umerged)",
      avg.render_pass_count, avg.render_pass_optimized);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Clear    %u(%umerged)",
      avg.clear_pass_count, avg.clear_pass_optimized);
  hud_.updateLine(line++, buf);

  // API thread
  snprintf(buf, sizeof(buf), "-- API %.1fms (%.0f%% d3d9) --", f, d3d9_pct);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Latency  %.1fms", ms(avg.present_lantency_interval));
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Draws    %u(+%uUP)",
      avg.d3d9_draw_count, avg.d3d9_draw_up_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  States   %u", avg.d3d9_state_change_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Capture  %.1fms(%umiss)", cap, avg.d3d9_pso_miss_count);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Lock     %.1fms", lock);
  hud_.updateLine(line++, buf);
  snprintf(buf, sizeof(buf), "  Other    %.1fms", f - cap - lock);
  hud_.updateLine(line++, buf);

  hud_.endUpdate();
#endif
}

// CreateOffscreenPlainSurface
HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  Logger::err(str::format("TRACE: CreateOffscreenPlainSurface ", Width, "x", Height, " fmt=", (int)Format, " pool=", (int)Pool));
  if (!ppSurface) return D3DERR_INVALIDCALL;
  if (Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;

  *ppSurface = ref(new D3D9Surface(this, Width, Height, Format, Pool));
  return S_OK;
}

// GetRenderTargetData - copy render target to system memory surface
HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTargetData(
    IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  InvalidateCurrentPass();
  if (!pRenderTarget || !pDestSurface) return D3DERR_INVALIDCALL;

  auto *dst = static_cast<D3D9Surface *>(pDestSurface);
  if (dst->pool() != D3DPOOL_SYSTEMMEM || !dst->sysMemData())
    return D3DERR_INVALIDCALL;

  auto *src = static_cast<D3D9Surface *>(pRenderTarget);
  if (!src->texture()) return D3DERR_INVALIDCALL;

  UINT width = src->texture()->width();
  UINT height = src->texture()->height();
  UINT bytesPerRow = width * 4;
  UINT totalBytes = bytesPerRow * height;
  UINT alignedSize = (totalBytes + 0xFFF) & ~0xFFFu; // page-align

  // Create a shared buffer for readback
  void *placedMem = wsi::aligned_malloc(alignedSize, DXMT_PAGE_SIZE);
  if (!placedMem) return D3DERR_INVALIDCALL;

  WMTBufferInfo bufInfo = {};
  bufInfo.length = alignedSize;
  bufInfo.options = WMTResourceStorageModeShared;
  bufInfo.memory.set(placedMem);
  auto readbackBuf = dxmt_device_->device().newBuffer(bufInfo);

  InvalidateCurrentPass();

  // Emit blit through the standard command queue
  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  chunk->emitcc([
    srcTex = src->texture(),
    readbackBufHandle = readbackBuf.handle,
    width, height, bytesPerRow, totalBytes
  ](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();

    auto srcHandle = ctx.access(srcTex, 0u, 0u, DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
    blitCmd.type = WMTBlitCommandCopyFromTextureToBuffer;
    blitCmd.src = srcHandle;
    blitCmd.slice = 0;
    blitCmd.level = 0;
    blitCmd.origin = {0, 0, 0};
    blitCmd.size = {width, height, 1};
    blitCmd.dst = readbackBufHandle;
    blitCmd.offset = 0;
    blitCmd.bytes_per_row = bytesPerRow;
    blitCmd.bytes_per_image = totalBytes;

    ctx.endPass();
  });

  queue.CommitCurrentChunk();
  queue.WaitForIdle();

  // Copy from shared buffer to system memory surface
  std::memcpy(dst->sysMemData(), placedMem, totalBytes);

  wsi::aligned_free(placedMem);
  return S_OK;
}

// Fixed-function pipeline helpers

FFVSKey D3D9Device::BuildFFVSKey() {
  FFVSKey key = {};
  if (!current_vdecl_) return key;

  for (auto &elem : current_vdecl_->elements()) {
    if (elem.Stream == 0xFF) break;
    switch (elem.Usage) {
    case D3DDECLUSAGE_POSITIONT:
      key.has_position_t = 1;
      break;
    case D3DDECLUSAGE_NORMAL:
      key.has_normal = 1;
      break;
    case D3DDECLUSAGE_COLOR:
      if (elem.UsageIndex == 0) key.has_color0 = 1;
      else if (elem.UsageIndex == 1) key.has_color1 = 1;
      break;
    case D3DDECLUSAGE_TEXCOORD:
      if (elem.UsageIndex + 1 > key.tex_coord_count)
        key.tex_coord_count = elem.UsageIndex + 1;
      break;
    default:
      break;
    }
  }

  // TCI generation modes and texture transform flags per texcoord output
  for (uint8_t i = 0; i < key.tex_coord_count && i < 8; i++) {
    DWORD tci = texture_stage_states_[i][D3DTSS_TEXCOORDINDEX];
    key.tci_modes[i] = (uint8_t)((tci >> 16) & 0x3); // 0=passthru, 1=normal, 2=position, 3=reflection
    key.tci_coord_indices[i] = (uint8_t)(tci & 0x7);
    key.ttf_modes[i] = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXTURETRANSFORMFLAGS] & 0x7);
  }

  // Fog mode: prefer vertex fog, fall back to table fog mode
  // (table fog is normally per-pixel, but we approximate per-vertex in the FF VS)
  if (render_states_[D3DRS_FOGENABLE]) {
    DWORD fogMode = render_states_[D3DRS_FOGVERTEXMODE];
    if (fogMode == D3DFOG_NONE)
      fogMode = render_states_[D3DRS_FOGTABLEMODE];
    switch (fogMode) {
    case D3DFOG_EXP:    key.fog_mode = 1; break;
    case D3DFOG_EXP2:   key.fog_mode = 2; break;
    case D3DFOG_LINEAR: key.fog_mode = 3; break;
    default:            key.fog_mode = 0; break;
    }
  }

  // Lighting (only for non-POSITIONT vertices)
  if (render_states_[D3DRS_LIGHTING] && !key.has_position_t) {
    key.lighting_enabled = 1;
    key.normalize_normals = render_states_[D3DRS_NORMALIZENORMALS] ? 1 : 0;
    key.color_vertex = render_states_[D3DRS_COLORVERTEX] ? 1 : 0;
    key.diffuse_source = (uint8_t)render_states_[D3DRS_DIFFUSEMATERIALSOURCE];
    key.ambient_source = (uint8_t)render_states_[D3DRS_AMBIENTMATERIALSOURCE];
    key.specular_source = (uint8_t)render_states_[D3DRS_SPECULARMATERIALSOURCE];
    key.emissive_source = (uint8_t)render_states_[D3DRS_EMISSIVEMATERIALSOURCE];

    uint8_t count = 0;
    for (DWORD i = 0; i < kMaxLights && count < 8; i++) {
      if (light_enabled_[i]) {
        key.light_types[count] = (uint8_t)lights_[i].Type;
        count++;
      }
    }
    key.num_active_lights = count;
  }

  return key;
}

FFPSKey D3D9Device::BuildFFPSKey() {
  FFPSKey key = {};
  for (uint32_t i = 0; i < 8; i++) {
    auto &stage = key.stages[i];
    stage.color_op   = (uint8_t)texture_stage_states_[i][D3DTSS_COLOROP];
    stage.color_arg1 = (uint8_t)texture_stage_states_[i][D3DTSS_COLORARG1];
    stage.color_arg2 = (uint8_t)texture_stage_states_[i][D3DTSS_COLORARG2];
    stage.alpha_op   = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAOP];
    stage.alpha_arg1 = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAARG1];
    stage.alpha_arg2 = (uint8_t)texture_stage_states_[i][D3DTSS_ALPHAARG2];
    stage.has_texture = bound_textures_[i] ? 1 : 0;
    stage.texcoord_index = (uint8_t)(texture_stage_states_[i][D3DTSS_TEXCOORDINDEX] & 0x7);
    if (stage.color_op == D3DTOP_DISABLE) break;
  }
  key.specular_enable = render_states_[D3DRS_SPECULARENABLE] ? 1 : 0;
  key.alpha_test_enable = render_states_[D3DRS_ALPHATESTENABLE] ? 1 : 0;
  key.alpha_test_func = (uint8_t)render_states_[D3DRS_ALPHAFUNC];
  key.fog_enable = render_states_[D3DRS_FOGENABLE] ? 1 : 0;
  return key;
}

WMT::Reference<WMT::Function> D3D9Device::GetOrCreateFFVS(
    const FFVSKey &key, D3D9VertexDeclaration *vdecl) {
  auto it = ff_vs_cache_.find(key);
  if (it != ff_vs_cache_.end())
    return it->second;

  auto &elements = vdecl->elements();
  uint32_t slotMask = vdecl->slotMask();
  uint32_t numElements = elements.size() > 0 ? (uint32_t)elements.size() - 1 : 0; // exclude end sentinel

  auto func = GenerateFFVertexShader(
    dxmt_device_->device(), key, elements.data(), numElements, slotMask,
    dxmt_device_->metalVersion());
  if (func)
    ff_vs_cache_.emplace(key, func);
  return func;
}

WMT::Reference<WMT::Function> D3D9Device::GetOrCreateFFPS(const FFPSKey &key) {
  auto it = ff_ps_cache_.find(key);
  if (it != ff_ps_cache_.end())
    return it->second;

  auto func = GenerateFFPixelShader(
    dxmt_device_->device(), key, key.tex_coord_count,
    dxmt_device_->metalVersion());
  if (func)
    ff_ps_cache_.emplace(key, func);
  return func;
}

static void TransformDirectionByMatrix(const D3DMATRIX &m, float inX, float inY, float inZ,
                                        float &outX, float &outY, float &outZ) {
  outX = inX * m._11 + inY * m._21 + inZ * m._31;
  outY = inX * m._12 + inY * m._22 + inZ * m._32;
  outZ = inX * m._13 + inY * m._23 + inZ * m._33;
}

static void TransformPointByMatrix(const D3DMATRIX &m, float inX, float inY, float inZ,
                                   float &outX, float &outY, float &outZ) {
  outX = inX * m._11 + inY * m._21 + inZ * m._31 + m._41;
  outY = inX * m._12 + inY * m._22 + inZ * m._32 + m._42;
  outZ = inX * m._13 + inY * m._23 + inZ * m._33 + m._43;
}

void D3D9Device::UpdateFFConstants() {
  auto *vs = cached_ff_vs_;
  uint32_t dirty = ff_dirty_;
  if (!dirty) return;

  auto &world = transforms_[TransformIndex(D3DTS_WORLD)];
  auto &view = transforms_[TransformIndex(D3DTS_VIEW)];
  auto &proj = transforms_[TransformIndex(D3DTS_PROJECTION)];

  // Transforms: world/view/proj always needed for WorldView/WVP computation
  if (dirty & (kFFDirtyWorld | kFFDirtyView | kFFDirtyProj)) {
    if (dirty & kFFDirtyWorld) memcpy(&vs[0], &world, sizeof(D3DMATRIX));
    if (dirty & kFFDirtyView) memcpy(&vs[4], &view, sizeof(D3DMATRIX));
    if (dirty & kFFDirtyProj) memcpy(&vs[8], &proj, sizeof(D3DMATRIX));
    // WorldView and WVP must be recomputed when any transform changes
    auto wv = MultiplyMatrices(world, view);
    memcpy(&vs[12], &wv, sizeof(D3DMATRIX));
    auto wvp = MultiplyMatrices(wv, proj);
    memcpy(&vs[16], &wvp, sizeof(D3DMATRIX));
  }

  if (dirty & kFFDirtyViewport) {
    vs[20][0] = (float)viewport_.Width;
    vs[20][1] = (float)viewport_.Height;
    vs[20][2] = 0.0f;
    vs[20][3] = 0.0f;
  }

  if (dirty & kFFDirtyFog) {
    float fogStart, fogEnd, fogDensity;
    memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
    memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
    memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
    vs[21][0] = fogStart;
    vs[21][1] = fogEnd;
    vs[21][2] = fogDensity;
    vs[21][3] = 0.0f;
  }

  if (dirty & kFFDirtyAmbient) {
    DWORD amb = render_states_[D3DRS_AMBIENT];
    vs[22][0] = (float)((amb >> 16) & 0xFF) / 255.0f;
    vs[22][1] = (float)((amb >>  8) & 0xFF) / 255.0f;
    vs[22][2] = (float)((amb >>  0) & 0xFF) / 255.0f;
    vs[22][3] = (float)((amb >> 24) & 0xFF) / 255.0f;
  }

  if (dirty & kFFDirtyMaterial) {
    vs[23][0] = material_.Diffuse.r;  vs[23][1] = material_.Diffuse.g;
    vs[23][2] = material_.Diffuse.b;  vs[23][3] = material_.Diffuse.a;
    vs[24][0] = material_.Ambient.r;  vs[24][1] = material_.Ambient.g;
    vs[24][2] = material_.Ambient.b;  vs[24][3] = material_.Ambient.a;
    vs[25][0] = material_.Specular.r; vs[25][1] = material_.Specular.g;
    vs[25][2] = material_.Specular.b; vs[25][3] = material_.Specular.a;
    vs[26][0] = material_.Emissive.r; vs[26][1] = material_.Emissive.g;
    vs[26][2] = material_.Emissive.b; vs[26][3] = material_.Emissive.a;
    vs[27][0] = material_.Power;
    vs[27][1] = 0.0f; vs[27][2] = 0.0f; vs[27][3] = 0.0f;
  }

  if (dirty & kFFDirtyLights) {
    memset(&vs[28], 0, 40 * 4 * sizeof(float));
    uint32_t li = 0;
    for (DWORD i = 0; i < kMaxLights && li < 8; i++) {
      if (!light_enabled_[i]) continue;
      auto &light = lights_[i];
      uint32_t base = 28 + li * 5;

      float px, py, pz;
      TransformPointByMatrix(view, light.Position.x, light.Position.y, light.Position.z, px, py, pz);
      vs[base][0] = px; vs[base][1] = py; vs[base][2] = pz;
      vs[base][3] = (float)light.Type;

      float dx, dy, dz;
      TransformDirectionByMatrix(view, light.Direction.x, light.Direction.y, light.Direction.z, dx, dy, dz);
      float dlen = sqrtf(dx*dx + dy*dy + dz*dz);
      if (dlen > 0.0f) { dx /= dlen; dy /= dlen; dz /= dlen; }
      vs[base+1][0] = dx; vs[base+1][1] = dy; vs[base+1][2] = dz;
      vs[base+1][3] = light.Falloff;

      vs[base+2][0] = light.Diffuse.r;  vs[base+2][1] = light.Diffuse.g;
      vs[base+2][2] = light.Diffuse.b;  vs[base+2][3] = light.Diffuse.a;
      vs[base+3][0] = light.Ambient.r;  vs[base+3][1] = light.Ambient.g;
      vs[base+3][2] = light.Ambient.b;  vs[base+3][3] = light.Ambient.a;
      vs[base+4][0] = light.Attenuation0; vs[base+4][1] = light.Attenuation1;
      vs[base+4][2] = light.Attenuation2; vs[base+4][3] = light.Range;

      li++;
    }
  }

  if (dirty & kFFDirtyTexMat) {
    for (uint32_t i = 0; i < 8; i++) {
      auto &texMat = transforms_[TransformIndex((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i))];
      memcpy(&vs[68 + i * 4], &texMat, sizeof(D3DMATRIX));
    }
  }

  // PS constants
  if (dirty & (kFFDirtyPS | kFFDirtyFog)) {
    auto *ps = cached_ff_ps_;
    DWORD tf = render_states_[D3DRS_TEXTUREFACTOR];
    ps[0][0] = (float)((tf >> 16) & 0xFF) / 255.0f;
    ps[0][1] = (float)((tf >>  8) & 0xFF) / 255.0f;
    ps[0][2] = (float)((tf >>  0) & 0xFF) / 255.0f;
    ps[0][3] = (float)((tf >> 24) & 0xFF) / 255.0f;

    DWORD fc = render_states_[D3DRS_FOGCOLOR];
    ps[1][0] = (float)((fc >> 16) & 0xFF) / 255.0f;
    ps[1][1] = (float)((fc >>  8) & 0xFF) / 255.0f;
    ps[1][2] = (float)((fc >>  0) & 0xFF) / 255.0f;
    ps[1][3] = (float)(render_states_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;

    float fogStart, fogEnd, fogDensity;
    memcpy(&fogStart, &render_states_[D3DRS_FOGSTART], sizeof(float));
    memcpy(&fogEnd, &render_states_[D3DRS_FOGEND], sizeof(float));
    memcpy(&fogDensity, &render_states_[D3DRS_FOGDENSITY], sizeof(float));
    ps[2][0] = fogStart;
    ps[2][1] = fogEnd;
    ps[2][2] = fogDensity;
    ps[2][3] = 0.0f;
  }

  ff_dirty_ = 0;
}

// CreateRenderTarget
HRESULT STDMETHODCALLTYPE D3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
  Logger::err(str::format("TRACE: CreateRenderTarget ", Width, "x", Height, " fmt=", (int)Format));
  if (!ppSurface) return D3DERR_INVALIDCALL;

  auto mtlFormat = ConvertD3D9Format(Format);
  if (mtlFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("D3D9: CreateRenderTarget unsupported format ", (int)Format));
    return D3DERR_INVALIDCALL;
  }

  Logger::info(str::format("D3D9: CreateRenderTarget ", Width, "x", Height,
      " ", D3D9FormatName(Format), " msaa=", (int)MultiSample, " msaaQ=", MultisampleQuality,
      " lockable=", Lockable ? 1 : 0));

  WMTTextureInfo info = {};
  info.pixel_format = mtlFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = (WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  info.options = WMTResourceStorageModePrivate;

  auto texture = Rc(new Texture(info, dxmt_device_->device()));
  texture->rename(texture->allocate({}));

  auto viewKey = texture->createView({
      .format = mtlFormat,
      .type = WMTTextureType2DArray,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });

  *ppSurface = ref(new D3D9Surface(this, texture, viewKey, mtlFormat));
  return S_OK;
}

// StretchRect - basic blit copy
HRESULT STDMETHODCALLTYPE D3D9Device::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect,
    IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter) {
  InvalidateCurrentPass();
  if (!pSourceSurface || !pDestSurface) return D3DERR_INVALIDCALL;

  // Extract GPU textures from either D3D9Surface or D3D9TextureSurface
  auto getTexture = [](IDirect3DSurface9 *surf) -> Rc<Texture> {
    IDirect3DTexture9 *container = nullptr;
    if (SUCCEEDED(surf->GetContainer(__uuidof(IDirect3DTexture9), (void **)&container))) {
      auto *texSurf = static_cast<D3D9TextureSurface *>(surf);
      container->Release();
      return texSurf->texture();
    }
    return static_cast<D3D9Surface *>(surf)->texture();
  };

  auto srcTex = getTexture(pSourceSurface);
  auto dstTex = getTexture(pDestSurface);

  if (!srcTex || !dstTex) {
    Logger::warn("D3D9: StretchRect requires GPU-backed surfaces");
    return D3DERR_INVALIDCALL;
  }

  // Determine source/dest regions
  UINT sx = 0, sy = 0;
  UINT sw = srcTex->width();
  UINT sh = srcTex->height();
  UINT dx = 0, dy = 0;
  if (pSourceRect) {
    sx = pSourceRect->left; sy = pSourceRect->top;
    sw = pSourceRect->right - pSourceRect->left;
    sh = pSourceRect->bottom - pSourceRect->top;
  }
  if (pDestRect) {
    dx = pDestRect->left; dy = pDestRect->top;
    UINT dw = pDestRect->right - pDestRect->left;
    UINT dh = pDestRect->bottom - pDestRect->top;
    if (dw != sw || dh != sh) {
      static bool warned = false;
      if (!warned) {
        Logger::warn("D3D9: StretchRect scaling not implemented, using source size");
        warned = true;
      }
    }
  }

  auto &queue = dxmt_device_->queue();
  auto chunk = queue.CurrentChunk();

  Rc<TextureAllocation> srcAlloc(srcTex->current());
  Rc<TextureAllocation> dstAlloc(dstTex->current());

  chunk->emitcc([
    srcTex = std::move(srcTex), srcAlloc = std::move(srcAlloc),
    dstTex = std::move(dstTex), dstAlloc = std::move(dstAlloc),
    sx, sy, sw, sh, dx, dy
  ](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();

    auto srcHandle = ctx.access(srcAlloc.ptr(), DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto dstHandle = ctx.access(dstAlloc.ptr(), DXMT_ENCODER_RESOURCE_ACESS_WRITE);

    auto &blitCmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
    blitCmd.type = WMTBlitCommandCopyFromTextureToTexture;
    blitCmd.src = srcHandle;
    blitCmd.src_slice = 0;
    blitCmd.src_level = 0;
    blitCmd.src_origin = {sx, sy, 0};
    blitCmd.src_size = {sw, sh, 1};
    blitCmd.dst = dstHandle;
    blitCmd.dst_slice = 0;
    blitCmd.dst_level = 0;
    blitCmd.dst_origin = {dx, dy, 0};

    ctx.endPass();
  });

  return S_OK;
}

// UpdateTexture - copy mip data from src to dst texture
HRESULT STDMETHODCALLTYPE D3D9Device::UpdateTexture(
    IDirect3DBaseTexture9 *pSourceTexture,
    IDirect3DBaseTexture9 *pDestinationTexture) {
  if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;

  // Both must be 2D textures
  IDirect3DTexture9 *srcTex2D = nullptr;
  IDirect3DTexture9 *dstTex2D = nullptr;
  if (FAILED(pSourceTexture->QueryInterface(__uuidof(IDirect3DTexture9), (void **)&srcTex2D)) ||
      FAILED(pDestinationTexture->QueryInterface(__uuidof(IDirect3DTexture9), (void **)&dstTex2D))) {
    Logger::warn("D3D9: UpdateTexture only supports 2D textures");
    if (srcTex2D) srcTex2D->Release();
    if (dstTex2D) dstTex2D->Release();
    return D3DERR_INVALIDCALL;
  }

  auto *src = static_cast<D3D9Texture2D *>(srcTex2D);
  auto *dst = static_cast<D3D9Texture2D *>(dstTex2D);

  // Copy mip data from source staging to dest staging, then mark dest dirty
  UINT levels = std::min(src->levelCount(), dst->levelCount());
  for (UINT i = 0; i < levels; i++) {
    D3DLOCKED_RECT srcLR, dstLR;
    src->LockRect(i, &srcLR, nullptr, D3DLOCK_READONLY);
    dst->LockRect(i, &dstLR, nullptr, 0);

    D3DSURFACE_DESC desc;
    src->GetLevelDesc(i, &desc);
    UINT rowSize = D3D9FormatPitch(src->format(), desc.Width);
    UINT rows = IsCompressedFormat(src->format()) ? (desc.Height + 3) / 4 : desc.Height;

    for (UINT row = 0; row < rows; row++) {
      memcpy((uint8_t *)dstLR.pBits + row * dstLR.Pitch,
             (const uint8_t *)srcLR.pBits + row * srcLR.Pitch,
             rowSize);
    }

    src->UnlockRect(i);
    dst->UnlockRect(i);
  }

  srcTex2D->Release();
  dstTex2D->Release();
  return S_OK;
}

// GetVertexShaderConstantF
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vsConstants_[StartRegister], Vector4fCount * 4 * sizeof(float));
  return S_OK;
}

// GetPixelShaderConstantF
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantF(
    UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > 256) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &psConstants_[StartRegister], Vector4fCount * 4 * sizeof(float));
  return S_OK;
}

// GetStreamSource
HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData,
    UINT *pOffsetInBytes, UINT *pStride) {
  if (StreamNumber >= 16 || !ppStreamData) return D3DERR_INVALIDCALL;
  *ppStreamData = stream_sources_[StreamNumber].ref();
  if (pOffsetInBytes) *pOffsetInBytes = stream_offsets_[StreamNumber];
  if (pStride) *pStride = stream_strides_[StreamNumber];
  return S_OK;
}

// State blocks
HRESULT STDMETHODCALLTYPE D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  Logger::err(str::format("TRACE: CreateStateBlock type=", (int)Type));
  if (!ppSB) return D3DERR_INVALIDCALL;

  Logger::debug(str::format("D3D9: CreateStateBlock type=", (int)Type));

  auto *sb = new D3D9StateBlock(this, Type);

  // Capture current state based on block type
  bool capturePixel = (Type == D3DSBT_ALL || Type == D3DSBT_PIXELSTATE);
  bool captureVertex = (Type == D3DSBT_ALL || Type == D3DSBT_VERTEXSTATE);

  memcpy(sb->render_states, render_states_, sizeof(render_states_));

  if (capturePixel) {
    memcpy(sb->texture_stage_states, texture_stage_states_, sizeof(texture_stage_states_));
    memcpy(sb->sampler_states, sampler_states_, sizeof(sampler_states_));
    memcpy(sb->ps_constants, psConstants_, sizeof(psConstants_));
    sb->ps = current_ps_;
    for (int i = 0; i < 16; i++)
      sb->textures[i] = bound_textures_[i];
  }

  if (captureVertex) {
    memcpy(sb->transforms, transforms_, sizeof(transforms_));
    sb->viewport = viewport_;
    sb->scissor_rect = scissor_rect_;
    sb->material = material_;
    memcpy(sb->lights, lights_, sizeof(lights_));
    memcpy(sb->light_enabled, light_enabled_, sizeof(light_enabled_));
    sb->fvf = current_fvf_;
    memcpy(sb->vs_constants, vsConstants_, sizeof(vsConstants_));
    sb->vs = current_vs_;
    sb->vdecl = current_vdecl_;
    for (int i = 0; i < 16; i++) {
      sb->stream_sources[i] = stream_sources_[i];
      sb->stream_offsets[i] = stream_offsets_[i];
      sb->stream_strides[i] = stream_strides_[i];
    }
    sb->ib = current_ib_;
  }

  *ppSB = ref(sb);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::BeginStateBlock() {
  recording_state_block_ = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  if (!ppSB) return D3DERR_INVALIDCALL;
  recording_state_block_ = false;

  // Create a state block capturing current state (same as CreateStateBlock ALL)
  return CreateStateBlock(D3DSBT_ALL, ppSB);
}

// D3D9StateBlock::Capture
HRESULT STDMETHODCALLTYPE D3D9StateBlock::Capture() {
  bool capturePixel = (type_ == D3DSBT_ALL || type_ == D3DSBT_PIXELSTATE);
  bool captureVertex = (type_ == D3DSBT_ALL || type_ == D3DSBT_VERTEXSTATE);

  memcpy(render_states, device_->render_states_, sizeof(render_states));

  if (capturePixel) {
    memcpy(texture_stage_states, device_->texture_stage_states_, sizeof(texture_stage_states));
    memcpy(sampler_states, device_->sampler_states_, sizeof(sampler_states));
    memcpy(ps_constants, device_->psConstants_, sizeof(ps_constants));
    ps = device_->current_ps_;
    for (int i = 0; i < 16; i++)
      textures[i] = device_->bound_textures_[i];
  }

  if (captureVertex) {
    memcpy(transforms, device_->transforms_, sizeof(transforms));
    viewport = device_->viewport_;
    scissor_rect = device_->scissor_rect_;
    material = device_->material_;
    memcpy(lights, device_->lights_, sizeof(lights));
    memcpy(light_enabled, device_->light_enabled_, sizeof(light_enabled));
    fvf = device_->current_fvf_;
    memcpy(vs_constants, device_->vsConstants_, sizeof(vs_constants));
    vs = device_->current_vs_;
    vdecl = device_->current_vdecl_;
    for (int i = 0; i < 16; i++) {
      stream_sources[i] = device_->stream_sources_[i];
      stream_offsets[i] = device_->stream_offsets_[i];
      stream_strides[i] = device_->stream_strides_[i];
    }
    ib = device_->current_ib_;
  }

  return S_OK;
}

// D3D9StateBlock::Apply
HRESULT STDMETHODCALLTYPE D3D9StateBlock::Apply() {
  bool applyPixel = (type_ == D3DSBT_ALL || type_ == D3DSBT_PIXELSTATE);
  bool applyVertex = (type_ == D3DSBT_ALL || type_ == D3DSBT_VERTEXSTATE);

  memcpy(device_->render_states_, render_states, sizeof(render_states));
  device_->pso_dirty_ = true;
  device_->dsso_dirty_ = true;

  if (applyPixel) {
    memcpy(device_->texture_stage_states_, texture_stage_states, sizeof(texture_stage_states));
    memcpy(device_->sampler_states_, sampler_states, sizeof(sampler_states));
    memcpy(device_->psConstants_, ps_constants, sizeof(ps_constants));
    device_->current_ps_ = ps;
    for (int i = 0; i < 16; i++)
      device_->bound_textures_[i] = textures[i];
    device_->tex_dirty_ = true;
    device_->ps_const_version_++;
  }

  if (applyVertex) {
    memcpy(device_->transforms_, transforms, sizeof(transforms));
    device_->viewport_ = viewport;
    device_->scissor_rect_ = scissor_rect;
    device_->material_ = material;
    memcpy(device_->lights_, lights, sizeof(lights));
    memcpy(device_->light_enabled_, light_enabled, sizeof(light_enabled));
    if (fvf) device_->SetFVF(fvf);
    memcpy(device_->vsConstants_, vs_constants, sizeof(vs_constants));
    device_->current_vs_ = vs;
    device_->current_vdecl_ = vdecl;
    for (int i = 0; i < 16; i++) {
      device_->stream_sources_[i] = stream_sources[i];
      device_->stream_offsets_[i] = stream_offsets[i];
      device_->stream_strides_[i] = stream_strides[i];
    }
    device_->current_ib_ = ib;
    device_->vb_dirty_ = true;
    device_->vp_dirty_ = true;
    device_->scissor_dirty_ = true;
    device_->vs_const_version_++;
    device_->ff_const_version_++;
    device_->ff_dirty_ |= D3D9Device::kFFDirtyAll;
  }

  return S_OK;
}

} // namespace dxmt
