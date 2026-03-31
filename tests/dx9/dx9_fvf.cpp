#include "dx9_test_utils.h"
#include <d3d9.h>
#include <d3dcompiler.h>

static const char *shaderHLSL = R"(
struct VS_Input {
    float3 pos : POSITION;
    float4 color : COLOR0;
};

struct VS_Output {
    float4 position : POSITION;
    float4 color    : COLOR0;
};

VS_Output vs_main(VS_Input input) {
    VS_Output output;
    output.position = float4(input.pos, 1.0);
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
    winClass.lpszClassName = L"D3D9FVFWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 FVF Test (DXMT)",
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
    hr = D3DCompile(shaderHLSL, strlen(shaderHLSL), "fvf.hlsl", nullptr, nullptr,
                     "vs_main", "vs_3_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        fprintf(stderr, "VS compile failed: %s\n", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return 1;
    }
    hr = D3DCompile(shaderHLSL, strlen(shaderHLSL), "fvf.hlsl", nullptr, nullptr,
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

    // Use SetFVF instead of CreateVertexDeclaration
    // D3DFVF_XYZ = float3 position, D3DFVF_DIFFUSE = D3DCOLOR
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;

    struct Vertex { float x, y, z; DWORD color; };
    Vertex triVerts[] = {
        {  0.0f,  0.5f, 0.5f, 0xFFFF0000 }, // top - red
        {  0.5f, -0.5f, 0.5f, 0xFF00FF00 }, // right - green
        { -0.5f, -0.5f, 0.5f, 0xFF0000FF }, // left - blue
    };

    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(sizeof(triVerts), 0, fvf, D3DPOOL_DEFAULT, &vb, nullptr);
    void *vbData = nullptr;
    vb->Lock(0, sizeof(triVerts), &vbData, 0);
    memcpy(vbData, triVerts, sizeof(triVerts));
    vb->Unlock();

    // Verify GetFVF roundtrip
    device->SetFVF(fvf);
    DWORD retrievedFVF = 0;
    device->GetFVF(&retrievedFVF);
    bool fvfRoundtrip = (retrievedFVF == fvf);
    fprintf(stderr, "TEST fvf_roundtrip: %s (set=0x%x got=0x%x)\n",
            fvfRoundtrip ? "PASS" : "FAIL", (unsigned)fvf, (unsigned)retrievedFVF);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();
        device->SetVertexShader(vs);
        device->SetPixelShader(ps);
        device->SetFVF(fvf);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
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

    // Check center of screen (should be non-black since triangle is there)
    uint32_t center = getPixel(WIDTH / 2, HEIGHT / 2);
    bool centerNotBlack = (center & 0x00FFFFFF) != 0;

    // Check corners (should be black/background)
    uint32_t corner = getPixel(0, 0);
    bool cornerBlack = (corner & 0x00FFFFFF) == 0;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_fvf.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    bool pass = fvfRoundtrip && centerNotBlack && cornerBlack;
    fprintf(stderr, "TEST fvf_render: %s (center=0x%08x corner=0x%08x)\n",
            (centerNotBlack && cornerBlack) ? "PASS" : "FAIL", center, corner);
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vb->Release(); vs->Release(); ps->Release();
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

    vb->Release(); vs->Release(); ps->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
