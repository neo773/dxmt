#include <d3d9.h>
#include <cstdio>
#include <cstring>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    [[maybe_unused]] bool autoExit = (lpCmdLine && strstr(lpCmdLine, "--auto"));

    bool allPass = true;

    // Test D3DPERF_BeginEvent returns >= 0
    int beginResult = D3DPERF_BeginEvent(D3DCOLOR_XRGB(255, 0, 0), L"TestEvent");
    bool beginOk = (beginResult >= 0);
    fprintf(stderr, "TEST d3dperf_begin_event: %s (ret=%d)\n", beginOk ? "PASS" : "FAIL", beginResult);
    allPass &= beginOk;

    // Test D3DPERF_EndEvent returns >= 0
    int endResult = D3DPERF_EndEvent();
    bool endOk = (endResult >= 0);
    fprintf(stderr, "TEST d3dperf_end_event: %s (ret=%d)\n", endOk ? "PASS" : "FAIL", endResult);
    allPass &= endOk;

    // Test D3DPERF_GetStatus returns 0 (no profiler attached)
    DWORD status = D3DPERF_GetStatus();
    bool statusOk = (status == 0);
    fprintf(stderr, "TEST d3dperf_get_status: %s (ret=%u)\n", statusOk ? "PASS" : "FAIL", (unsigned)status);
    allPass &= statusOk;

    // Test D3DPERF_QueryRepeatFrame returns FALSE
    BOOL repeat = D3DPERF_QueryRepeatFrame();
    bool repeatOk = (repeat == FALSE);
    fprintf(stderr, "TEST d3dperf_query_repeat_frame: %s (ret=%d)\n", repeatOk ? "PASS" : "FAIL", repeat);
    allPass &= repeatOk;

    // Test D3DPERF_SetMarker does not crash
    D3DPERF_SetMarker(D3DCOLOR_XRGB(0, 255, 0), L"TestMarker");
    fprintf(stderr, "TEST d3dperf_set_marker: PASS\n");

    // Test D3DPERF_SetOptions does not crash
    D3DPERF_SetOptions(1);
    fprintf(stderr, "TEST d3dperf_set_options: PASS\n");

    // Test D3DPERF_SetRegion does not crash
    D3DPERF_SetRegion(D3DCOLOR_XRGB(0, 0, 255), L"TestRegion");
    fprintf(stderr, "TEST d3dperf_set_region: PASS\n");

    fprintf(stderr, "\n%s\n", allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return allPass ? 0 : 1;
}
