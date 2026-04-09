
#include "d3d9_interface.hpp"
#include "log/log.hpp"

namespace dxmt {
Logger Logger::s_instance("d3d9.log");

static void PatchRegistryVRAMImpl() {
  Logger::info("PatchRegistryVRAM: starting");

  HKEY hKey;
  const char *basePath = "SYSTEM\\CurrentControlSet\\Control\\Video";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, basePath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
    Logger::warn("PatchRegistryVRAM: failed to open Video registry key");
    return;
  }

  char subKeyName[256];
  for (DWORD i = 0; RegEnumKeyA(hKey, i, subKeyName, sizeof(subKeyName)) == ERROR_SUCCESS; i++) {
    char devPath[512];
    snprintf(devPath, sizeof(devPath), "%s\\%s\\0000", basePath, subKeyName);
    HKEY hDevKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, devPath, 0, KEY_READ | KEY_WRITE, &hDevKey) == ERROR_SUCCESS) {
      DWORD memSize = 0;
      DWORD memSizeLen = sizeof(memSize);
      DWORD type = 0;
      RegQueryValueExA(hDevKey, "HardwareInformation.MemorySize", nullptr, &type, (BYTE *)&memSize, &memSizeLen);
      Logger::info(str::format("PatchRegistryVRAM: ", devPath, " MemorySize=", memSize));
      if (memSize == 0) {
        DWORD newSize = 128 * 1024 * 1024;
        RegSetValueExA(hDevKey, "HardwareInformation.MemorySize", 0, REG_DWORD, (const BYTE *)&newSize, sizeof(newSize));
        UINT64 newSize64 = newSize;
        RegSetValueExA(hDevKey, "HardwareInformation.qwMemorySize", 0, REG_QWORD, (const BYTE *)&newSize64, sizeof(newSize64));
        Logger::info("PatchRegistryVRAM: patched to 128MB");
      }
      RegCloseKey(hDevKey);
    }
  }
  RegCloseKey(hKey);
}

static FILE *dxmt_rawlog() {
  static FILE *f = nullptr;
  if (!f) { f = fopen("C:\\dxmt_raw.log", "w"); }
  return f;
}
static void rawlog(const char *msg) {
  FILE *f = dxmt_rawlog();
  if (f) { fputs(msg, f); fflush(f); }
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
  rawlog("Direct3DCreate9 called\n");
  Logger::info("Direct3DCreate9 called");
  PatchRegistryVRAMImpl();
  auto *iface = new D3D9Interface();
  rawlog("Direct3DCreate9 returning\n");
  return iface;
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
  if (reason == DLL_PROCESS_ATTACH) {
    // Write marker using OutputDebugString which Wine captures
    OutputDebugStringA("DXMT d3d9.dll DLL_PROCESS_ATTACH\n");
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

#endif
