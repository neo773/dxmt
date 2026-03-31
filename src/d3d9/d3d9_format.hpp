#pragma once
#include "winemetal.h"
#include <d3d9.h>

namespace dxmt {

inline WMTPixelFormat ConvertD3D9Format(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    return WMTPixelFormatBGRA8Unorm;
  case D3DFMT_DXT1:
    return WMTPixelFormatBC1_RGBA;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    return WMTPixelFormatBC2_RGBA;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return WMTPixelFormatBC3_RGBA;
  case D3DFMT_A8:
    return WMTPixelFormatA8Unorm;
  case D3DFMT_L8:
    return WMTPixelFormatR8Unorm;
  case D3DFMT_A8L8:
    // HACK(wow-launch): A8L8 → RG8 needs shader swizzle .rrrg to remap
    // L→R (replicate to RGB) and A→G (to alpha). Currently sampled as-is.
    return WMTPixelFormatRG8Unorm;
  case D3DFMT_R5G6B5:
    return WMTPixelFormatB5G6R5Unorm;
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
    return WMTPixelFormatBGR5A1Unorm;
  case D3DFMT_A4R4G4B4:
    // Emulate via BGRA8: Metal's ABGR4Unorm swaps R↔B vs D3D9's ARGB layout
    // and produces incorrect samples for zero-RGB pixels on Apple Silicon.
    // Data is expanded from 4-bit to 8-bit channels during texture upload.
    return WMTPixelFormatBGRA8Unorm;
  case D3DFMT_V8U8:
    return WMTPixelFormatRG8Snorm;
  case D3DFMT_R32F:
    return WMTPixelFormatR32Float;
  case D3DFMT_A16B16G16R16F:
    return WMTPixelFormatRGBA16Float;
  case D3DFMT_G16R16F:
    return WMTPixelFormatRG16Float;
  case D3DFMT_R16F:
    return WMTPixelFormatR16Float;
  case D3DFMT_A32B32G32R32F:
    return WMTPixelFormatRGBA32Float;
  case D3DFMT_G16R16:
    return WMTPixelFormatRG16Unorm;
  case D3DFMT_A2B10G10R10:
    return WMTPixelFormatRGB10A2Unorm;
  default:
    return WMTPixelFormatInvalid;
  }
}

inline bool IsCompressedFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return true;
  default:
    return false;
  }
}

inline uint32_t D3D9FormatBlockSize(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_DXT1:
    return 8; // 8 bytes per 4x4 block
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return 16; // 16 bytes per 4x4 block
  default:
    return 0;
  }
}

inline uint32_t D3D9FormatBytesPerPixel(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    return 4;
  case D3DFMT_R5G6B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A4R4G4B4:
    return 2;
  case D3DFMT_A8L8:
  case D3DFMT_V8U8:
    return 2;
  case D3DFMT_A8:
  case D3DFMT_L8:
    return 1;
  case D3DFMT_R32F:
    return 4;
  case D3DFMT_A16B16G16R16F:
    return 8;
  case D3DFMT_G16R16F:
    return 4;
  case D3DFMT_R16F:
    return 2;
  case D3DFMT_A32B32G32R32F:
    return 16;
  case D3DFMT_G16R16:
    return 4;
  case D3DFMT_A2B10G10R10:
    return 4;
  default:
    return 0;
  }
}

// Compute pitch for a given format and width
inline uint32_t D3D9FormatPitch(D3DFORMAT format, uint32_t width) {
  if (IsCompressedFormat(format)) {
    uint32_t blocksWide = (width + 3) / 4;
    return blocksWide * D3D9FormatBlockSize(format);
  }
  return width * D3D9FormatBytesPerPixel(format);
}

// Compute total data size for a mip level
inline uint32_t D3D9FormatMipSize(D3DFORMAT format, uint32_t width, uint32_t height) {
  if (IsCompressedFormat(format)) {
    uint32_t blocksWide = (width + 3) / 4;
    uint32_t blocksHigh = (height + 3) / 4;
    return blocksWide * blocksHigh * D3D9FormatBlockSize(format);
  }
  return width * height * D3D9FormatBytesPerPixel(format);
}

inline const char *D3D9FormatName(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:       return "A8R8G8B8";
  case D3DFMT_X8R8G8B8:       return "X8R8G8B8";
  case D3DFMT_R5G6B5:         return "R5G6B5";
  case D3DFMT_A1R5G5B5:       return "A1R5G5B5";
  case D3DFMT_X1R5G5B5:       return "X1R5G5B5";
  case D3DFMT_A4R4G4B4:       return "A4R4G4B4";
  case D3DFMT_A8:             return "A8";
  case D3DFMT_L8:             return "L8";
  case D3DFMT_A8L8:           return "A8L8";
  case D3DFMT_V8U8:           return "V8U8";
  case D3DFMT_DXT1:           return "DXT1";
  case D3DFMT_DXT2:           return "DXT2";
  case D3DFMT_DXT3:           return "DXT3";
  case D3DFMT_DXT4:           return "DXT4";
  case D3DFMT_DXT5:           return "DXT5";
  case D3DFMT_R32F:           return "R32F";
  case D3DFMT_R16F:           return "R16F";
  case D3DFMT_G16R16F:        return "G16R16F";
  case D3DFMT_A16B16G16R16F:  return "A16B16G16R16F";
  case D3DFMT_A32B32G32R32F:  return "A32B32G32R32F";
  case D3DFMT_G16R16:         return "G16R16";
  case D3DFMT_A2B10G10R10:    return "A2B10G10R10";
  case D3DFMT_D16:            return "D16";
  case D3DFMT_D16_LOCKABLE:   return "D16_LOCKABLE";
  case D3DFMT_D24S8:          return "D24S8";
  case D3DFMT_D24X8:          return "D24X8";
  case D3DFMT_D24X4S4:        return "D24X4S4";
  case D3DFMT_D24FS8:         return "D24FS8";
  case D3DFMT_D32:            return "D32";
  case D3DFMT_D32F_LOCKABLE:  return "D32F_LOCKABLE";
  default:                    return "Unknown";
  }
}

} // namespace dxmt
