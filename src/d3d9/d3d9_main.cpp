
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
  if (!f) { f = fopen("C:\\dxmt_raw.log", "a"); }
  return f;
}
static void rawlog(const char *msg) {
  FILE *f = dxmt_rawlog();
  if (f) { fputs(msg, f); fflush(f); }
}

// Global singleton — games may call Direct3DCreate9 multiple times (e.g. Oblivion's wrapper
// creates+releases a temp IDirect3D9). The singleton survives Release via clamped refcount.
static IDirect3D9 *g_d3d9_singleton = nullptr;

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) {
  void *caller = __builtin_return_address(0);
  Logger::info(str::format("Direct3DCreate9(sdk=", SDKVersion, ") called from ", caller));
  PatchRegistryVRAMImpl();
  if (!g_d3d9_singleton) {
    g_d3d9_singleton = new D3D9Interface();
  }
  // AddRef so each caller gets their own reference
  g_d3d9_singleton->AddRef();

  // Oblivion fix: the game reads a global IDirect3D9* at 0xb42154 before its init
  // function runs. Write it here so early code doesn't crash on NULL.
  // We also patch the init function to not skip when [0xb42154] is already set,
  // so the full renderer setup still happens later.
  *(IDirect3D9 *volatile *)0xb42154 = g_d3d9_singleton;

  Logger::info(str::format("Direct3DCreate9 returning ptr=", (void*)g_d3d9_singleton));
  return g_d3d9_singleton;
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
    OutputDebugStringA("DXMT d3d9.dll DLL_PROCESS_ATTACH\n");
    DisableThreadLibraryCalls(instance);

    // Oblivion fix #1: patch init at 0x761df0 to not skip when [0xb42154] is set.
    // On Wine, the wrapper path sets [0xb42154] before init runs.
    {
      uint8_t *p = (uint8_t *)0x761df3;
      DWORD oldProt;
      if (VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
        if (p[0] == 0x39 && p[1] == 0x35 && p[4] == 0xb4) {
          p[0] = 0x39; p[1] = 0xf6; // cmp esi,esi (always equal → jne never taken)
          p[2] = 0x90; p[3] = 0x90; p[4] = 0x90; p[5] = 0x90;
          p[6] = 0xeb; p[7] = 0x00;
          OutputDebugStringA("DXMT: Patched Oblivion init skip\n");
        }
        VirtualProtect(p, 8, oldProt, &oldProt);
      }
    }

    // Oblivion fix #2: patch 0x497E10 to handle NULL renderer at [0xb350d8].
    // Original: 8b 89 80 08 00 00 (mov 0x880(%ecx),%ecx)
    // Crashes when ecx=0. We trampoline to a NULL check.
    {
      // Allocate executable trampoline near the game's code
      uint8_t *tramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (tramp) {
        // Build trampoline with explicit offsets
        // [0]  test ecx, ecx
        // [2]  jz +11 → offset 15 (ret_default)
        // [4]  mov 0x880(%ecx),%ecx  (original instruction)
        // [10] jmp back to 0x497E16
        // [15] mov eax, 0x14  (ret_default)
        // [20] pop esi
        // [21] ret
        tramp[0] = 0x85; tramp[1] = 0xc9;         // test ecx, ecx
        tramp[2] = 0x74; tramp[3] = 0x0b;         // jz +11 → offset 15
        tramp[4] = 0x8b; tramp[5] = 0x89;         // mov 0x880(%ecx),%ecx
        tramp[6] = 0x80; tramp[7] = 0x08; tramp[8] = 0x00; tramp[9] = 0x00;
        tramp[10] = 0xe9;                          // jmp rel32
        {
          int32_t rel = (int32_t)((intptr_t)0x497E16 - (intptr_t)(tramp + 15));
          memcpy(tramp + 11, &rel, 4);
        }
        tramp[15] = 0xb8;                          // mov eax, 0x14
        tramp[16] = 0x14; tramp[17] = 0x00; tramp[18] = 0x00; tramp[19] = 0x00;
        tramp[20] = 0x5e;                          // pop esi
        tramp[21] = 0xc3;                          // ret

        // Patch 0x497E10: replace 6-byte mov with 5-byte jmp + 1 nop
        uint8_t *p = (uint8_t *)0x497E10;
        DWORD oldProt;
        if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
          if (p[0] == 0x8b && p[1] == 0x89) { // verify it's the right instruction
            p[0] = 0xe9; // jmp rel32
            int32_t jrel = (int32_t)(tramp - (p + 5));
            memcpy(p + 1, &jrel, 4);
            p[5] = 0x90; // nop
            OutputDebugStringA("DXMT: Patched Oblivion renderer null check\n");
          }
          VirtualProtect(p, 6, oldProt, &oldProt);
        }
      }
    }
  }
  return TRUE;
}

#endif
