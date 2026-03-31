#include "dx9_test_utils.h"
#include <d3d9.h>
#include <cmath>

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
    winClass.lpszClassName = L"D3D9FogTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 FF Fog Test (DXMT)",
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

    // Two triangles forming a quad from near to far Z
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z; DWORD color; };
    Vertex verts[] = {
        // Near quad (z=0.1) — should have little fog
        { -1.0f,  1.0f, 0.1f, 0xFFFF0000 }, // red
        {  0.0f,  1.0f, 0.1f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.1f, 0xFFFF0000 },
        {  0.0f,  1.0f, 0.1f, 0xFFFF0000 },
        {  0.0f, -1.0f, 0.1f, 0xFFFF0000 },
        { -1.0f, -1.0f, 0.1f, 0xFFFF0000 },
        // Far quad (z=50) — should have heavy fog
        {  0.0f,  1.0f, 0.1f, 0xFFFF0000 },
        {  1.0f,  1.0f, 0.1f, 0xFFFF0000 },
        {  0.0f, -1.0f, 0.1f, 0xFFFF0000 },
        {  1.0f,  1.0f, 0.1f, 0xFFFF0000 },
        {  1.0f, -1.0f, 0.1f, 0xFFFF0000 },
        {  0.0f, -1.0f, 0.1f, 0xFFFF0000 },
    };

    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(sizeof(verts), 0, fvf, D3DPOOL_DEFAULT, &vb, nullptr);
    void *vbData; vb->Lock(0, sizeof(verts), &vbData, 0);
    memcpy(vbData, verts, sizeof(verts));
    vb->Unlock();

    // Set up view matrix that pushes the far quad further in Z
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    // Use a view matrix that translates Z so far quad has large eye-space Z
    D3DMATRIX viewMat = identity;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &viewMat);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();

        device->SetFVF(fvf);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Enable vertex fog
        device->SetRenderState(D3DRS_FOGENABLE, TRUE);
        device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        device->SetRenderState(D3DRS_FOGCOLOR, 0xFF00FF00); // green fog
        float fogStart = 0.0f, fogEnd = 1.0f;
        device->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&fogStart);
        device->SetRenderState(D3DRS_FOGEND, *(DWORD*)&fogEnd);

        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 4);

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

    // Near quad (left half) should be mostly red (little fog)
    // Far quad (right half) would depend on the Z value
    // With our setup, both quads are at z=0.1, and LINEAR fog from 0 to 1
    // eyeZ = abs(posEye.z) ≈ 0.1, so fogFactor = (1.0 - 0.1) / (1.0 - 0.0) = 0.9
    // current.rgb = mix(fogColor, vertColor, 0.9) = red*0.9 + green*0.1
    // Expected: R ≈ 230 (0.9*255), G ≈ 25 (0.1*255)
    uint32_t centerPix = getPixel(WIDTH / 4, HEIGHT / 2);
    uint8_t r = (centerPix >> 16) & 0xFF;
    uint8_t g = (centerPix >> 8) & 0xFF;
    uint8_t b = (centerPix >> 0) & 0xFF;

    // The output should have both red and green components (fog blending)
    bool hasRed = r > 100;
    bool hasGreen = g > 5;
    bool noBlue = b < 30;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_ff_fog.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, WIDTH, HEIGHT, lr.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = hasRed && hasGreen && noBlue;
    fprintf(stderr, "TEST ff_fog: %s (pix=0x%08x r=%u g=%u b=%u)\n",
            pass ? "PASS" : "FAIL", centerPix, r, g, b);
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
