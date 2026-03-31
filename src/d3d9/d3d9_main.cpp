
#include "d3d9_interface.hpp"
#include "log/log.hpp"

namespace dxmt {
Logger Logger::s_instance("d3d9.log");

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
  Logger::info("Direct3DCreate9 called");
  return new D3D9Interface();
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_BeginEvent not supported (profiling API stubbed)"); w = true; }
  return 0;
}

extern "C" int WINAPI D3DPERF_EndEvent(void) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_EndEvent not supported (profiling API stubbed)"); w = true; }
  return 0;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus(void) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_GetStatus not supported (profiling API stubbed)"); w = true; }
  return 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_QueryRepeatFrame not supported (profiling API stubbed)"); w = true; }
  return FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_SetMarker not supported (profiling API stubbed)"); w = true; }
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_SetOptions not supported (profiling API stubbed)"); w = true; }
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
  static bool w = false;
  if (!w) { Logger::info("D3D9: D3DPERF_SetRegion not supported (profiling API stubbed)"); w = true; }
}

} // namespace dxmt

#ifndef DXMT_NATIVE

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

#endif
