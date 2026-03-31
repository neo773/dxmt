#pragma once
#include "dxso_enums.hpp"
#include "dxso_tables.hpp"
#include "dxso_reader.hpp"
#include <array>
#include <cstdint>

namespace dxmt::dxso {

struct DxsoRegisterId {
  DxsoRegisterType type;
  uint32_t num;
};

struct DxsoRegMask {
  uint8_t mask; // bits 0-3 = xyzw
  DxsoRegMask() : mask(0xf) {}
  explicit DxsoRegMask(uint8_t m) : mask(m & 0xf) {}
  bool operator[](uint32_t i) const { return (mask >> i) & 1; }
};

struct DxsoRegSwizzle {
  uint8_t data; // 2 bits per component
  DxsoRegSwizzle() : data(0xe4) {} // identity xyzw
  explicit DxsoRegSwizzle(uint8_t d) : data(d) {}
  uint32_t operator[](uint32_t i) const { return (data >> (i * 2)) & 3; }
};

struct DxsoSemantic {
  DxsoUsage usage;
  uint32_t usageIndex;
};

struct DxsoDeclaration {
  DxsoSemantic semantic;
  DxsoTextureType textureType;
};

struct DxsoDefinition {
  union {
    float float32[4];
    int32_t int32[4];
    uint32_t uint32[4];
  };
};

struct DxsoRegister {
  DxsoRegisterId id;
  DxsoRegMask mask;
  DxsoRegSwizzle swizzle;
  DxsoRegModifier modifier;
  bool saturate;
  bool hasRelative = false;
  DxsoRegisterId relativeId = {};
  DxsoRegSwizzle relativeSwizzle = {};
};

struct DxsoInstruction {
  DxsoOpcode opcode;
  uint32_t tokenLength;
};

constexpr uint32_t DxsoMaxSrcRegs = 4;

struct DxsoInstructionContext {
  DxsoInstruction instruction;
  DxsoRegister dst;
  std::array<DxsoRegister, DxsoMaxSrcRegs> src;
  DxsoDeclaration dcl;
  DxsoDefinition def;
};

// Decode DXSO bytecode instructions one at a time
class DxsoDecoder {
public:
  DxsoDecoder(const uint32_t *bytecode)
    : iter_(bytecode) {
    info_ = DxsoReadProgramInfo(iter_);
  }

  const DxsoProgramInfo &programInfo() const { return info_; }

  // Decode next instruction. Returns false when END is reached.
  bool decodeInstruction(DxsoInstructionContext &ctx);

private:
  void decodeRegister(DxsoRegister &reg, uint32_t token, bool isDst);
  uint32_t decodeInstructionLength(uint32_t token, DxsoOpcode opcode);

  DxsoCodeIter iter_;
  DxsoProgramInfo info_;
};

inline uint32_t DxsoDecoder::decodeInstructionLength(uint32_t token, DxsoOpcode opcode) {
  if (opcode == DxsoOpcode::Comment)
    return (token & 0x7fff0000) >> 16;
  if (opcode == DxsoOpcode::End)
    return 0;
  if (opcode != DxsoOpcode::Phase) {
    if (info_.majorVersion >= 2)
      return (token & 0x0f000000) >> 24;
    else {
      uint32_t len = DxsoGetDefaultOpcodeLength(opcode);
      // SM 1.4 has an extra param on Tex and TexCoord
      if (info_.majorVersion == 1 && info_.minorVersion == 4) {
        if (opcode == DxsoOpcode::TexCoord || opcode == DxsoOpcode::Tex)
          len += 1;
      }
      return len;
    }
  }
  return 0;
}

inline void DxsoDecoder::decodeRegister(DxsoRegister &reg, uint32_t token, bool isDst) {
  reg.id.type = static_cast<DxsoRegisterType>(
    ((token & 0x00001800) >> 8) | ((token & 0x70000000) >> 28));
  reg.id.num = token & 0x000007ff;
  reg.modifier = DxsoRegModifier::None;
  reg.saturate = false;
  reg.hasRelative = false;

  if (isDst) {
    reg.mask = DxsoRegMask(uint8_t((token & 0x000f0000) >> 16));
    reg.saturate = (token & (1 << 20)) != 0;
    reg.swizzle = DxsoRegSwizzle(0xe4); // identity for dst
  } else {
    reg.swizzle = DxsoRegSwizzle(uint8_t((token & 0x00ff0000) >> 16));
    reg.modifier = static_cast<DxsoRegModifier>((token & 0x0f000000) >> 24);
    reg.mask = DxsoRegMask(0xf); // all components for src
    reg.hasRelative = (token & (1 << 13)) != 0;
  }
}

inline bool DxsoDecoder::decodeInstruction(DxsoInstructionContext &ctx) {
  uint32_t token = iter_.read();

  ctx.instruction.opcode = static_cast<DxsoOpcode>(token & 0x0000ffff);
  ctx.instruction.tokenLength = decodeInstructionLength(token, ctx.instruction.opcode);

  if (ctx.instruction.opcode == DxsoOpcode::End)
    return false;

  if (ctx.instruction.opcode == DxsoOpcode::Comment) {
    iter_ = iter_.skip(ctx.instruction.tokenLength);
    return true;
  }

  uint32_t remaining = ctx.instruction.tokenLength;

  switch (ctx.instruction.opcode) {
  case DxsoOpcode::Dcl: {
    // DCL token + destination register
    uint32_t dclToken = iter_.read();
    ctx.dcl.textureType = static_cast<DxsoTextureType>((dclToken & 0x78000000) >> 27);
    ctx.dcl.semantic.usage = static_cast<DxsoUsage>(dclToken & 0x0000000f);
    ctx.dcl.semantic.usageIndex = (dclToken & 0x000f0000) >> 16;
    decodeRegister(ctx.dst, iter_.read(), true);
    return true;
  }
  case DxsoOpcode::Def: {
    // Destination register + 4 float constants
    decodeRegister(ctx.dst, iter_.read(), true);
    for (uint32_t i = 0; i < 4; i++)
      ctx.def.uint32[i] = iter_.read();
    return true;
  }
  case DxsoOpcode::DefI: {
    decodeRegister(ctx.dst, iter_.read(), true);
    for (uint32_t i = 0; i < 4; i++)
      ctx.def.int32[i] = (int32_t)iter_.read();
    return true;
  }
  case DxsoOpcode::DefB: {
    decodeRegister(ctx.dst, iter_.read(), true);
    ctx.def.uint32[0] = iter_.read();
    return true;
  }
  default: {
    // Generic: first token is dst, rest are src
    uint32_t srcIdx = 0;
    for (uint32_t i = 0; i < remaining; i++) {
      if (i == 0) {
        decodeRegister(ctx.dst, iter_.read(), true);
      } else {
        if (srcIdx < DxsoMaxSrcRegs) {
          decodeRegister(ctx.src[srcIdx], iter_.read(), false);
          if (ctx.src[srcIdx].hasRelative) {
            if (info_.majorVersion >= 2) {
              // SM 2.0+: relative addressing token follows
              uint32_t relToken = iter_.read();
              i++; // consumed an extra token
              ctx.src[srcIdx].relativeId.type = static_cast<DxsoRegisterType>(
                ((relToken & 0x00001800) >> 8) | ((relToken & 0x70000000) >> 28));
              ctx.src[srcIdx].relativeId.num = relToken & 0x000007ff;
              ctx.src[srcIdx].relativeSwizzle = DxsoRegSwizzle(uint8_t((relToken & 0x00ff0000) >> 16));
            } else {
              // SM 1.x: implicit a0.x
              ctx.src[srcIdx].relativeId = {DxsoRegisterType::Addr, 0};
              ctx.src[srcIdx].relativeSwizzle = DxsoRegSwizzle(0x00); // .x = xxxx
            }
          }
          srcIdx++;
        } else {
          iter_.read(); // skip
        }
      }
    }
    return true;
  }
  }
}

} // namespace dxmt::dxso
