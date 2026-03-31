#include "dx9_test_utils.h"
#include <d3d9.h>

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
    winClass.lpszClassName = L"D3D9QueryWindowClass";
    winClass.hIconSm = LoadIconW(0, IDI_APPLICATION);
    RegisterClassExW(&winClass);

    RECT wr = { 0, 0, 256, 256 };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
    HWND hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, winClass.lpszClassName,
                                L"D3D9 Query Test (DXMT)",
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
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &pp, &device);
    if (FAILED(hr)) { fprintf(stderr, "FATAL: CreateDevice failed 0x%08x\n", (unsigned)hr); return 1; }

    bool pass = true;

    // Test 1: CreateQuery with EVENT should succeed
    IDirect3DQuery9 *eventQuery = nullptr;
    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &eventQuery);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: CreateQuery(EVENT) returned 0x%08x\n", (unsigned)hr);
        pass = false;
    } else {
        fprintf(stderr, "TEST create_event_query: PASS\n");
    }

    // Test 2: Issue + GetData should succeed
    if (eventQuery) {
        hr = eventQuery->Issue(D3DISSUE_END);
        if (FAILED(hr)) {
            fprintf(stderr, "FAIL: Issue() returned 0x%08x\n", (unsigned)hr);
            pass = false;
        }

        BOOL result = FALSE;
        hr = eventQuery->GetData(&result, sizeof(result), 0);
        if (FAILED(hr)) {
            fprintf(stderr, "FAIL: GetData() returned 0x%08x\n", (unsigned)hr);
            pass = false;
        } else if (result != TRUE) {
            fprintf(stderr, "FAIL: GetData() returned result=%d, expected TRUE\n", result);
            pass = false;
        } else {
            fprintf(stderr, "TEST event_query_getdata: PASS\n");
        }

        eventQuery->Release();
    }

    // Test 3: Check support query (null ppQuery) should return S_OK for EVENT
    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, nullptr);
    if (hr != S_OK) {
        fprintf(stderr, "FAIL: CreateQuery(EVENT, null) returned 0x%08x\n", (unsigned)hr);
        pass = false;
    } else {
        fprintf(stderr, "TEST event_query_support: PASS\n");
    }

    // Test 4: Unsupported query type should return D3DERR_NOTAVAILABLE
    hr = device->CreateQuery(D3DQUERYTYPE_VCACHE, nullptr);
    if (hr != D3DERR_NOTAVAILABLE) {
        fprintf(stderr, "FAIL: CreateQuery(VCACHE) returned 0x%08x, expected D3DERR_NOTAVAILABLE\n", (unsigned)hr);
        pass = false;
    } else {
        fprintf(stderr, "TEST unsupported_query: PASS\n");
    }

    fprintf(stderr, "\n%s\n", pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    device->Release();
    d3d9->Release();
    return pass ? 0 : 1;
}
