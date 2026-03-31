#define WIN_EXPORT
#include "wineunixlib.h"
#include "airconv_thunks.h"

AIRCONV_API int
SM50Initialize(
    const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl,
    sm50_error_t *ppError
) {
  struct sm50_initialize_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.shader = ppShader;
  params.reflection = pRefl;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_initialize, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM50Destroy(sm50_shader_t pShader) {
  struct sm50_destroy_params params;
  params.shader = pShader;
  UNIX_CALL(sm50_destroy, &params);
}

AIRCONV_API int
SM50Compile(
    sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_params params;
  params.shader = (sm50_shader_t)pShader;
  params.args = pArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM50GetCompiledBitcode(sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  struct sm50_get_compiled_bitcode_params params;
  params.bitcode = pBitcode;
  params.data_out = pData;
  UNIX_CALL(sm50_get_compiled_bitcode, &params);
}

AIRCONV_API void
SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  struct sm50_destroy_bitcode_params params;
  params.bitcode = pBitcode;
  UNIX_CALL(sm50_destroy_bitcode, &params);
}

AIRCONV_API size_t
SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  struct sm50_get_error_message_params params;
  params.error = pError;
  params.buffer = pBuffer;
  params.buffer_size = BufferSize;
  params.ret_size = 0;
  UNIX_CALL(sm50_get_error_message, &params);
  return params.ret_size;
}

AIRCONV_API void
SM50FreeError(sm50_error_t pError) {
  struct sm50_free_error_params params;
  params.error = pError;
  UNIX_CALL(sm50_free_error, &params);
}

AIRCONV_API int
SM50CompileTessellationPipelineHull(
    sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode,
    sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_hull_params params;
  params.vertex = pVertexShader;
  params.hull = pHullShader;
  params.hull_args = pHullShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_hull, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileTessellationPipelineDomain(
    sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_domain_params params;
  params.domain = pDomainShader;
  params.hull = pHullShader;
  params.domain_args = pDomainShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_domain, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineVertex(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_vertex_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.vertex_args = pVertexShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_vertex, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineGeometry(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_geometry_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.geometry_args = pGeometryShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_geometry, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  struct sm50_get_arguments_info_params params;
  params.shader = pShader;
  params.constant_buffers = pConstantBuffers;
  params.arguments = pArguments;
  UNIX_CALL(sm50_get_arguments_info, &params);
};

AIRCONV_API int
SM30Initialize(
    const void *pBytecode, size_t BytecodeSize, sm30_shader_t *ppShader,
    sm50_error_t *ppError
) {
  struct sm30_initialize_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.shader = ppShader;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm30_initialize, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM30Destroy(sm30_shader_t pShader) {
  struct sm30_destroy_params params;
  params.shader = pShader;
  UNIX_CALL(sm30_destroy, &params);
}

AIRCONV_API int
SM30Compile(
    sm30_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm30_compile_params params;
  params.shader = (sm30_shader_t)pShader;
  params.args = pArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm30_compile, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API uint32_t
SM30GetInputDeclCount(sm30_shader_t pShader) {
  struct sm30_get_input_decl_count_params params;
  params.shader = pShader;
  params.ret = 0;
  UNIX_CALL(sm30_get_input_decl_count, &params);
  return params.ret;
}

AIRCONV_API void
SM30GetInputDecls(sm30_shader_t pShader, struct SM30_INPUT_DECL *pDecls) {
  struct sm30_get_input_decls_params params;
  params.shader = pShader;
  params.decls = pDecls;
  UNIX_CALL(sm30_get_input_decls, &params);
}

AIRCONV_API uint32_t
SM30GetPSMaxTexcoordCount(sm30_shader_t pShader) {
  struct sm30_get_ps_max_texcoord_count_params params;
  params.shader = pShader;
  params.ret = 0;
  UNIX_CALL(sm30_get_ps_max_texcoord_count, &params);
  return params.ret;
}

AIRCONV_API uint32_t
SM30GetVSHasFogOutput(sm30_shader_t pShader) {
  struct sm30_get_vs_has_fog_output_params params;
  params.shader = pShader;
  params.ret = 0;
  UNIX_CALL(sm30_get_vs_has_fog_output, &params);
  return params.ret;
}

AIRCONV_API uint32_t
SM30GetSamplerDeclCount(sm30_shader_t pShader) {
  struct sm30_get_sampler_decl_count_params params;
  params.shader = pShader;
  params.ret = 0;
  UNIX_CALL(sm30_get_sampler_decl_count, &params);
  return params.ret;
}

AIRCONV_API void
SM30GetSamplerDecls(sm30_shader_t pShader, struct SM30_SAMPLER_DECL *pDecls) {
  struct sm30_get_sampler_decls_params params;
  params.shader = pShader;
  params.decls = pDecls;
  UNIX_CALL(sm30_get_sampler_decls, &params);
}

AIRCONV_API uint32_t
SM30GetMaxConstantRegister(sm30_shader_t pShader) {
  struct sm30_get_max_constant_register_params params;
  params.shader = pShader;
  params.ret = 256;
  UNIX_CALL(sm30_get_max_constant_register, &params);
  return params.ret;
}

AIRCONV_API int
D3D9FFCompileVS(
    const struct D3D9_FF_VS_KEY *pKey,
    const struct D3D9_FF_VS_ELEMENT *pElements,
    uint32_t numElements, uint32_t slotMask,
    const char *FunctionName,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct d3d9_ff_compile_vs_params params;
  params.key = pKey;
  params.elements = pElements;
  params.num_elements = numElements;
  params.slot_mask = slotMask;
  params.func_name = FunctionName;
  params.args = pArgs;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(d3d9_ff_compile_vs, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
D3D9FFCompilePS(
    const struct D3D9_FF_PS_KEY *pKey,
    uint8_t texcoordCount,
    const char *FunctionName,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct d3d9_ff_compile_ps_params params;
  params.key = pKey;
  params.texcoord_count = texcoordCount;
  params.func_name = FunctionName;
  params.args = pArgs;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(d3d9_ff_compile_ps, &params);
  if (status)
    return -1;
  return params.ret;
}