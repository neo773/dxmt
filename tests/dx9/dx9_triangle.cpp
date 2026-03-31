#include "dx9_test_utils.h"
#include <d3d9.h>
#include <d3dcompiler.h>

static const char *triangleHLSL = R"(
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
    winClass.lpszClassName = L"D3D9TriWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Triangle (DXMT)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top,
                                0, 0, hInstance, 0);
    if (!hwnd) { fprintf(stderr, "FATAL: CreateWindow failed\n"); return 1; }

    fprintf(stderr, "INFO: creating D3D9 device...\n");

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { fprintf(stderr, "FATAL: Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d3dpp.BackBufferCount = 1;
    d3dpp.hDeviceWindow = hwnd;

    IDirect3DDevice9* device;
    HRESULT hr = d3d9->CreateDevice(0, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &d3dpp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 1; }

    // Query actual backbuffer size
    IDirect3DSurface9* bbQuery = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbQuery);
    D3DSURFACE_DESC bbDesc;
    bbQuery->GetDesc(&bbDesc);
    const UINT WIDTH = bbDesc.Width;
    const UINT HEIGHT = bbDesc.Height;
    bbQuery->Release();
    fprintf(stderr, "INFO: backbuffer size = %ux%u\n", WIDTH, HEIGHT);

    // --- Compile shaders from inline HLSL ---
    fprintf(stderr, "INFO: compiling shaders...\n");

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* vsErrors = nullptr;
    hr = D3DCompile(triangleHLSL, strlen(triangleHLSL), "triangle.hlsl", nullptr, nullptr,
                    "vs_main", "vs_3_0", 0, 0, &vsBlob, &vsErrors);
    if (FAILED(hr)) {
        if (vsErrors) fprintf(stderr, "FATAL: VS compile error: %s\n", (const char*)vsErrors->GetBufferPointer());
        else fprintf(stderr, "FATAL: VS compile failed 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    ID3DBlob* psBlob = nullptr;
    ID3DBlob* psErrors = nullptr;
    hr = D3DCompile(triangleHLSL, strlen(triangleHLSL), "triangle.hlsl", nullptr, nullptr,
                    "ps_main", "ps_3_0", 0, 0, &psBlob, &psErrors);
    if (FAILED(hr)) {
        if (psErrors) fprintf(stderr, "FATAL: PS compile error: %s\n", (const char*)psErrors->GetBufferPointer());
        else fprintf(stderr, "FATAL: PS compile failed 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    IDirect3DVertexShader9* vertexShader;
    hr = device->CreateVertexShader((const DWORD*)vsBlob->GetBufferPointer(), &vertexShader);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateVertexShader failed 0x%08lX\n", (unsigned long)hr); return 1; }

    IDirect3DPixelShader9* pixelShader;
    hr = device->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &pixelShader);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreatePixelShader failed 0x%08lX\n", (unsigned long)hr); return 1; }

    vsBlob->Release();
    psBlob->Release();

    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 8,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* vertexDecl;
    hr = device->CreateVertexDeclaration(vertexElements, &vertexDecl);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateVertexDeclaration failed\n"); return 1; }

    float vertexData[] = {
         0.0f,  0.5f, 0.f, 1.f, 0.f, 1.f,
         0.5f, -0.5f, 1.f, 0.f, 0.f, 1.f,
        -0.5f, -0.5f, 0.f, 0.f, 1.f, 1.f
    };
    UINT stride = 6 * sizeof(float);

    IDirect3DVertexBuffer9* vertexBuffer;
    hr = device->CreateVertexBuffer(sizeof(vertexData), 0, 0, D3DPOOL_DEFAULT, &vertexBuffer, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateVertexBuffer failed\n"); return 1; }

    void* pData;
    vertexBuffer->Lock(0, sizeof(vertexData), &pData, 0);
    memcpy(pData, vertexData, sizeof(vertexData));
    vertexBuffer->Unlock();

    fprintf(stderr, "INFO: drawing triangle...\n");
    D3DCOLOR clearColor = D3DCOLOR_XRGB(25, 51, 153);

    auto renderFrame = [&](UINT vpW, UINT vpH) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
        device->BeginScene();

        D3DVIEWPORT9 vp = { 0, 0, vpW, vpH, 0.0f, 1.0f };
        device->SetViewport(&vp);
        device->SetStreamSource(0, vertexBuffer, 0, stride);
        device->SetVertexDeclaration(vertexDecl);
        device->SetVertexShader(vertexShader);
        device->SetPixelShader(pixelShader);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    };

    renderFrame(WIDTH, HEIGHT);

    // Read back and save screenshot
    IDirect3DSurface9* offscreen = nullptr;
    hr = device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateOffscreenPlainSurface failed\n"); return 1; }

    IDirect3DSurface9* backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

    hr = device->GetRenderTargetData(backbuffer, offscreen);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: GetRenderTargetData failed\n"); return 1; }

    D3DLOCKED_RECT lockedRect;
    hr = offscreen->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: LockRect failed\n"); return 1; }

    char triBmpPath[MAX_PATH];
    GetOutputPath("dx9_triangle.bmp", triBmpPath, MAX_PATH);
    WriteBMP(triBmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", triBmpPath);

    const uint8_t *pixel = (const uint8_t *)lockedRect.pBits + (HEIGHT/2) * lockedRect.Pitch + (WIDTH/2) * 4;
    fprintf(stderr, "INFO: triangle center pixel BGRA = (%u, %u, %u, %u)\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    bool triOk = !(pixel[0] == 153 && pixel[1] == 51 && pixel[2] == 25);
    fprintf(stderr, "TEST triangle: %s\n", triOk ? "PASS" : "FAIL");
    offscreen->UnlockRect();

    backbuffer->Release();
    offscreen->Release();

    fprintf(stderr, "\n%s\n", triOk ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vertexBuffer->Release(); vertexDecl->Release();
        pixelShader->Release(); vertexShader->Release();
        device->Release(); d3d9->Release();
        return triOk ? 0 : 1;
    }

    // Keep window open, re-render each frame
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

    vertexBuffer->Release(); vertexDecl->Release();
    pixelShader->Release(); vertexShader->Release();
    device->Release(); d3d9->Release();

    return triOk ? 0 : 1;
}
