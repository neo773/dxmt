// Standalone version of DX SDK Tutorial 05: Textures
// No d3dx9 or CRT dependency — loads banana.bmp via Win32 API, own matrix math
#include <windows.h>
#include <d3d9.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---- CRT replacements ----
extern "C" {
  void *memset(void *s, int c, unsigned int n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
  }
  void *memcpy(void *d, const void *s, unsigned int n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
  }
  // math intrinsics — mingw links these from libgcc
  float sinf(float);
  float cosf(float);
  float tanf(float);
  float sqrtf(float);
}

static void dbg(const char *msg) {
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD w;
    WriteFile(h, msg, lstrlenA(msg), &w, NULL);
    WriteFile(h, "\n", 1, &w, NULL);
  }
}

// ---- Minimal matrix math ----
struct Mat4 { float m[4][4]; };

static Mat4 mat4_identity() {
  Mat4 r; memset(&r, 0, sizeof(r));
  r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
  return r;
}

static Mat4 mat4_rotation_x(float angle) {
  Mat4 r = mat4_identity();
  float c = cosf(angle), s = sinf(angle);
  r.m[1][1] = c;  r.m[1][2] = s;
  r.m[2][1] = -s; r.m[2][2] = c;
  return r;
}

// D3D9 uses row-vector convention: pos_out = pos * M
// Translation goes in row 3 (m[3][0..2]), not column 3
static Mat4 mat4_lookat_lh(float eyeX, float eyeY, float eyeZ,
                            float atX, float atY, float atZ,
                            float upX, float upY, float upZ) {
  // zaxis = normalize(at - eye)
  float zx = atX-eyeX, zy = atY-eyeY, zz = atZ-eyeZ;
  float zl = sqrtf(zx*zx + zy*zy + zz*zz);
  zx /= zl; zy /= zl; zz /= zl;
  // xaxis = normalize(cross(up, zaxis))
  float xx = upY*zz - upZ*zy, xy = upZ*zx - upX*zz, xz = upX*zy - upY*zx;
  float xl = sqrtf(xx*xx + xy*xy + xz*xz);
  xx /= xl; xy /= xl; xz /= xl;
  // yaxis = cross(zaxis, xaxis)
  float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
  Mat4 m; memset(&m, 0, sizeof(m));
  m.m[0][0] = xx; m.m[0][1] = yx; m.m[0][2] = zx;
  m.m[1][0] = xy; m.m[1][1] = yy; m.m[1][2] = zy;
  m.m[2][0] = xz; m.m[2][1] = yz; m.m[2][2] = zz;
  m.m[3][0] = -(xx*eyeX + xy*eyeY + xz*eyeZ);
  m.m[3][1] = -(yx*eyeX + yy*eyeY + yz*eyeZ);
  m.m[3][2] = -(zx*eyeX + zy*eyeY + zz*eyeZ);
  m.m[3][3] = 1.0f;
  return m;
}

static Mat4 mat4_perspective_fov_lh(float fovY, float aspect, float zn, float zf) {
  float h = 1.0f / tanf(fovY * 0.5f);
  float w = h / aspect;
  Mat4 m; memset(&m, 0, sizeof(m));
  m.m[0][0] = w;
  m.m[1][1] = h;
  m.m[2][2] = zf / (zf - zn);
  m.m[2][3] = 1.0f;
  m.m[3][2] = -zn * zf / (zf - zn);
  return m;
}

// ---- BMP loader using Win32 API ----
static IDirect3DTexture9 *LoadBMP(IDirect3DDevice9 *dev, const char *path) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;

  unsigned char header[54];
  DWORD bytesRead;
  ReadFile(f, header, 54, &bytesRead, NULL);
  if (bytesRead < 54) { CloseHandle(f); return NULL; }

  int width  = *(int *)&header[18];
  int height = *(int *)&header[22];
  int bpp    = *(short *)&header[28];
  int offset = *(int *)&header[10];

  dbg("BMP loaded");
  if (bpp != 24 && bpp != 32) { CloseHandle(f); return NULL; }

  int absH = height < 0 ? -height : height;
  int rowBytes = ((width * (bpp / 8) + 3) & ~3);
  DWORD pixSize = rowBytes * absH;
  unsigned char *pixels = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, pixSize);

  SetFilePointer(f, offset, NULL, FILE_BEGIN);
  ReadFile(f, pixels, pixSize, &bytesRead, NULL);
  CloseHandle(f);

  IDirect3DTexture9 *tex = NULL;
  if (FAILED(dev->CreateTexture(width, absH, 1, 0, D3DFMT_A8R8G8B8,
                                 D3DPOOL_MANAGED, &tex, NULL))) {
    HeapFree(GetProcessHeap(), 0, pixels);
    return NULL;
  }

  D3DLOCKED_RECT lr;
  tex->LockRect(0, &lr, NULL, 0);
  for (int y = 0; y < absH; y++) {
    int srcRow = (height > 0) ? (absH - 1 - y) : y;
    unsigned char *src = pixels + srcRow * rowBytes;
    unsigned int *dst = (unsigned int *)((unsigned char *)lr.pBits + y * lr.Pitch);
    for (int x = 0; x < width; x++) {
      if (bpp == 24)
        dst[x] = 0xFF000000u | ((unsigned int)src[x*3+2] << 16) |
                 ((unsigned int)src[x*3+1] << 8) | src[x*3];
      else
        dst[x] = ((unsigned int)src[x*4+3] << 24) | ((unsigned int)src[x*4+2] << 16) |
                 ((unsigned int)src[x*4+1] << 8) | src[x*4];
    }
  }
  tex->UnlockRect(0);
  HeapFree(GetProcessHeap(), 0, pixels);
  return tex;
}

// ---- Globals ----
struct Vertex { float x, y, z; DWORD color; float tu, tv; };
#define D3DFVF_CUSTOM (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define NUM_SEGMENTS 50

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
  DWORD startTime = GetTickCount();

  WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"BananaTex";
  wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(0, L"BananaTex", L"DX9 Tutorial 05: Textures (DXMT)",
    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 3456, 2234, 0, 0, hInst, 0);

  IDirect3D9 *pD3D = Direct3DCreate9(D3D_SDK_VERSION);
  if (!pD3D) { dbg("FATAL: Direct3DCreate9"); return 1; }

  D3DPRESENT_PARAMETERS pp; memset(&pp, 0, sizeof(pp));
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  if (FAILED(pD3D->CreateDevice(0, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
    dbg("FATAL: CreateDevice"); return 1;
  }

  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_ZENABLE, TRUE);

  IDirect3DTexture9 *tex = LoadBMP(dev, "C:\\banana.bmp");
  if (!tex) { dbg("FATAL: banana.bmp not found"); return 1; }

  IDirect3DVertexBuffer9 *vb = NULL;
  dev->CreateVertexBuffer(NUM_SEGMENTS * 2 * sizeof(Vertex), 0,
    D3DFVF_CUSTOM, D3DPOOL_DEFAULT, &vb, NULL);
  Vertex *pv;
  vb->Lock(0, 0, (void **)&pv, 0);
  for (int i = 0; i < NUM_SEGMENTS; i++) {
    float theta = (2.0f * M_PI * i) / (NUM_SEGMENTS - 1);
    float u = (float)i / (NUM_SEGMENTS - 1);
    pv[2*i].x = sinf(theta); pv[2*i].y = -1.0f; pv[2*i].z = cosf(theta);
    pv[2*i].color = 0xFFFFFFFF; pv[2*i].tu = u; pv[2*i].tv = 1.0f;
    pv[2*i+1].x = sinf(theta); pv[2*i+1].y = 1.0f; pv[2*i+1].z = cosf(theta);
    pv[2*i+1].color = 0xFF808080; pv[2*i+1].tu = u; pv[2*i+1].tv = 0.0f;
  }
  vb->Unlock();
  dbg("INFO: ready");

  MSG msg; memset(&msg, 0, sizeof(msg));
  while (msg.message != WM_QUIT) {
    if (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg); DispatchMessageW(&msg);
      continue;
    }

    float t = (GetTickCount() - startTime) / 1000.0f;
    Mat4 world = mat4_rotation_x(t);
    Mat4 view = mat4_lookat_lh(0, 30, -60, 0, 0, 0, 0, 1, 0);
    Mat4 proj = mat4_perspective_fov_lh(M_PI / 4.0f, 3456.0f / 2234.0f, 1.0f, 100.0f);

    dev->SetTransform(D3DTS_WORLD, (D3DMATRIX *)&world);
    dev->SetTransform(D3DTS_VIEW, (D3DMATRIX *)&view);
    dev->SetTransform(D3DTS_PROJECTION, (D3DMATRIX *)&proj);

    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
      D3DCOLOR_XRGB(0, 0, 128), 1.0f, 0);
    dev->BeginScene();

    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    dev->SetStreamSource(0, vb, 0, sizeof(Vertex));
    dev->SetFVF(D3DFVF_CUSTOM);

    // Draw 5000 cylinders in a 100x50 grid
    for (int ci = 0; ci < 5000; ci++) {
      float col = (float)(ci % 100) - 50.0f;
      float row = (float)(ci / 100) - 25.0f;
      Mat4 rot = mat4_rotation_x(t + ci * 0.1f);
      Mat4 scale = mat4_identity();
      scale.m[0][0] = 0.35f; scale.m[1][1] = 0.35f; scale.m[2][2] = 0.35f;
      Mat4 translate = mat4_identity();
      translate.m[3][0] = col * 1.8f;
      translate.m[3][2] = row * 1.8f;
      // world = scale * rot * translate
      Mat4 sr; memset(&sr, 0, sizeof(sr));
      for (int i=0;i<4;i++) for (int j=0;j<4;j++) for (int k=0;k<4;k++) sr.m[i][j] += scale.m[i][k]*rot.m[k][j];
      Mat4 w2; memset(&w2, 0, sizeof(w2));
      for (int i=0;i<4;i++) for (int j=0;j<4;j++) for (int k=0;k<4;k++) w2.m[i][j] += sr.m[i][k]*translate.m[k][j];
      dev->SetTransform(D3DTS_WORLD, (D3DMATRIX *)&w2);
      dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2 * NUM_SEGMENTS - 2);
    }

    dev->EndScene();
    dev->Present(NULL, NULL, NULL, NULL);
  }

  tex->Release(); vb->Release(); dev->Release(); pD3D->Release();
  return 0;
}
