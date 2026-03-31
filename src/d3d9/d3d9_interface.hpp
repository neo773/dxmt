#pragma once

#include "com/com_object.hpp"
#include <d3d9.h>

namespace dxmt {

class D3D9Interface final : public ComObjectWithInitialRef<IDirect3D9> {

public:
  D3D9Interface();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) final;

  // IDirect3D9
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) final;
  UINT STDMETHODCALLTYPE GetAdapterCount() final;
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                  D3DADAPTER_IDENTIFIER9 *pIdentifier) final;
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) final;
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                              D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType,
                                             D3DFORMAT AdapterFormat,
                                             D3DFORMAT BackBufferFormat, BOOL bWindowed) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                               D3DFORMAT AdapterFormat, DWORD Usage,
                                               D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                        D3DFORMAT SurfaceFormat, BOOL Windowed,
                                                        D3DMULTISAMPLE_TYPE MultiSampleType,
                                                        DWORD *pQualityLevels) final;
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                                    D3DFORMAT AdapterFormat,
                                                    D3DFORMAT RenderTargetFormat,
                                                    D3DFORMAT DepthStencilFormat) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
                                                         D3DFORMAT SourceFormat,
                                                         D3DFORMAT TargetFormat) final;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) final;
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) final;
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                          DWORD BehaviorFlags,
                                          D3DPRESENT_PARAMETERS *pPresentationParameters,
                                          IDirect3DDevice9 **ppReturnedDeviceInterface) final;
};

} // namespace dxmt
