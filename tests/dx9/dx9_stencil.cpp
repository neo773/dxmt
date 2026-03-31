#include "dx9_test_utils.h"
#include <d3d9.h>

// Test stencil operations:
// Pass 1: draw quad writing stencil ref=1 with REPLACE
// Pass 2: draw second quad testing stencil ref=1 func=EQUAL in different color
// Verify: second quad only renders where first quad wrote stencil

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
    winClass.lpszClassName = L"D3D9StencilTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Stencil Test (DXMT)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top,
                                0, 0, hInstance, 0);
    if (!hwnd) { fprintf(stderr, "FATAL: CreateWindow failed\n"); return 1; }

    IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1024;
    pp.BackBufferHeight = 768;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08x\n", (unsigned)hr); return 1; }

    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z; DWORD color; };

    // Left-half quad (red) — will write stencil
    Vertex leftQuad[] = {
        { -1.0f,  1.0f, 0.5f, 0xFFFF0000 },
        {  0.0f,  1.0f, 0.5f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000 },
        {  0.0f,  1.0f, 0.5f, 0xFFFF0000 },
        {  0.0f, -1.0f, 0.5f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000 },
    };

    // Fullscreen quad (green) — stencil tested, should only appear on left half
    Vertex fullQuad[] = {
        { -1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        { -1.0f, -1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f, -1.0f, 0.5f, 0xFF00FF00 },
        { -1.0f, -1.0f, 0.5f, 0xFF00FF00 },
    };

    IDirect3DVertexBuffer9 *vbLeft = nullptr, *vbFull = nullptr;
    device->CreateVertexBuffer(sizeof(leftQuad), 0, fvf, D3DPOOL_DEFAULT, &vbLeft, nullptr);
    void *data;
    vbLeft->Lock(0, sizeof(leftQuad), &data, 0); memcpy(data, leftQuad, sizeof(leftQuad)); vbLeft->Unlock();

    device->CreateVertexBuffer(sizeof(fullQuad), 0, fvf, D3DPOOL_DEFAULT, &vbFull, nullptr);
    vbFull->Lock(0, sizeof(fullQuad), &data, 0); memcpy(data, fullQuad, sizeof(fullQuad)); vbFull->Unlock();

    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    // Clear color to blue, depth+stencil to defaults
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF0000FF, 1.0f, 0);
    device->BeginScene();
    device->SetFVF(fvf);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);

    // Pass 1: Draw left quad, writing stencil ref=1
    device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILREF, 1);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    device->SetRenderState(D3DRS_STENCILMASK, 0xFF);
    device->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);

    device->SetStreamSource(0, vbLeft, 0, sizeof(Vertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

    // Pass 2: Draw fullscreen quad, only where stencil == 1 (left half)
    device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);

    device->SetStreamSource(0, vbFull, 0, sizeof(Vertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->EndScene();

    // Readback
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    IDirect3DSurface9 *offscreen = nullptr;
    device->CreateOffscreenPlainSurface(1024, 768, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT lr;
    offscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY);

    auto getPixel = [&](int x, int y) -> uint32_t {
        return ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x];
    };

    // Left center (256, 384) should be green (pass 2 drew over pass 1's red)
    uint32_t leftPix = getPixel(256, 384);
    uint8_t lR = (leftPix >> 16) & 0xFF;
    uint8_t lG = (leftPix >> 8) & 0xFF;

    // Right center (768, 384) should be blue (stencil test failed, no pass 2 draw)
    uint32_t rightPix = getPixel(768, 384);
    uint8_t rB = rightPix & 0xFF;
    uint8_t rR = (rightPix >> 16) & 0xFF;
    uint8_t rG = (rightPix >> 8) & 0xFF;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_stencil.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, 1024, 768, lr.Pitch);
    offscreen->UnlockRect();

    // Left should be green (high G, low R)
    bool leftGreen = lG > 200 && lR < 50;
    // Right should be blue (high B, low R and G)
    bool rightBlue = rB > 200 && rR < 50 && rG < 50;

    fprintf(stderr, "TEST stencil_left_green: %s (0x%08x)\n", leftGreen ? "PASS" : "FAIL", leftPix);
    fprintf(stderr, "TEST stencil_right_blue: %s (0x%08x)\n", rightBlue ? "PASS" : "FAIL", rightPix);

    bool allPass = leftGreen && rightBlue;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    vbLeft->Release(); vbFull->Release();
    backbuffer->Release(); offscreen->Release();
    device->Release(); d3d9->Release();
    return allPass ? 0 : 1;
}
