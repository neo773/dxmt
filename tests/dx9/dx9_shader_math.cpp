#include "dx9_test_utils.h"
#include <cmath>
#include <d3d9.h>
#include <d3dcompiler.h>

// Test shader math — uses simple operations that map to known DXSO opcodes
// All math done via input color to avoid constant register collision issues
static const char *mathHLSL = R"(
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
    // input.color = (0.25, 0.5, 0.75, 1.0)
    // R = min(input.color.x, 0.5) = min(0.25, 0.5) = 0.25
    // G = max(input.color.y, 0.75) = max(0.5, 0.75) = 0.75
    // B = saturate(input.color.z * 2.0) = saturate(1.5) = 1.0
    // A = 1.0
    float r = min(input.color.x, input.color.y); // min(0.25, 0.5) = 0.25
    float g = max(input.color.y, input.color.z); // max(0.5, 0.75) = 0.75
    float b = input.color.x + input.color.y;      // 0.25 + 0.5 = 0.75
    return float4(r, g, b, 1.0);
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
    winClass.lpszClassName = L"D3D9MathWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 800, 600 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Shader Math Test (DXMT)",
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
    hr = D3DCompile(mathHLSL, strlen(mathHLSL), "math.hlsl", nullptr, nullptr,
                    "vs_main", "vs_3_0", 0, 0, &vsBlob, &vsErrors);
    if (FAILED(hr)) {
        if (vsErrors) fprintf(stderr, "FATAL: VS compile: %s\n", (const char*)vsErrors->GetBufferPointer());
        return 1;
    }
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* psErrors = nullptr;
    hr = D3DCompile(mathHLSL, strlen(mathHLSL), "math.hlsl", nullptr, nullptr,
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
        { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 8, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* vertexDecl;
    device->CreateVertexDeclaration(vertexElements, &vertexDecl);

    // Full-screen quad with color (0.25, 0.5, 0.75, 1.0)
    float quad[] = {
        -1,-1,  0.25f,0.5f,0.75f,1.0f,
         1,-1,  0.25f,0.5f,0.75f,1.0f,
        -1, 1,  0.25f,0.5f,0.75f,1.0f,
         1,-1,  0.25f,0.5f,0.75f,1.0f,
         1, 1,  0.25f,0.5f,0.75f,1.0f,
        -1, 1,  0.25f,0.5f,0.75f,1.0f,
    };
    UINT stride = 6 * sizeof(float);

    IDirect3DVertexBuffer9* vb;
    device->CreateVertexBuffer(sizeof(quad), 0, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    void *p;
    vb->Lock(0, sizeof(quad), &p, 0); memcpy(p, quad, sizeof(quad)); vb->Unlock();

    (void)0; // no PS constants needed for this test

    D3DCOLOR clearColor = D3DCOLOR_XRGB(0, 0, 0);

    auto renderFrame = [&](UINT vpW, UINT vpH) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
        device->BeginScene();

        D3DVIEWPORT9 vp = { 0, 0, vpW, vpH, 0.0f, 1.0f };
        device->SetViewport(&vp);
        device->SetVertexDeclaration(vertexDecl);
        device->SetVertexShader(vertexShader);
        device->SetPixelShader(pixelShader);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

        device->SetStreamSource(0, vb, 0, stride);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

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
    GetOutputPath("dx9_shader_math.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, lockedRect.pBits, WIDTH, HEIGHT, lockedRect.Pitch);
    fprintf(stderr, "OK: wrote %s\n", bmpPath);

    const uint8_t *pixel = (const uint8_t *)lockedRect.pBits + (HEIGHT/2) * lockedRect.Pitch + (WIDTH/2) * 4;
    fprintf(stderr, "INFO: center pixel BGRA = (%u, %u, %u, %u)\n", pixel[0], pixel[1], pixel[2], pixel[3]);

    // Expected: R=0.25→64, G=0.75→191, B=0.75→191 (in 0-255 range)
    // BGRA layout: pixel[0]=B, pixel[1]=G, pixel[2]=R
    int expectedR = 64;  // min(0.25, 0.5) = 0.25
    int expectedG = 191; // max(0.5, 0.75) = 0.75
    int expectedB = 191; // 0.25 + 0.5 = 0.75

    bool rOk = abs((int)pixel[2] - expectedR) < 10;
    bool gOk = abs((int)pixel[1] - expectedG) < 10;
    bool bOk = abs((int)pixel[0] - expectedB) < 10;

    fprintf(stderr, "  R: got %u, expected ~%d: %s\n", pixel[2], expectedR, rOk ? "OK" : "FAIL");
    fprintf(stderr, "  G: got %u, expected ~%d: %s\n", pixel[1], expectedG, gOk ? "OK" : "FAIL");
    fprintf(stderr, "  B: got %u, expected ~%d: %s\n", pixel[0], expectedB, bOk ? "OK" : "FAIL");

    bool pass = rOk && gOk && bOk;
    fprintf(stderr, "TEST shader_math: %s\n", pass ? "PASS" : "FAIL");

    offscreen->UnlockRect();
    backbuffer->Release();
    offscreen->Release();

    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    if (autoExit) {
        vb->Release(); vertexDecl->Release();
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

    vb->Release(); vertexDecl->Release();
    pixelShader->Release(); vertexShader->Release();
    device->Release(); d3d9->Release();
    return pass ? 0 : 1;
}
