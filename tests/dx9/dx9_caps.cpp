#include "dx9_test_utils.h"
#include <d3d9.h>

// Test caps advertising and CheckDeviceFormat validation
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    [[maybe_unused]] bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));

    IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { fprintf(stderr, "FATAL: Direct3DCreate9 failed\n"); return 1; }

    // Test 1: GetDeviceCaps should NOT advertise cubemap
    D3DCAPS9 caps = {};
    HRESULT hr = d3d9->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps);
    bool capsOk = SUCCEEDED(hr);
    bool noCubemap = !(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP);
    fprintf(stderr, "TEST caps_no_cubemap: %s (TextureCaps=0x%08x)\n",
            noCubemap ? "PASS" : "FAIL", (unsigned)caps.TextureCaps);

    // Test 2: CheckDeviceFormat with supported format → S_OK
    hr = d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                  D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    bool supportedOk = (hr == S_OK);
    fprintf(stderr, "TEST cdf_supported: %s (hr=0x%08x)\n", supportedOk ? "PASS" : "FAIL", (unsigned)hr);

    // Test 3: CheckDeviceFormat with A8L8 → S_OK (we now support it)
    hr = d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                  D3DRTYPE_TEXTURE, D3DFMT_A8L8);
    bool a8l8Ok = (hr == S_OK);
    fprintf(stderr, "TEST cdf_a8l8: %s (hr=0x%08x)\n", a8l8Ok ? "PASS" : "FAIL", (unsigned)hr);

    // Test 4: CheckDeviceFormat with unsupported format → D3DERR_NOTAVAILABLE
    // D3DFMT_Q8W8V8U8 (63) is not mapped in our format table
    hr = d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                  D3DRTYPE_TEXTURE, D3DFMT_Q8W8V8U8);
    bool unsupportedRejected = (hr == D3DERR_NOTAVAILABLE);
    fprintf(stderr, "TEST cdf_unsupported: %s (hr=0x%08x)\n", unsupportedRejected ? "PASS" : "FAIL", (unsigned)hr);

    // Test 5: CheckDeviceFormat with cubemap → D3DERR_NOTAVAILABLE
    hr = d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                  D3DRTYPE_CUBETEXTURE, D3DFMT_A8R8G8B8);
    bool cubeRejected = (hr == D3DERR_NOTAVAILABLE);
    fprintf(stderr, "TEST cdf_cube_rejected: %s (hr=0x%08x)\n", cubeRejected ? "PASS" : "FAIL", (unsigned)hr);

    // Test 6: CheckDeviceFormat for depth stencil → S_OK
    hr = d3d9->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                  D3DRTYPE_SURFACE, D3DFMT_D24S8);
    bool depthOk = (hr == S_OK);
    fprintf(stderr, "TEST cdf_depth: %s (hr=0x%08x)\n", depthOk ? "PASS" : "FAIL", (unsigned)hr);

    bool allPass = capsOk && noCubemap && supportedOk && a8l8Ok && unsupportedRejected && cubeRejected && depthOk;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    d3d9->Release();
    return allPass ? 0 : 1;
}
