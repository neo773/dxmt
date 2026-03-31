#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define UNICODE
#include <windows.h>

static inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch(msg) {
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return 0;
}

static inline bool WriteBMP(const char *path, const void *pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t rowSize = (width * 3 + 3) & ~3u;
    uint32_t imageSize = rowSize * height;
    uint32_t fileSize = 54 + imageSize;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    memcpy(&header[2], &fileSize, 4);
    uint32_t dataOffset = 54; memcpy(&header[10], &dataOffset, 4);
    uint32_t dibSize = 40; memcpy(&header[14], &dibSize, 4);
    int32_t w = (int32_t)width, h = (int32_t)height;
    memcpy(&header[18], &w, 4); memcpy(&header[22], &h, 4);
    uint16_t planes = 1; memcpy(&header[26], &planes, 2);
    uint16_t bpp = 24; memcpy(&header[28], &bpp, 2);
    memcpy(&header[34], &imageSize, 4);
    fwrite(header, 1, 54, f);
    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t *row = new uint8_t[rowSize];
    for (int y = (int)height - 1; y >= 0; y--) {
        const uint8_t *srcRow = src + y * pitch;
        for (uint32_t x = 0; x < width; x++) {
            row[x*3+0] = srcRow[x*4+0];
            row[x*3+1] = srcRow[x*4+1];
            row[x*3+2] = srcRow[x*4+2];
        }
        for (uint32_t p = width*3; p < rowSize; p++) row[p] = 0;
        fwrite(row, 1, rowSize, f);
    }
    delete[] row;
    fclose(f);
    return true;
}

static inline bool GetOutputPath(const char *filename, char *out, size_t outLen) {
    WCHAR exePath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    WCHAR *last = wcsrchr(exePath, L'\\');
    if (!last) last = wcsrchr(exePath, L'/');
    if (last) last[1] = L'\0'; else exePath[0] = L'\0';
    char narrowDir[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, narrowDir, MAX_PATH, NULL, NULL);
    snprintf(out, outLen, "%s%s", narrowDir, filename);
    return true;
}
