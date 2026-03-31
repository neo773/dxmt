#include "dx9_test_utils.h"
#include <d3d9.h>

// Test A8L8 texture format: create, lock, fill, sample via FF PS, readback
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
    winClass.lpszClassName = L"D3D9A8L8TestClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 A8L8 Test (DXMT)",
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
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08x\n", (unsigned)hr); return 1; }

    // Create A8L8 texture (2 bytes per pixel: L in low byte, A in high byte)
    IDirect3DTexture9 *tex = nullptr;
    hr = device->CreateTexture(4, 4, 1, 0, D3DFMT_A8L8, D3DPOOL_MANAGED, &tex, nullptr);
    fprintf(stderr, "CreateTexture A8L8: hr=0x%08x\n", (unsigned)hr);
    if (FAILED(hr)) {
        fprintf(stderr, "\nSOME TESTS FAILED\n");
        device->Release(); d3d9->Release(); return 1;
    }

    // Fill texture: L=200, A=128 for all texels
    D3DLOCKED_RECT lr;
    hr = tex->LockRect(0, &lr, nullptr, 0);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < 4; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < 4; x++) {
                // A8L8 format: low byte = luminance, high byte = alpha
                row[x] = (128 << 8) | 200;
            }
        }
        tex->UnlockRect(0);
    }

    // Also test GetSurfaceLevel while we're at it
    IDirect3DSurface9 *surfLevel = nullptr;
    hr = tex->GetSurfaceLevel(0, &surfLevel);
    fprintf(stderr, "GetSurfaceLevel: hr=0x%08x\n", (unsigned)hr);
    bool getSurfPass = SUCCEEDED(hr);
    if (surfLevel) {
        D3DSURFACE_DESC desc;
        surfLevel->GetDesc(&desc);
        fprintf(stderr, "  Surface desc: %ux%u format=%d\n", desc.Width, desc.Height, desc.Format);
        getSurfPass = getSurfPass && (desc.Width == 4 && desc.Height == 4);
        surfLevel->Release();
    }

    // Set up rendering: fullscreen quad with the A8L8 texture
    DWORD fvf = D3DFVF_XYZ | D3DFVF_TEX1;
    struct Vertex { float x, y, z; float u, v; };
    Vertex verts[] = {
        { -1.0f,  1.0f, 0.5f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.5f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f },
        {  1.0f,  1.0f, 0.5f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.5f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.5f, 0.0f, 1.0f },
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

    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    device->BeginScene();
    device->SetFVF(fvf);
    device->SetStreamSource(0, vb, 0, sizeof(Vertex));
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetTexture(0, tex);
    // FF PS: COLOROP=SELECTARG1, COLORARG1=TEXTURE (default stage 0)
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    device->SetTexture(0, nullptr);
    device->EndScene();

    // Readback
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    IDirect3DSurface9 *offscreen = nullptr;
    device->CreateOffscreenPlainSurface(1024, 768, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
    device->GetRenderTargetData(backbuffer, offscreen);

    D3DLOCKED_RECT readLr;
    offscreen->LockRect(&readLr, nullptr, D3DLOCK_READONLY);
    uint32_t centerPix = ((uint32_t *)((uint8_t *)readLr.pBits + 384 * readLr.Pitch))[512];
    uint8_t r = (centerPix >> 16) & 0xFF;
    uint8_t g = (centerPix >> 8) & 0xFF;
    uint8_t b = centerPix & 0xFF;

    char bmpPath[MAX_PATH];
    GetOutputPath("dx9_a8l8.bmp", bmpPath, MAX_PATH);
    WriteBMP(bmpPath, readLr.pBits, 1024, 768, readLr.Pitch);
    offscreen->UnlockRect();

    // A8L8 with .rrrg swizzle: L=200 replicated to RGB, A=128 in alpha channel.
    // FF PS SELECTARG1=TEXTURE outputs the swizzled value to the render target.
    // Render target is X8R8G8B8: R~200, G~200, B~200.
    bool texCreated = tex != nullptr;
    bool swizzleCorrect = (r >= 195 && r <= 205) && (g >= 195 && g <= 205) && (b >= 195 && b <= 205);

    fprintf(stderr, "TEST a8l8_create: %s\n", texCreated ? "PASS" : "FAIL");
    fprintf(stderr, "TEST a8l8_get_surface: %s\n", getSurfPass ? "PASS" : "FAIL");
    fprintf(stderr, "TEST a8l8_swizzle: %s (center=0x%08x r=%d g=%d b=%d, expect ~200)\n",
            swizzleCorrect ? "PASS" : "FAIL", centerPix, r, g, b);

    bool allPass = texCreated && getSurfPass && swizzleCorrect;
    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    tex->Release(); vb->Release();
    backbuffer->Release(); offscreen->Release();
    device->Release(); d3d9->Release();
    return allPass ? 0 : 1;
}
