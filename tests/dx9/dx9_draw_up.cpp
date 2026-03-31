#include "dx9_test_utils.h"
#include <d3d9.h>
#include <d3dcompiler.h>

static const char *shaderHLSL = R"(
struct VS_Input {
    float4 pos : POSITION;
    float4 color : COLOR0;
};

struct VS_Output {
    float4 position : POSITION;
    float4 color    : COLOR0;
};

VS_Output vs_main(VS_Input input) {
    VS_Output output;
    output.position = input.pos;
    output.color = input.color;
    return output;
}

float4 ps_main(float4 color : COLOR0) : COLOR {
    return color;
}
)";

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
    winClass.lpszClassName = L"D3D9DrawUPWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 DrawPrimitiveUP Test (DXMT)",
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

    // Compile shaders
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errBlob = nullptr;
    hr = D3DCompile(shaderHLSL, strlen(shaderHLSL), "up.hlsl", nullptr, nullptr,
                     "vs_main", "vs_3_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        fprintf(stderr, "VS compile failed: %s\n", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return 1;
    }
    hr = D3DCompile(shaderHLSL, strlen(shaderHLSL), "up.hlsl", nullptr, nullptr,
                     "ps_main", "ps_3_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        fprintf(stderr, "PS compile failed: %s\n", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return 1;
    }

    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    device->CreateVertexShader((const DWORD*)vsBlob->GetBufferPointer(), &vs);
    device->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &ps);
    vsBlob->Release(); psBlob->Release();

    D3DVERTEXELEMENT9 decl[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *vdecl = nullptr;
    device->CreateVertexDeclaration(decl, &vdecl);

    // Fullscreen quad vertices using user pointer (no VB)
    struct Vertex { float x, y, z, w; DWORD color; };
    Vertex triVerts[] = {
        { -1.0f,  1.0f, 0.5f, 1.0f, 0xFF00FF00 }, // top-left - green
        {  1.0f,  1.0f, 0.5f, 1.0f, 0xFF00FF00 }, // top-right - green
        {  0.0f, -1.0f, 0.5f, 1.0f, 0xFF00FF00 }, // bottom - green
    };

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();
        device->SetVertexShader(vs);
        device->SetPixelShader(ps);
        device->SetVertexDeclaration(vdecl);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triVerts, sizeof(Vertex));
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

    // Center should be green (the triangle covers center)
    uint32_t center = getPixel(WIDTH / 2, HEIGHT / 2);
    bool centerGreen = ((center >> 8) & 0xFF) > 0x20; // some green present

    // Bottom-left corner should be black (outside triangle)
    // Triangle goes from (-1,1) to (1,1) to (0,-1), so bottom corners are outside
    uint32_t corner = getPixel(0, HEIGHT - 1);
    bool cornerBlack = (corner & 0x00FFFFFF) == 0;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_draw_up.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = centerGreen && cornerBlack;
    fprintf(stderr, "TEST draw_up: %s (center=0x%08x corner=0x%08x)\n",
            pass ? "PASS" : "FAIL", center, corner);
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vdecl->Release(); vs->Release(); ps->Release();
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

    vdecl->Release(); vs->Release(); ps->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
