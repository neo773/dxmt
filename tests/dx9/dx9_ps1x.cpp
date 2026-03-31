#include "dx9_test_utils.h"
#include <d3d9.h>

// Simple ps_1_1: just pass through diffuse color
//   ps_1_1
//   mov r0, v0
static const DWORD ps11_simple[] = {
    0xFFFF0101,  // ps_1_1
    0x00000001, 0x800F0000, 0xD0E40000, // mov r0, v0
    0x0000FFFF,  // end
};

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
    winClass.lpszClassName = L"D3D9PS1xTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 PS 1.x Test (DXMT)",
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

    // First: verify clear+readback works WITHOUT any pixel shader
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF112233, 1.0f, 0);
    device->Present(nullptr, nullptr, nullptr, nullptr);

    IDirect3DSurface9 *offscreen = nullptr;
    device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT lr;
    offscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    uint32_t sanityPix = ((uint32_t *)lr.pBits)[0];
    offscreen->UnlockRect();
    fprintf(stderr, "Sanity clear: 0x%08x (expect 0xFF112233)\n", sanityPix);

    // Now create PS and test
    IDirect3DPixelShader9 *psSimple = nullptr;
    hr = device->CreatePixelShader(ps11_simple, &psSimple);
    fprintf(stderr, "CreatePixelShader hr=0x%08x\n", (unsigned)hr);

    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    struct Vertex { float x, y, z; DWORD color; float u, v; };
    Vertex verts[] = {
        { -1.0f,  1.0f, 0.5f, 0xFFFF0000, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.5f, 0xFFFF0000, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000, 0.0f, 1.0f },
        {  1.0f,  1.0f, 0.5f, 0xFFFF0000, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f, 0xFFFF0000, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.5f, 0xFFFF0000, 0.0f, 1.0f },
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
        device->SetPixelShader(psSimple);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
        device->SetPixelShader(nullptr);
        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };

    // Render once without Present, then readback
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->BeginScene();
    device->SetFVF(fvf);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetPixelShader(psSimple);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    device->SetPixelShader(nullptr);
    device->EndScene();

    // Readback
    device->GetRenderTargetData(backbuffer, offscreen);
    offscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY);

    auto getPixel = [&](int x, int y) -> uint32_t {
        return ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x];
    };

    uint32_t centerPix = getPixel(WIDTH / 2, HEIGHT / 2);
    uint8_t r = (centerPix >> 16) & 0xFF;
    uint8_t g = (centerPix >> 8) & 0xFF;
    uint8_t b = centerPix & 0xFF;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_ps1x.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, WIDTH, HEIGHT, lr.Pitch);
    offscreen->UnlockRect();

    // mov r0, v0 with red diffuse → should be red (high R, low G and B)
    bool pass = r > 200 && g < 20 && b < 20;
    fprintf(stderr, "TEST ps1x_render: %s (center=0x%08x)\n",
            pass ? "PASS" : "FAIL", centerPix);
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        if (psSimple) psSimple->Release(); vb->Release();
        backbuffer->Release(); offscreen->Release();
        device->Release(); d3d9->Release();
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

    if (psSimple) psSimple->Release(); vb->Release();
    backbuffer->Release(); offscreen->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
