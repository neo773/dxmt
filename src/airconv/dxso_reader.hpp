#pragma once
#include <cstdint>
#include <cstddef>

namespace dxmt::dxso {

// Iterates over DXSO uint32 token stream
class DxsoCodeIter {
public:
  DxsoCodeIter(const uint32_t *ptr) : ptr_(ptr) {}

  uint32_t read() { return *ptr_++; }
  float readFloat() { union { uint32_t u; float f; } v; v.u = *ptr_++; return v.f; }
  const uint32_t *ptr() const { return ptr_; }
  DxsoCodeIter skip(uint32_t count) const { return DxsoCodeIter(ptr_ + count); }

private:
  const uint32_t *ptr_;
};

// Parse version token at the start of DXSO bytecode
struct DxsoProgramInfo {
  uint32_t majorVersion;
  uint32_t minorVersion;
  bool isVertexShader; // true = VS (0xfffe), false = PS (0xffff)
};

inline DxsoProgramInfo DxsoReadProgramInfo(DxsoCodeIter &iter) {
  uint32_t token = iter.read();
  DxsoProgramInfo info;
  info.majorVersion = (token >> 8) & 0xff;
  info.minorVersion = token & 0xff;
  info.isVertexShader = ((token >> 16) & 0xffff) == 0xfffe;
  return info;
}

// Scan for END token to determine bytecode size (in bytes)
inline size_t DxsoFindBytecodeSize(const uint32_t *pFunction) {
  const uint32_t *p = pFunction;
  while ((*p & 0xffff) != 0xffff) // END opcode
    p++;
  p++; // include the END token itself
  return (size_t)((const uint8_t *)p - (const uint8_t *)pFunction);
}

} // namespace dxmt::dxso
