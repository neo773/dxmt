#include "dx9_test_utils.h"
#include <d3d9.h>
#include <d3dcompiler.h>

// Shader: position (float4 with z) + color passthrough
static const char *depthHLSL = R"(
struct VS_Input {
    float4 pos : POSITION;
    float4 color : COLOR;
};
struct VS_Output {
    float4 position : POSITION;
    float4 color : COLOR;
};
VS_Output vs_main(VS_Input input) {
    VS_Output output;
    output.position = input.pos;
    output.color = input.color;
    return output;
}
float4 ps_main(VS_Output input) : COLOR {
    return input.color;
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
    winClass.lpszClassName = L"D3D9DepthWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 800, 600 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Depth Test (DXMT)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top,
                                0, 0, hInstance, 0);
    if (!hwnd) { fprintf(stderr, "FATAL: CreateWindow failed\n"); return 1; }

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { fprintf(stderr, "FATAL: Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d3dpp.BackBufferCount = 1;
    d3dpp.hDeviceWindow = hwnd;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;

    IDirect3DDevice9* device;
    HRESULT hr = d3d9->CreateDevice(0, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &d3dpp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 1; }

    IDirect3DSurface9* bbQuery = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbQuery);
    D3DSURFACE_DESC bbDesc;
    bbQuery->GetDesc(&bbDesc);
    const UINT WIDTH = bbDesc.Width;
    const UINT HEIGHT = bbDesc.Height;
    bbQuery->Release();

    // Compile shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* vsErrors = nullptr;
    hr = D3DCompile(depthHLSL, strlen(depthHLSL), "depth.hlsl", nullptr, nullptr,
                    "vs_main", "vs_3_0", 0, 0, &vsBlob, &vsErrors);
    if (FAILED(hr)) {
        if (vsErrors) fprintf(stderr, "FATAL: VS compile: %s\n", (const char*)vsErrors->GetBufferPointer());
        return 1;
    }
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* psErrors = nullptr;
    hr = D3DCompile(depthHLSL, strlen(depthHLSL), "depth.hlsl", nullptr, nullptr,
                    "ps_main", "ps_3_0", 0, 0, &psBlob, &psErrors);
    if (FAILED(hr)) {
        if (psErrors) fprintf(stderr, "FATAL: PS compile: %s\n", (const char*)psErrors->GetBufferPointer());
        return 1;
    }

    IDirect3DVertexShader9* vertexShader;
    device->CreateVertexShader((const DWORD*)vsBlob->GetBufferPointer(), &vertexShader);
    IDirect3DPixelShader9* pixelShader;
    device->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &pixelShader);
    vsBlob->Release(); psBlob->Release();

    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* vertexDecl;
    device->CreateVertexDeclaration(vertexElements, &vertexDecl);

    // Two overlapping full-screen quads at different Z values
    // Triangle A (red) at z=0.3 — drawn SECOND but closer (should win)
    // Triangle B (blue) at z=0.7 — drawn FIRST but farther
    float triB[] = { // Blue, z=0.7 (far)
        -0.8f, -0.8f, 0.7f, 1.0f,   0,0,1,1,
         0.8f, -0.8f, 0.7f, 1.0f,   0,0,1,1,
         0.0f,  0.8f, 0.7f, 1.0f,   0,0,1,1,
    };
    float triA[] = { // Red, z=0.3 (close)
        -0.8f, -0.8f, 0.3f, 1.0f,   1,0,0,1,
         0.8f, -0.8f, 0.3f, 1.0f,   1,0,0,1,
         0.0f,  0.8f, 0.3f, 1.0f,   1,0,0,1,
    };
    UINT stride = 8 * sizeof(float);

    IDirect3DVertexBuffer9 *vbB, *vbA;
    device->CreateVertexBuffer(sizeof(triB), 0, 0, D3DPOOL_DEFAULT, &vbB, nullptr);
    device->CreateVertexBuffer(sizeof(triA), 0, 0, D3DPOOL_DEFAULT, &vbA, nullptr);
    void *p;
    vbB->Lock(0, sizeof(triB), &p, 0); memcpy(p, triB, sizeof(triB)); vbB->Unlock();
    vbA->Lock(0, sizeof(triA), &p, 0); memcpy(p, triA, sizeof(triA)); vbA->Unlock();

    D3DCOLOR clearColor = D3DCOLOR_XRGB(40, 40, 40);

    auto renderFrame = [&](UINT vpW, UINT vpH) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
        device->BeginScene();

        D3DVIEWPORT9 vp = { 0, 0, vpW, vpH, 0.0f, 1.0f };
        device->SetViewport(&vp);
        device->SetVertexDeclaration(vertexDecl);
        device->SetVertexShader(vertexShader);
        device->SetPixelShader(pixelShader);

        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

        // Draw far blue triangle first
        device->SetStreamSource(0, vbB, 0, stride);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

        // Draw close red triangle second
        device->SetStreamSource(0, vbA, 0, stride);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };

    renderFrame(WIDTH, HEIGHT);

    // Read back
    IDirect3DSurface9* offscreen = nullptr;
    device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    IDirect3DSurface9* backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT lockedRect;
    offscreen->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_depth.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    // Center pixel should be red (closer triangle wins with depth test)
    const uint8_t *pixel = (const uint8_t *)lockedRect.pBits + (HEIGHT/2) * lockedRect.Pitch + (WIDTH/2) * 4;
    fprintf(stderr, "INFO: center pixel BGRA = (%u, %u, %u, %u)\n", pixel[0], pixel[1], pixel[2], pixel[3]);

    // Red in BGRA = (0, 0, 255, 255) — R channel should be dominant
    bool pass = (pixel[2] > 200 && pixel[0] < 50); // high R, low B in BGRA format
    fprintf(stderr, "TEST depth: %s\n", pass ? "PASS" : "FAIL");

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vbA->Release(); vbB->Release();
        vertexDecl->Release();
        pixelShader->Release(); vertexShader->Release();
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
        renderFrame(WIDTH, HEIGHT);
    }

    vbA->Release(); vbB->Release();
    vertexDecl->Release();
    pixelShader->Release(); vertexShader->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
