#include "dx9_test_utils.h"
#include <d3d9.h>

// Test DXSO source register modifiers: _comp (1-x), _x2 (2*x), _bias (x-0.5)
// We test these by using ps_1_1 with modifier tokens in the bytecode.
//
// Test 1 (_comp): mov r0, 1-v0  — input white (1,1,1,1) → expect black (0,0,0)
// Test 2 (_x2):   mov r0, v0_x2 — input (0.25,0.25,0.25) → expect (0.5,0.5,0.5)
// Test 3 (_bias): mov r0, v0_bias — input (0.75,0.75,0.75) → expect (0.25,0.25,0.25)

// ps_1_1 bytecodes with source modifiers:
// Token format for source register: tttttttt_mmmm_ssssssss_nnnnnnnnnn1
// where mmmm = modifier (4 bits at 24-27)
// DxsoRegModifier::Comp = 6, X2 = 7, Bias = 2

// ps_1_1; mov r0, 1-v0  (comp modifier = 6 << 24 = 0x06000000)
static const DWORD ps_comp[] = {
    0xFFFF0101,                          // ps_1_1
    0x00000001, 0x800F0000, 0x96E40000,  // mov r0, 1-v0  (0x90 | 0x06000000)
    0x0000FFFF,                          // end
};

// ps_1_1; mov r0, v0_x2  (x2 modifier = 7 << 24 = 0x07000000)
static const DWORD ps_x2[] = {
    0xFFFF0101,                          // ps_1_1
    0x00000001, 0x800F0000, 0x97E40000,  // mov r0, v0_x2  (0x90 | 0x07000000)
    0x0000FFFF,                          // end
};

// ps_1_1; mov r0, v0_bias  (bias modifier = 2 << 24 = 0x02000000)
static const DWORD ps_bias[] = {
    0xFFFF0101,                          // ps_1_1
    0x00000001, 0x800F0000, 0x92E40000,  // mov r0, v0_bias  (0x90 | 0x02000000)
    0x0000FFFF,                          // end
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
    winClass.lpszClassName = L"D3D9SrcModTestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Source Modifiers Test (DXMT)",
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

    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    struct Vertex { float x, y, z; DWORD color; float u, v; };

    // Helper: draw a fullscreen quad with given color, pixel shader, readback center pixel
    auto testModifier = [&](const char *name, const DWORD *psCode, DWORD vertexColor,
                            int expectR, int expectG, int expectB, int tolerance) -> bool {
        IDirect3DPixelShader9 *ps = nullptr;
        hr = device->CreatePixelShader(psCode, &ps);
        if (FAILED(hr)) {
            fprintf(stderr, "TEST %s: FAIL (CreatePixelShader hr=0x%08x)\n", name, (unsigned)hr);
            return false;
        }

        Vertex verts[] = {
            { -1.0f,  1.0f, 0.5f, vertexColor, 0.0f, 0.0f },
            {  1.0f,  1.0f, 0.5f, vertexColor, 1.0f, 0.0f },
            { -1.0f, -1.0f, 0.5f, vertexColor, 0.0f, 1.0f },
            {  1.0f,  1.0f, 0.5f, vertexColor, 1.0f, 0.0f },
            {  1.0f, -1.0f, 0.5f, vertexColor, 1.0f, 1.0f },
            { -1.0f, -1.0f, 0.5f, vertexColor, 0.0f, 1.0f },
        };

        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        device->BeginScene();
        device->SetFVF(fvf);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetPixelShader(ps);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(Vertex));
        device->SetPixelShader(nullptr);
        device->EndScene();

        IDirect3DSurface9 *backbuffer = nullptr;
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        IDirect3DSurface9 *offscreen = nullptr;
        device->CreateOffscreenPlainSurface(1024, 768, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
        device->GetRenderTargetData(backbuffer, offscreen);

        D3DLOCKED_RECT lr;
        offscreen->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        uint32_t pix = ((uint32_t *)((uint8_t *)lr.pBits + 384 * lr.Pitch))[512];

        char bmpPath[MAX_PATH];
        char bmpName[64];
        snprintf(bmpName, sizeof(bmpName), "dx9_srcmod_%s.bmp", name);
        GetOutputPath(bmpName, bmpPath, MAX_PATH);
        WriteBMP(bmpPath, lr.pBits, 1024, 768, lr.Pitch);
        offscreen->UnlockRect();

        int r = (pix >> 16) & 0xFF;
        int g = (pix >> 8) & 0xFF;
        int b = pix & 0xFF;
        bool pass = (abs(r - expectR) <= tolerance) &&
                    (abs(g - expectG) <= tolerance) &&
                    (abs(b - expectB) <= tolerance);
        fprintf(stderr, "TEST %s: %s (got r=%d g=%d b=%d, expect %d %d %d ±%d)\n",
                name, pass ? "PASS" : "FAIL", r, g, b, expectR, expectG, expectB, tolerance);

        ps->Release();
        backbuffer->Release();
        offscreen->Release();
        return pass;
    };

    // Test _comp: input white (0xFF,0xFF,0xFF) → 1-1 = 0 → expect (0,0,0)
    bool passComp = testModifier("comp", ps_comp, 0xFFFFFFFF, 0, 0, 0, 5);

    // Test _x2: input ~0.25 (0x40=64/255≈0.251) → 2*0.251 ≈ 0.502 → ~128
    bool passX2 = testModifier("x2", ps_x2, 0xFF404040, 128, 128, 128, 10);

    // Test _bias: input ~0.75 (0xBF=191/255≈0.749) → 0.749-0.5 = 0.249 → ~64
    bool passBias = testModifier("bias", ps_bias, 0xFFBFBFBF, 64, 64, 64, 10);

    bool allPass = passComp && passX2 && passBias;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    device->Release(); d3d9->Release();
    return allPass ? 0 : 1;
}
