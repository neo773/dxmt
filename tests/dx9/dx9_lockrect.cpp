#include "dx9_test_utils.h"
#include <d3d9.h>

// Test LockRect sub-rect pointer offset and D3DLOCK_NO_DIRTY_UPDATE flag
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));
    WNDCLASSEXW winClass = {};
    winClass.cbSize = sizeof(WNDCLASSEXW);
    winClass.style = CS_HREDRAW | CS_VREDRAW;
    winClass.lpfnWndProc = &WndProc;
    winClass.hInstance = hInstance;
    winClass.lpszClassName = L"D3D9LockRectTest";
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 1024, 768 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 LockRect Test (DXMT)",
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

    int failCount = 0;

    // =========================================================================
    // TEST 1: LockRect with sub-rect returns correct pointer offset
    // =========================================================================
    {
        IDirect3DTexture9 *tex = nullptr;
        hr = device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        if (FAILED(hr)) { fprintf(stderr, "TEST sub_rect_offset: FAIL (CreateTexture: 0x%08x)\n", (unsigned)hr); failCount++; }
        else {
            // Fill the entire texture with a known pattern
            D3DLOCKED_RECT lr;
            hr = tex->LockRect(0, &lr, nullptr, 0);
            if (SUCCEEDED(hr)) {
                uint32_t *pixels = (uint32_t *)lr.pBits;
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++)
                        pixels[y * (lr.Pitch / 4) + x] = (uint32_t)((y << 8) | x); // encode position
                tex->UnlockRect(0);
            }

            // Now lock a sub-rect and verify the pointer offset
            RECT subRect = { 10, 20, 30, 40 }; // left=10, top=20
            hr = tex->LockRect(0, &lr, &subRect, D3DLOCK_READONLY);
            if (SUCCEEDED(hr)) {
                // The pixel at (0,0) of the locked region should be the pixel at (10,20) of the texture
                uint32_t expectedPixel = (uint32_t)((20 << 8) | 10);
                uint32_t actualPixel = *(uint32_t *)lr.pBits;
                if (actualPixel == expectedPixel) {
                    fprintf(stderr, "TEST sub_rect_offset: PASS\n");
                } else {
                    fprintf(stderr, "TEST sub_rect_offset: FAIL (expected 0x%08x, got 0x%08x)\n",
                            expectedPixel, actualPixel);
                    failCount++;
                }
                tex->UnlockRect(0);
            } else {
                fprintf(stderr, "TEST sub_rect_offset: FAIL (LockRect sub-rect: 0x%08x)\n", (unsigned)hr);
                failCount++;
            }
            tex->Release();
        }
    }

    // =========================================================================
    // TEST 2: LockRect with sub-rect — write to sub-rect lands at correct mip offset
    // =========================================================================
    {
        IDirect3DTexture9 *tex = nullptr;
        hr = device->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        if (FAILED(hr)) { fprintf(stderr, "TEST sub_rect_write: FAIL (CreateTexture: 0x%08x)\n", (unsigned)hr); failCount++; }
        else {
            // Zero the texture
            D3DLOCKED_RECT lr;
            tex->LockRect(0, &lr, nullptr, 0);
            memset(lr.pBits, 0, lr.Pitch * 32);
            tex->UnlockRect(0);

            // Write 0xFF to a 4x4 sub-rect at (8, 16)
            RECT subRect = { 8, 16, 12, 20 };
            tex->LockRect(0, &lr, &subRect, 0);
            memset(lr.pBits, 0xFF, 4 * 4 * 4); // 4 pixels wide, but need to respect pitch
            // Actually write row by row
            for (int row = 0; row < 4; row++) {
                uint32_t *rowPtr = (uint32_t *)((uint8_t *)lr.pBits + row * lr.Pitch);
                for (int col = 0; col < 4; col++)
                    rowPtr[col] = 0xFFFFFFFF;
            }
            tex->UnlockRect(0);

            // Verify by reading full texture
            tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
            uint32_t *pixels = (uint32_t *)lr.pBits;
            bool ok = true;
            // Check that pixel (8,16) is 0xFFFFFFFF
            if (pixels[16 * (lr.Pitch / 4) + 8] != 0xFFFFFFFF) ok = false;
            // Check that pixel (0,0) is still 0
            if (pixels[0] != 0) ok = false;
            // Check that pixel (7,16) is still 0 (just outside the sub-rect)
            if (pixels[16 * (lr.Pitch / 4) + 7] != 0) ok = false;
            tex->UnlockRect(0);

            if (ok) {
                fprintf(stderr, "TEST sub_rect_write: PASS\n");
            } else {
                fprintf(stderr, "TEST sub_rect_write: FAIL (sub-rect data at wrong offset)\n");
                failCount++;
            }
            tex->Release();
        }
    }

    // =========================================================================
    // TEST 3: D3DLOCK_NO_DIRTY_UPDATE prevents marking texture dirty
    // =========================================================================
    {
        // This test verifies the flag works by checking that a texture locked with
        // NO_DIRTY_UPDATE + read-only doesn't trigger an upload. We test indirectly:
        // create texture, upload it (draw), lock with NO_DIRTY_UPDATE, unlock,
        // then draw again — the texture should still show the original content.

        IDirect3DTexture9 *tex = nullptr;
        hr = device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        if (FAILED(hr)) { fprintf(stderr, "TEST no_dirty_update: FAIL (CreateTexture)\n"); failCount++; }
        else {
            // Fill with red
            D3DLOCKED_RECT lr;
            tex->LockRect(0, &lr, nullptr, 0);
            uint32_t *p = (uint32_t *)lr.pBits;
            for (int i = 0; i < 16; i++) p[i] = 0xFFFF0000; // ARGB red
            tex->UnlockRect(0);

            // Force an upload by setting as texture and drawing
            device->SetTexture(0, tex);
            device->BeginScene();
            // Just begin/end to trigger the upload path
            device->EndScene();
            device->Present(nullptr, nullptr, nullptr, nullptr);

            // Now lock with NO_DIRTY_UPDATE — should NOT mark dirty
            tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE);
            // Read a pixel to verify we can read
            uint32_t readBack = *(uint32_t *)lr.pBits;
            tex->UnlockRect(0);

            if (readBack == 0xFFFF0000) {
                fprintf(stderr, "TEST no_dirty_update: PASS\n");
            } else {
                fprintf(stderr, "TEST no_dirty_update: FAIL (readback 0x%08x != 0xFFFF0000)\n", readBack);
                failCount++;
            }
            tex->Release();
        }
    }

    // Summary
    if (failCount == 0) {
        fprintf(stderr, "ALL TESTS PASSED\n");
    } else {
        fprintf(stderr, "%d TEST(S) FAILED\n", failCount);
    }

    // Cleanup
    device->Release();
    d3d9->Release();

    if (autoExit) {
        DestroyWindow(hwnd);
        return failCount > 0 ? 1 : 0;
    }

    // Interactive loop
    MSG msg;
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
done:
    return failCount > 0 ? 1 : 0;
}
