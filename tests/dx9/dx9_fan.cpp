#include "dx9_test_utils.h"
#include <d3d9.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));
    WNDCLASSEXW winClass = {};
    winClass.cbSize = sizeof(WNDCLASSEXW);
    winClass.style = CS_HREDRAW | CS_VREDRAW;
    winClass.lpfnWndProc = &WndProc;
    winClass.hInstance = hInstance;
    winClass.hIcon = LoadIconW(0, IDI_APPLICATION);
    winClass.hCursor = LoadCursorW(0, IDC_ARROW);
    winClass.lpszClassName = L"D3D9FanTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Triangle Fan Test (DXMT)",
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

    IDirect3DSurface9 *bbQuery = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbQuery);
    D3DSURFACE_DESC bbDesc;
    bbQuery->GetDesc(&bbDesc);
    const UINT WIDTH = bbDesc.Width;
    const UINT HEIGHT = bbDesc.Height;
    bbQuery->Release();

    // Pentagon as a triangle fan: center + 5 outer vertices
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z; DWORD color; };

    // Fan center + 4 outer points = 4 triangles (a quad-ish shape)
    Vertex verts[] = {
        {  0.0f,  0.0f, 0.5f, 0xFFFF0000 }, // center (red)
        { -0.8f,  0.8f, 0.5f, 0xFF00FF00 }, // top-left (green)
        {  0.8f,  0.8f, 0.5f, 0xFF0000FF }, // top-right (blue)
        {  0.8f, -0.8f, 0.5f, 0xFFFFFF00 }, // bottom-right (yellow)
        { -0.8f, -0.8f, 0.5f, 0xFFFF00FF }, // bottom-left (magenta)
    };

    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(sizeof(verts), 0, fvf, D3DPOOL_DEFAULT, &vb, nullptr);
    void *vbData; vb->Lock(0, sizeof(verts), &vbData, 0);
    memcpy(vbData, verts, sizeof(verts));
    vb->Unlock();

    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();

        device->SetFVF(fvf);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Draw as triangle fan: 3 primitives from 5 vertices (center + 4 outer)
        device->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, 3);

        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };

    renderFrame();

    // Readback
    IDirect3DSurface9 *offscreen = nullptr;
    device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT lr;
    offscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY);

    auto getPixel = [&](int x, int y) -> uint32_t {
        return ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x];
    };

    // Center should be reddish (center vertex is red)
    uint32_t centerPix = getPixel(WIDTH / 2, HEIGHT / 2);
    bool centerNotBlack = (centerPix & 0x00FFFFFF) != 0;
    bool centerHasRed = ((centerPix >> 16) & 0xFF) > 50;

    // Sample points inside the fan (closer to center to avoid edge issues)
    // Top half center: should be in the top triangle
    uint32_t topPix = getPixel(WIDTH / 2, HEIGHT / 4);
    bool topNotBlack = (topPix & 0x00FFFFFF) != 0;
    // Right half center: should be in the right triangle
    uint32_t rightPix = getPixel(3 * WIDTH / 4 - 50, HEIGHT / 2);
    bool rightNotBlack = (rightPix & 0x00FFFFFF) != 0;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_fan.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, WIDTH, HEIGHT, lr.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = centerNotBlack && centerHasRed && topNotBlack && rightNotBlack;
    fprintf(stderr, "TEST fan: %s (center=0x%08x top=0x%08x right=0x%08x)\n",
            pass ? "PASS" : "FAIL", centerPix, topPix, rightPix);
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vb->Release(); device->Release(); d3d9->Release();
        return pass ? 0 : 1;
    }

    bool isRunning = true;
    while (isRunning) {
        MSG msg = {};
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) isRunning = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!isRunning) break;
        renderFrame();
    }

    vb->Release(); device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
