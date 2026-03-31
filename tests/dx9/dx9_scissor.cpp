#include "dx9_test_utils.h"
#include <d3d9.h>

// Test scissor rect: enable scissor, draw fullscreen quad, verify clipping
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
    winClass.lpszClassName = L"D3D9ScissorTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Scissor Test (DXMT)",
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

    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed\n"); return 1; }

    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z; DWORD color; };
    Vertex redVerts[] = {
        { -1.0f,  1.0f, 0.5f, 0xFFFF0000 },
        {  1.0f,  1.0f, 0.5f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000 },
        {  1.0f,  1.0f, 0.5f, 0xFFFF0000 },
        {  1.0f, -1.0f, 0.5f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000 },
    };
    Vertex greenVerts[] = {
        { -1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        { -1.0f, -1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f,  1.0f, 0.5f, 0xFF00FF00 },
        {  1.0f, -1.0f, 0.5f, 0xFF00FF00 },
        { -1.0f, -1.0f, 0.5f, 0xFF00FF00 },
    };

    IDirect3DVertexBuffer9 *vbRed = nullptr;
    device->CreateVertexBuffer(sizeof(redVerts), 0, fvf, D3DPOOL_DEFAULT, &vbRed, nullptr);
    void *data; vbRed->Lock(0, sizeof(redVerts), &data, 0);
    memcpy(data, redVerts, sizeof(redVerts)); vbRed->Unlock();

    IDirect3DVertexBuffer9 *vbGreen = nullptr;
    device->CreateVertexBuffer(sizeof(greenVerts), 0, fvf, D3DPOOL_DEFAULT, &vbGreen, nullptr);
    vbGreen->Lock(0, sizeof(greenVerts), &data, 0);
    memcpy(data, greenVerts, sizeof(greenVerts)); vbGreen->Unlock();

    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    // Clear to blue, then draw red fullscreen quad with scissor clipping to center 512x384
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
    device->BeginScene();
    device->SetFVF(fvf);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);

    // Pass 1: Draw red fullscreen quad with scissor clipping to center 512x384
    device->SetStreamSource(0, vbRed, 0, sizeof(Vertex));
    RECT scissor = { 256, 192, 768, 576 };
    device->SetScissorRect(&scissor);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

    // Pass 2: Disable scissor, draw green fullscreen quad — should cover entire screen
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetStreamSource(0, vbGreen, 0, sizeof(Vertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

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

    // After pass 2, the green quad should cover everything (scissor was disabled)
    // Center (512, 384) — green (was red from pass 1, overdrawn green in pass 2)
    uint32_t centerPix = getPixel(512, 384);
    uint8_t cR = (centerPix >> 16) & 0xFF;
    uint8_t cG = (centerPix >> 8) & 0xFF;

    // Corner (10, 10) — green (was blue from clear, overdrawn green in pass 2)
    // If scissor disable leaks, this stays blue because green is clipped to old scissor
    uint32_t cornerPix = getPixel(10, 10);
    uint8_t oR = (cornerPix >> 16) & 0xFF;
    uint8_t oG = (cornerPix >> 8) & 0xFF;
    uint8_t oB = cornerPix & 0xFF;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_scissor.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, 1024, 768, lr.Pitch);
    offscreen->UnlockRect();

    bool centerGreen = cG > 200 && cR < 50;
    bool cornerGreen = oG > 200 && oR < 50 && oB < 50;

    fprintf(stderr, "TEST scissor_center_green: %s (0x%08x)\n", centerGreen ? "PASS" : "FAIL", centerPix);
    fprintf(stderr, "TEST scissor_disable_corner_green: %s (0x%08x)\n", cornerGreen ? "PASS" : "FAIL", cornerPix);

    bool allPass = centerGreen && cornerGreen;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    vbRed->Release(); vbGreen->Release();
    backbuffer->Release(); offscreen->Release();
    device->Release(); d3d9->Release();
    return allPass ? 0 : 1;
}
