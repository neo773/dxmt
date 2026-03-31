#include "d3d9_fixed_function.hpp"
#include "airconv_public.h"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include <cstring>
#include <vector>
#include <d3d9.h>

namespace dxmt {

WMT::Reference<WMT::Function> GenerateFFVertexShader(
    WMT::Device device, const FFVSKey &key,
    const D3DVERTEXELEMENT9 *elements, uint32_t numElements, uint32_t slotMask,
    WMTMetalVersion metalVersion) {

  // Build element array for the C API
  std::vector<D3D9_FF_VS_ELEMENT> ffElements;
  for (uint32_t i = 0; i < numElements; i++) {
    auto &elem = elements[i];
    if (elem.Stream == 0xFF) break;

    D3D9_FF_VS_ELEMENT fe = {};
    fe.usage = (uint8_t)elem.Usage;
    fe.usage_index = (uint8_t)elem.UsageIndex;
    fe.type = (uint8_t)elem.Type;
    fe.stream = (uint8_t)elem.Stream;
    fe.offset = (uint16_t)elem.Offset;
    ffElements.push_back(fe);
  }

  auto start_ns = ShaderCache::nowNs();

  // Cache key: FF key + elements layout
  Sha1HashState keyHash;
  keyHash.update(key);
  keyHash.update(ffElements.data(), ffElements.size() * sizeof(D3D9_FF_VS_ELEMENT));
  keyHash.update(slotMask);
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction("shader_main");
      if (function) return function;
    }
  }

  D3D9_FF_VS_KEY cKey;
  static_assert(sizeof(cKey) == sizeof(key));
  memcpy(&cKey, &key, sizeof(cKey));

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t err = nullptr;
  if (D3D9FFCompileVS(&cKey, ffElements.data(), (uint32_t)ffElements.size(),
                       slotMask, "shader_main",
                       (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common,
                       &bitcode, &err)) {
    Logger::err("D3D9: D3D9FFCompileVS failed");
    if (err) SM50FreeError(err);
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for FF vertex shader");
    if (mtl_err) Logger::err(mtl_err.description().getUTF8String());
    return {};
  }

  auto function = library.newFunction("shader_main");
  if (!function) {
    Logger::err("D3D9: Failed to find MTLFunction for FF vertex shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

WMT::Reference<WMT::Function> GenerateFFPixelShader(
    WMT::Device device, const FFPSKey &key,
    uint8_t texcoord_count,
    WMTMetalVersion metalVersion) {

  auto start_ns = ShaderCache::nowNs();

  // Cache key: FF key + texcoord count
  Sha1HashState keyHash;
  keyHash.update(key);
  keyHash.update(texcoord_count);
  auto cacheKey = keyHash.final();

  auto &cache = ShaderCache::getInstance(metalVersion);

  // Check preloaded libraries
  {
    auto library = cache.findPreloadedLibrary(&cacheKey, sizeof(cacheKey));
    if (library) {
      auto function = library.newFunction("shader_main");
      if (function) return function;
    }
  }

  D3D9_FF_PS_KEY cKey;
  static_assert(sizeof(cKey) == sizeof(key));
  memcpy(&cKey, &key, sizeof(cKey));

  SM50_SHADER_COMMON_DATA common;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = (SM50_SHADER_METAL_VERSION)metalVersion;
  common.next = nullptr;

  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t err = nullptr;
  if (D3D9FFCompilePS(&cKey, texcoord_count, "shader_main",
                       (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common,
                       &bitcode, &err)) {
    Logger::err("D3D9: D3D9FFCompilePS failed");
    if (err) SM50FreeError(err);
    return {};
  }

  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);

  auto lib_data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  WMT::Reference<WMT::Error> mtl_err;
  auto library = device.newLibrary(lib_data, mtl_err);

  SM50DestroyBitcode(bitcode);

  if (mtl_err || !library) {
    Logger::err("D3D9: Failed to create MTLLibrary for FF pixel shader");
    if (mtl_err) Logger::err(mtl_err.description().getUTF8String());
    return {};
  }

  auto function = library.newFunction("shader_main");
  if (!function) {
    Logger::err("D3D9: Failed to find MTLFunction for FF pixel shader");
    return {};
  }

  // Store in cache
  {
    auto writer = cache.getWriter();
    if (writer) writer->set(cacheKey, lib_data);
  }
  ShaderCache::recordShader(start_ns);

  return function;
}

} // namespace dxmt
