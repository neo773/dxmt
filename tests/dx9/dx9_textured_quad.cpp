#include "dx9_test_utils.h"
#include <d3d9.h>
#include <d3dcompiler.h>

static const char *quadHLSL = R"(
struct VS_Input {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VS_Output {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

VS_Output vs_main(VS_Input input) {
    VS_Output output;
    output.position = input.pos;
    output.uv = input.uv;
    return output;
}

sampler2D s0 : register(s0);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR {
    return tex2D(s0, uv);
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
    winClass.lpszClassName = L"D3D9TexQuadWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Textured Quad (DXMT)",
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
    hr = D3DCompile(quadHLSL, strlen(quadHLSL), "quad.hlsl", nullptr, nullptr,
                     "vs_main", "vs_3_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        fprintf(stderr, "VS compile failed: %s\n", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return 1;
    }
    hr = D3DCompile(quadHLSL, strlen(quadHLSL), "quad.hlsl", nullptr, nullptr,
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

    // Vertex declaration: POSITION (float4) + TEXCOORD0 (float2)
    D3DVERTEXELEMENT9 decl[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *vdecl = nullptr;
    device->CreateVertexDeclaration(decl, &vdecl);

    // Fullscreen quad vertices: position (x,y,z,w) + texcoord (u,v)
    struct Vertex { float x, y, z, w, u, v; };
    Vertex quadVerts[] = {
        { -1.0f,  1.0f, 0.5f, 1.0f, 0.0f, 0.0f }, // top-left
        {  1.0f,  1.0f, 0.5f, 1.0f, 1.0f, 0.0f }, // top-right
        { -1.0f, -1.0f, 0.5f, 1.0f, 0.0f, 1.0f }, // bottom-left
        {  1.0f,  1.0f, 0.5f, 1.0f, 1.0f, 0.0f }, // top-right (dup)
        {  1.0f, -1.0f, 0.5f, 1.0f, 1.0f, 1.0f }, // bottom-right
        { -1.0f, -1.0f, 0.5f, 1.0f, 0.0f, 1.0f }, // bottom-left (dup)
    };

    IDirect3DVertexBuffer9 *vb = nullptr;
    device->CreateVertexBuffer(sizeof(quadVerts), 0, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    void *vbData = nullptr;
    vb->Lock(0, sizeof(quadVerts), &vbData, 0);
    memcpy(vbData, quadVerts, sizeof(quadVerts));
    vb->Unlock();

    // Create 2x2 checkerboard texture: red/blue in BGRA
    IDirect3DTexture9 *tex = nullptr;
    hr = device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateTexture failed 0x%08x\n", (unsigned)hr); return 1; }

    D3DLOCKED_RECT lr;
    tex->LockRect(0, &lr, nullptr, 0);
    uint32_t *pixels = (uint32_t *)lr.pBits;
    // D3DFMT_A8R8G8B8: ARGB in memory = BGRA byte order
    uint32_t red  = 0xFFFF0000; // A=FF R=FF G=00 B=00
    uint32_t blue = 0xFF0000FF; // A=FF R=00 G=00 B=FF
    // Checkerboard: row0=[red, blue], row1=[blue, red]
    pixels[0] = red;
    pixels[1] = blue;
    // Row 1 at pitch offset
    uint32_t *row1 = (uint32_t *)((uint8_t *)lr.pBits + lr.Pitch);
    row1[0] = blue;
    row1[1] = red;
    tex->UnlockRect(0);

    auto renderFrame = [&]() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();
        device->SetVertexShader(vs);
        device->SetPixelShader(ps);
        device->SetVertexDeclaration(vdecl);
        device->SetStreamSource(0, vb, 0, sizeof(Vertex));
        device->SetTexture(0, tex);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
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

    uint32_t tl = getPixel(0, 0);
    uint32_t tr = getPixel(WIDTH - 1, 0);
    uint32_t bl = getPixel(0, HEIGHT - 1);
    uint32_t br = getPixel(WIDTH - 1, HEIGHT - 1);

    bool pass = true;
    auto check = [&](const char *name, uint32_t got, uint32_t expected) {
        if ((got & 0x00FFFFFF) != (expected & 0x00FFFFFF)) {
            fprintf(stderr, "FAIL: %s = 0x%08X, expected 0x%08X\n", name, got, expected);
            pass = false;
        }
    };
    check("top-left", tl, red);
    check("top-right", tr, blue);
    check("bottom-left", bl, blue);
    check("bottom-right", br, red);

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_textured_quad.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    fprintf(stderr, "TEST textured_quad: %s\n", pass ? "PASS" : "FAIL");
    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        tex->Release();
        vb->Release();
        vdecl->Release();
        vs->Release();
        ps->Release();
        device->Release();
        d3d9->Release();
        return pass ? 0 : 1;
    }

    // Interactive mode — continuous render loop so HUD overlay works
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

    tex->Release();
    vb->Release();
    vdecl->Release();
    vs->Release();
    ps->Release();
    device->Release();
    d3d9->Release();
    return pass ? 0 : 1;
}
