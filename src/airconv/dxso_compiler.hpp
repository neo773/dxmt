#pragma once
#include "dxso_decoder.hpp"
#include "airconv_public.h"
#include <cstdint>
#include <vector>

namespace dxmt::dxso {

// Internal representation stored by SM30Initialize
struct DxsoShaderInternal {
  DxsoProgramInfo info;

  // Parsed DCL semantics for input registers (VS only)
  struct InputDecl {
    uint32_t reg;
    DxsoUsage usage;
    uint32_t usageIndex;
  };
  std::vector<InputDecl> inputDecls;

  // Parsed DCL semantics for output registers (VS only, SM3)
  struct OutputDecl {
    uint32_t reg;
    DxsoUsage usage;
    uint32_t usageIndex;
  };
  std::vector<OutputDecl> outputDecls;

  // Parsed DCL semantics for PS inputs
  struct PSInputDecl {
    uint32_t reg;
    DxsoUsage usage;
    uint32_t usageIndex;
  };
  std::vector<PSInputDecl> psInputDecls;

  // Parsed sampler declarations (PS only)
  struct SamplerDecl {
    uint32_t reg;
    DxsoTextureType textureType;
  };
  std::vector<SamplerDecl> samplerDecls;

  // Maximum float constant register index referenced (exclusive), or 256 if dynamic indexing
  uint32_t maxConstantReg = 0;
  bool hasRelativeConst = false;

  // Raw bytecode (without version token)
  std::vector<uint32_t> tokens;
  // Full bytecode including version token
  std::vector<uint32_t> fullTokens;
};

} // namespace dxmt::dxso
