#include "dx9_test_utils.h"
#include <d3d9.h>
#include <cmath>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    [[maybe_unused]] bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));
    WNDCLASSEXW winClass = {};
    winClass.cbSize = sizeof(WNDCLASSEXW);
    winClass.style = CS_HREDRAW | CS_VREDRAW;
    winClass.lpfnWndProc = &WndProc;
    winClass.hInstance = hInstance;
    winClass.hIcon = LoadIconW(0, IDI_APPLICATION);
    winClass.hCursor = LoadCursorW(0, IDC_ARROW);
    winClass.lpszClassName = L"D3D9StateWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 256, 256 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 State Storage Test (DXMT)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top,
                                0, 0, hInstance, 0);
    if (!hwnd) { fprintf(stderr, "FATAL: CreateWindow failed\n"); return 1; }

    IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { fprintf(stderr, "FATAL: Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &pp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08x\n", (unsigned)hr); return 1; }

    bool pass = true;

    // Test 1: Transform set/get roundtrip
    {
        D3DMATRIX mat = {};
        mat._11 = 1.0f; mat._12 = 2.0f; mat._13 = 3.0f; mat._14 = 4.0f;
        mat._21 = 5.0f; mat._22 = 6.0f; mat._23 = 7.0f; mat._24 = 8.0f;
        mat._31 = 9.0f; mat._32 = 10.0f; mat._33 = 11.0f; mat._34 = 12.0f;
        mat._41 = 13.0f; mat._42 = 14.0f; mat._43 = 15.0f; mat._44 = 16.0f;

        device->SetTransform(D3DTS_VIEW, &mat);

        D3DMATRIX got = {};
        device->GetTransform(D3DTS_VIEW, &got);

        bool match = (got._11 == 1.0f && got._12 == 2.0f && got._44 == 16.0f);
        fprintf(stderr, "TEST transform_roundtrip: %s\n", match ? "PASS" : "FAIL");
        if (!match) pass = false;
    }

    // Test 2: MultiplyTransform
    {
        D3DMATRIX identity = {};
        identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;

        D3DMATRIX scale = {};
        scale._11 = 2.0f; scale._22 = 3.0f; scale._33 = 4.0f; scale._44 = 1.0f;

        device->SetTransform(D3DTS_WORLD, &identity);
        device->MultiplyTransform(D3DTS_WORLD, &scale);

        D3DMATRIX got = {};
        device->GetTransform(D3DTS_WORLD, &got);

        bool match = (got._11 == 2.0f && got._22 == 3.0f && got._33 == 4.0f && got._44 == 1.0f);
        fprintf(stderr, "TEST multiply_transform: %s (_11=%f _22=%f _33=%f)\n",
                match ? "PASS" : "FAIL", got._11, got._22, got._33);
        if (!match) pass = false;
    }

    // Test 3: Texture stage state set/get roundtrip
    {
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);

        DWORD val = 0;
        device->GetTextureStageState(0, D3DTSS_COLOROP, &val);
        bool ok1 = (val == D3DTOP_ADD);

        device->GetTextureStageState(0, D3DTSS_COLORARG1, &val);
        bool ok2 = (val == D3DTA_DIFFUSE);

        device->GetTextureStageState(1, D3DTSS_ALPHAOP, &val);
        bool ok3 = (val == D3DTOP_MODULATE2X);

        bool match = ok1 && ok2 && ok3;
        fprintf(stderr, "TEST tss_roundtrip: %s\n", match ? "PASS" : "FAIL");
        if (!match) pass = false;
    }

    // Test 4: Material set/get roundtrip
    {
        D3DMATERIAL9 mat = {};
        mat.Diffuse = {0.1f, 0.2f, 0.3f, 0.4f};
        mat.Ambient = {0.5f, 0.6f, 0.7f, 0.8f};
        mat.Power = 42.0f;
        device->SetMaterial(&mat);

        D3DMATERIAL9 got = {};
        device->GetMaterial(&got);

        bool match = (fabsf(got.Diffuse.r - 0.1f) < 0.001f &&
                      fabsf(got.Ambient.b - 0.7f) < 0.001f &&
                      fabsf(got.Power - 42.0f) < 0.001f);
        fprintf(stderr, "TEST material_roundtrip: %s\n", match ? "PASS" : "FAIL");
        if (!match) pass = false;
    }

    // Test 5: Light set/get/enable roundtrip
    {
        D3DLIGHT9 light = {};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Direction.x = 0.0f;
        light.Direction.y = -1.0f;
        light.Direction.z = 0.0f;
        light.Diffuse = {1.0f, 0.5f, 0.0f, 1.0f};
        device->SetLight(0, &light);
        device->LightEnable(0, TRUE);

        D3DLIGHT9 got = {};
        device->GetLight(0, &got);
        BOOL enabled = FALSE;
        device->GetLightEnable(0, &enabled);

        bool match = (got.Type == D3DLIGHT_DIRECTIONAL &&
                      fabsf(got.Direction.y + 1.0f) < 0.001f &&
                      enabled == TRUE);
        fprintf(stderr, "TEST light_roundtrip: %s\n", match ? "PASS" : "FAIL");
        if (!match) pass = false;
    }

    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    device->Release();
    d3d9->Release();
    return pass ? 0 : 1;
}
