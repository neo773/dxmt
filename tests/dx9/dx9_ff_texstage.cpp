#include "dx9_test_utils.h"
#include <d3d9.h>
#include <cmath>

// Create a 4x4 checkerboard texture (red and white)
static IDirect3DTexture9 *CreateCheckerTexture(IDirect3DDevice9 *device) {
    IDirect3DTexture9 *tex = nullptr;
    device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    D3DLOCKED_RECT lr;
    tex->LockRect(0, &lr, nullptr, 0);
    uint32_t *pixels = (uint32_t *)lr.pBits;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            bool checker = ((x + y) & 1) != 0;
            pixels[y * (lr.Pitch / 4) + x] = checker ? 0xFFFF0000 : 0xFFFFFFFF;
        }
    }
    tex->UnlockRect(0);
    return tex;
}

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
    winClass.lpszClassName = L"D3D9FFTexStageWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 FF TexStage Test (DXMT)",
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
    pp.BackBufferWidth = 1024;
    pp.BackBufferHeight = 768;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &pp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08x\n", (unsigned)hr); return 1; }

    IDirect3DSurface9 *bbQuery = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbQuery);
    D3DSURFACE_DESC bbDesc;
    bbQuery->GetDesc(&bbDesc);
    const UINT WIDTH = bbDesc.Width;
    const UINT HEIGHT = bbDesc.Height;
    bbQuery->Release();

    // FVF: position + diffuse + 1 texcoord (no explicit VS/PS — FF pipeline)
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    struct Vertex { float x, y, z; DWORD color; float u, v; };
    // Full-screen quad (two triangles)
    Vertex quadVerts[] = {
        { -1.0f,  1.0f, 0.5f, 0xFF808080, 0.0f, 0.0f }, // TL
        {  1.0f,  1.0f, 0.5f, 0xFF808080, 1.0f, 0.0f }, // TR
        { -1.0f, -1.0f, 0.5f, 0xFF808080, 0.0f, 1.0f }, // BL
        {  1.0f,  1.0f, 0.5f, 0xFF808080, 1.0f, 0.0f }, // TR
        {  1.0f, -1.0f, 0.5f, 0xFF808080, 1.0f, 1.0f }, // BR
        { -1.0f, -1.0f, 0.5f, 0xFF808080, 0.0f, 1.0f }, // BL
    };

    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(sizeof(quadVerts), 0, fvf, D3DPOOL_DEFAULT, &vb, nullptr);
    void *vbData = nullptr;
    vb->Lock(0, sizeof(quadVerts), &vbData, 0);
    memcpy(vbData, quadVerts, sizeof(quadVerts));
    vb->Unlock();

    IDirect3DTexture9 *tex = CreateCheckerTexture(device);

    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();

        // FF pipeline: no VS/PS
        device->SetFVF(fvf);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Default TSS stage 0: MODULATE (texture * diffuse)
        device->SetTexture(0, tex);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };

    renderFrame();

    // Readback and validate
    IDirect3DSurface9 *offscreen = nullptr;
    hr = device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateOffscreenPlainSurface failed\n"); return 1; }

    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    hr = device->GetRenderTargetData(backbuffer, offscreen);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: GetRenderTargetData failed\n"); return 1; }

    D3DLOCKED_RECT lockedRect;
    hr = offscreen->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: LockRect failed\n"); return 1; }

    auto getPixel = [&](int x, int y) -> uint32_t {
        return ((uint32_t *)((uint8_t *)lockedRect.pBits + y * lockedRect.Pitch))[x];
    };

    // The quad fills the entire screen with a checkerboard pattern modulated by 50% gray diffuse
    // White checker cells: 0xFFFFFFFF * 0xFF808080 ≈ 0xFF808080
    // Red checker cells: 0xFFFF0000 * 0xFF808080 ≈ 0xFF800000
    // We just need to verify the output is not black (FF pipeline working)
    // and has variation (texture sampling working)
    uint32_t pix1 = getPixel(0, 0);             // one checker cell
    uint32_t pix2 = getPixel(WIDTH / 4, 0);     // adjacent checker cell

    bool notBlack = (pix1 & 0x00FFFFFF) != 0;
    bool hasVariation = pix1 != pix2; // different checker cells should differ

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_ff_texstage.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = notBlack && hasVariation;
    fprintf(stderr, "TEST ff_texstage: %s (pix1=0x%08x pix2=0x%08x)\n",
            pass ? "PASS" : "FAIL", pix1, pix2);
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vb->Release(); tex->Release();
        device->Release(); d3d9->Release();
        return pass ? 0 : 1;
    }

    bool isRunning = true;
    while (isRunning) {
        MSG msg = {};
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) isRunning = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!isRunning) break;
        renderFrame();
    }

    vb->Release(); tex->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
