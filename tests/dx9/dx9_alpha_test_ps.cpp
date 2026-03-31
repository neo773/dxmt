#include "dx9_test_utils.h"
#include <d3d9.h>

// ps_1_1: pass through v0 (diffuse color, including alpha)
//   ps_1_1
//   mov r0, v0
static const DWORD ps11_passthrough[] = {
    0xFFFF0101,  // ps_1_1
    0x00000001, 0x800F0000, 0xD0E40000, // mov r0, v0
    0x0000FFFF,  // end
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    [[maybe_unused]] bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));
    WNDCLASSEXW winClass = {};
    winClass.cbSize = sizeof(WNDCLASSEXW);
    winClass.style = CS_HREDRAW | CS_VREDRAW;
    winClass.lpfnWndProc = &WndProc;
    winClass.hInstance = hInstance;
    winClass.hIcon = LoadIconW(0, IDI_APPLICATION);
    winClass.hCursor = LoadCursorW(0, IDC_ARROW);
    winClass.lpszClassName = L"D3D9AlphaTestPSClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Alpha Test PS Test (DXMT)",
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

    // Create pixel shader
    IDirect3DPixelShader9 *ps = nullptr;
    hr = device->CreatePixelShader(ps11_passthrough, &ps);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreatePixelShader failed\n"); return 1; }

    // Vertices: two quads side by side
    // Left quad: x=-1..0, diffuse alpha = 204/255 (~0.8) — above ref
    // Right quad: x=0..1, diffuse alpha = 51/255 (~0.2) — below ref
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z; DWORD color; };

    // Alpha ref = 128 → 0.502
    DWORD colorAbove = 0xCC00FF00; // A=204, R=0, G=255, B=0
    DWORD colorBelow = 0x33FF0000; // A=51, R=255, G=0, B=0

    Vertex verts[] = {
        // Left quad (visible, alpha > ref)
        { -1.0f,  1.0f, 0.5f, colorAbove },
        {  0.0f,  1.0f, 0.5f, colorAbove },
        { -1.0f, -1.0f, 0.5f, colorAbove },
        {  0.0f,  1.0f, 0.5f, colorAbove },
        {  0.0f, -1.0f, 0.5f, colorAbove },
        { -1.0f, -1.0f, 0.5f, colorAbove },
        // Right quad (discarded, alpha < ref)
        {  0.0f,  1.0f, 0.5f, colorBelow },
        {  1.0f,  1.0f, 0.5f, colorBelow },
        {  0.0f, -1.0f, 0.5f, colorBelow },
        {  1.0f,  1.0f, 0.5f, colorBelow },
        {  1.0f, -1.0f, 0.5f, colorBelow },
        {  0.0f, -1.0f, 0.5f, colorBelow },
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

    // Test 1: GREATEREQUAL with ref=128 — left visible, right discarded
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->BeginScene();
    device->SetFVF(fvf);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetPixelShader(ps);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 4);
    device->EndScene();

    // Readback
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    IDirect3DSurface9 *offscreen = nullptr;
    device->CreateOffscreenPlainSurface(1024, 768, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT readLr;
    offscreen->LockRect(&readLr, nullptr, D3DLOCK_READONLY);

    // Sample left quad center (256, 384) and right quad center (768, 384)
    uint32_t leftPix = ((uint32_t *)((uint8_t *)readLr.pBits + 384 * readLr.Pitch))[256];
    uint32_t rightPix = ((uint32_t *)((uint8_t *)readLr.pBits + 384 * readLr.Pitch))[768];

    uint8_t leftG = (leftPix >> 8) & 0xFF;
    uint8_t rightR = (rightPix >> 16) & 0xFF;
    uint8_t rightG = (rightPix >> 8) & 0xFF;
    uint8_t rightB = rightPix & 0xFF;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_alpha_test_ps.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, readLr.pBits, 1024, 768, readLr.Pitch);
    offscreen->UnlockRect();

    // Left quad should be green (alpha > ref, passes GREATEREQUAL)
    bool leftVisible = leftG > 200;
    // Right quad should be black (alpha < ref, discarded by GREATEREQUAL)
    bool rightDiscarded = (rightR < 10 && rightG < 10 && rightB < 10);

    fprintf(stderr, "TEST ge_visible: %s (left G=%d, expect >200)\n",
            leftVisible ? "PASS" : "FAIL", leftG);
    fprintf(stderr, "TEST ge_discarded: %s (right R=%d G=%d B=%d, expect black)\n",
            rightDiscarded ? "PASS" : "FAIL", rightR, rightG, rightB);

    // Test 2: D3DCMP_LESS with same ref — left discarded (alpha >= ref), right visible (alpha < ref)
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->BeginScene();
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 4);
    device->SetPixelShader(nullptr);
    device->EndScene();

    device->GetRenderTargetData(backbuffer, offscreen);
    offscreen->LockRect(&readLr, nullptr, D3DLOCK_READONLY);

    leftPix = ((uint32_t *)((uint8_t *)readLr.pBits + 384 * readLr.Pitch))[256];
    rightPix = ((uint32_t *)((uint8_t *)readLr.pBits + 384 * readLr.Pitch))[768];

    uint8_t leftR2 = (leftPix >> 16) & 0xFF;
    uint8_t leftG2 = (leftPix >> 8) & 0xFF;
    uint8_t leftB2 = leftPix & 0xFF;
    uint8_t rightR2 = (rightPix >> 16) & 0xFF;

    GetOutputPath("dx9_alpha_test_ps_less.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, readLr.pBits, 1024, 768, readLr.Pitch);
    offscreen->UnlockRect();

    // Left should be black (alpha >= ref, fails LESS)
    bool leftDiscarded2 = (leftR2 < 10 && leftG2 < 10 && leftB2 < 10);
    // Right should be red (alpha < ref, passes LESS)
    bool rightVisible2 = rightR2 > 200;

    fprintf(stderr, "TEST less_discarded: %s (left R=%d G=%d B=%d, expect black)\n",
            leftDiscarded2 ? "PASS" : "FAIL", leftR2, leftG2, leftB2);
    fprintf(stderr, "TEST less_visible: %s (right R=%d, expect >200)\n",
            rightVisible2 ? "PASS" : "FAIL", rightR2);

    bool allPass = leftVisible && rightDiscarded && leftDiscarded2 && rightVisible2;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    ps->Release(); vb->Release();
    backbuffer->Release(); offscreen->Release();
    device->Release(); d3d9->Release();
    return allPass ? 0 : 1;
}
