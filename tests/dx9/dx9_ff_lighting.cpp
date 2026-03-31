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
    winClass.lpszClassName = L"D3D9LightingTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 FF Lighting Test (DXMT)",
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

    // Triangle with normals facing camera (0,0,-1)
    DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL;
    struct Vertex { float x, y, z; float nx, ny, nz; };
    Vertex verts[] = {
        { -0.8f, -0.8f, 0.5f,  0.0f, 0.0f, -1.0f },
        {  0.0f,  0.8f, 0.5f,  0.0f, 0.0f, -1.0f },
        {  0.8f, -0.8f, 0.5f,  0.0f, 0.0f, -1.0f },
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

    // Set up a directional light pointing towards the triangle
    D3DLIGHT9 light = {};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
    light.Ambient = { 0.0f, 0.0f, 0.0f, 0.0f };
    light.Direction.x = 0.0f;
    light.Direction.y = 0.0f;
    light.Direction.z = 1.0f; // towards +Z (towards the triangle)
    device->SetLight(0, &light);
    device->LightEnable(0, TRUE);

    // White material
    D3DMATERIAL9 mat = {};
    mat.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
    mat.Ambient = { 0.2f, 0.2f, 0.2f, 1.0f };
    device->SetMaterial(&mat);

    // Global ambient
    device->SetRenderState(D3DRS_AMBIENT, 0xFF333333);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();

        device->SetFVF(fvf);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, TRUE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);

        // Use default texture stage (SELECTARG1=DIFFUSE for stage 0 with no texture)
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

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

    // The triangle should be lit (not black). With a directional light pointing at
    // the triangle (dot(N,L) = dot((0,0,-1), (0,0,-1)) = 1), and white material,
    // the triangle should be white (plus ambient).
    uint32_t centerPix = getPixel(WIDTH / 2, HEIGHT / 2);
    uint8_t r = (centerPix >> 16) & 0xFF;
    uint8_t g = (centerPix >> 8) & 0xFF;
    uint8_t b = (centerPix >> 0) & 0xFF;

    // Should be bright (lit by directional light)
    bool isBright = r > 100 && g > 100 && b > 100;

    // Background should be black
    uint32_t bgPix = getPixel(10, 10);
    bool bgBlack = (bgPix & 0x00FFFFFF) == 0;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_ff_lighting.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lr.pBits, WIDTH, HEIGHT, lr.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = isBright && bgBlack;
    fprintf(stderr, "TEST ff_lighting: %s (center=0x%08x r=%u g=%u b=%u bg=0x%08x)\n",
            pass ? "PASS" : "FAIL", centerPix, r, g, b, bgPix);
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
