#include "stddef.h"
#include "airconv_public.h"

#ifndef __WINEMETAL_AIRCONV_THUNKS_H
#define __WINEMETAL_AIRCONV_THUNKS_H

enum airconv_unixcalls {
  unix_sm50_initialize = 74,
  unix_sm50_destroy,
  unix_sm50_compile,
  unix_sm50_get_compiled_bitcode,
  unix_sm50_destroy_bitcode,
  unix_sm50_get_error_message,
  unix_sm50_free_error,
  unix_sm50_compile_geometry_pipeline_vertex,
  unix_sm50_compile_geometry_pipeline_geometry,
  unix_sm50_compile_tessellation_vertex,
  unix_sm50_compile_tessellation_hull,
  unix_sm50_compile_tessellation_domain,
  unix_sm50_get_arguments_info = 88,
  unix_sm30_initialize = 131,
  unix_sm30_destroy,
  unix_sm30_compile,
  unix_d3d9_ff_compile_vs,
  unix_d3d9_ff_compile_ps,
  unix_sm30_get_input_decl_count,
  unix_sm30_get_input_decls,
  unix_sm30_get_ps_max_texcoord_count,
  unix_sm30_get_vs_has_fog_output,
  unix_sm30_get_sampler_decl_count,
  unix_sm30_get_sampler_decls,
  unix_sm30_get_max_constant_register,
};

struct sm50_initialize_params {
  const void *bytecode;
  size_t bytecode_size;
  sm50_shader_t *shader;
  void *reflection;
  sm50_error_t *error;
  int ret;
};

struct sm50_destroy_params {
  sm50_shader_t shader;
};

struct sm50_compile_params {
  sm50_shader_t shader;
  void *args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_get_compiled_bitcode_params {
  sm50_bitcode_t bitcode;
  struct SM50_COMPILED_BITCODE *data_out;
};

struct sm50_destroy_bitcode_params {
  sm50_bitcode_t bitcode;
};

struct sm50_get_error_message_params {
  sm50_error_t error;
  char *buffer;
  size_t buffer_size;
  size_t ret_size;
};

struct sm50_free_error_params {
  sm50_error_t error;
};

struct sm50_compile_geometry_pipeline_vertex_params {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  void *vertex_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_geometry_pipeline_geometry_params {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  void *geometry_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_vertex_params {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  void *vertex_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_hull_params {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  void *hull_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_domain_params {
  sm50_shader_t hull;
  sm50_shader_t domain;
  void *domain_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_get_arguments_info_params {
  sm50_shader_t shader;
  struct MTL_SM50_SHADER_ARGUMENT *constant_buffers;
  struct MTL_SM50_SHADER_ARGUMENT *arguments;
};

#if defined(__LP64__) || defined(_WIN64)
#define COMPATIBLE_STRUCT32(stru, size) _Static_assert(sizeof(struct stru##32) == size, "incompatible struct size");
#else
#define COMPATIBLE_STRUCT32(stru, size) _Static_assert(sizeof(struct stru) == size, "incompatible struct size");
#endif

struct sm50_initialize_params32 {
  uint32_t bytecode;
  unsigned int bytecode_size;
  uint32_t shader;
  uint32_t reflection;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_initialize_params, 24)

struct sm50_compile_params32 {
  sm50_shader_t shader;
  uint32_t args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_params, 32)

struct sm50_get_compiled_bitcode_params32 {
  sm50_bitcode_t bitcode;
  uint32_t data_out;
};

COMPATIBLE_STRUCT32(sm50_get_compiled_bitcode_params, 16)

struct sm50_get_error_message_params32 {
  sm50_error_t error;
  uint32_t buffer;
  uint32_t buffer_size;
  uint32_t ret_size;
};

COMPATIBLE_STRUCT32(sm50_get_error_message_params, 24)

struct sm50_compile_geometry_pipeline_vertex_params32 {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  uint32_t vertex_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_geometry_pipeline_vertex_params, 40)

struct sm50_compile_geometry_pipeline_geometry_params32 {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  uint32_t geometry_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_geometry_pipeline_geometry_params, 40)

struct sm50_compile_tessellation_pipeline_vertex_params32 {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  uint32_t vertex_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_vertex_params, 40)

struct sm50_compile_tessellation_pipeline_hull_params32 {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  uint32_t hull_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_hull_params, 40)

struct sm50_compile_tessellation_pipeline_domain_params32 {
  sm50_shader_t hull;
  sm50_shader_t domain;
  uint32_t domain_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_domain_params, 40)

struct sm50_get_arguments_info_params32 {
  sm50_shader_t shader;
  uint32_t constant_buffers;
  uint32_t arguments;
};

COMPATIBLE_STRUCT32(sm50_get_arguments_info_params, 16)

struct sm30_initialize_params {
  const void *bytecode;
  size_t bytecode_size;
  sm30_shader_t *shader;
  sm50_error_t *error;
  int ret;
};

struct sm30_destroy_params {
  sm30_shader_t shader;
};

struct sm30_compile_params {
  sm30_shader_t shader;
  void *args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm30_initialize_params32 {
  uint32_t bytecode;
  unsigned int bytecode_size;
  uint32_t shader;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm30_initialize_params, 20)

struct sm30_compile_params32 {
  sm30_shader_t shader;
  uint32_t args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm30_compile_params, 32)

struct sm30_get_input_decl_count_params {
  sm30_shader_t shader;
  uint32_t ret;
};

struct sm30_get_input_decls_params {
  sm30_shader_t shader;
  struct SM30_INPUT_DECL *decls;
};

struct sm30_get_input_decls_params32 {
  sm30_shader_t shader;
  uint32_t decls;
};

COMPATIBLE_STRUCT32(sm30_get_input_decls_params, 16)

struct sm30_get_ps_max_texcoord_count_params {
  sm30_shader_t shader;
  uint32_t ret;
};

struct sm30_get_vs_has_fog_output_params {
  sm30_shader_t shader;
  uint32_t ret;
};

struct sm30_get_sampler_decl_count_params {
  sm30_shader_t shader;
  uint32_t ret;
};

struct sm30_get_sampler_decls_params {
  sm30_shader_t shader;
  struct SM30_SAMPLER_DECL *decls;
};

struct sm30_get_sampler_decls_params32 {
  sm30_shader_t shader;
  uint32_t decls;
};

COMPATIBLE_STRUCT32(sm30_get_sampler_decls_params, 16)

struct sm30_get_max_constant_register_params {
  sm30_shader_t shader;
  uint32_t ret;
};

struct d3d9_ff_compile_vs_params {
  const void *key;
  const void *elements;
  uint32_t num_elements;
  uint32_t slot_mask;
  const char *func_name;
  void *args;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct d3d9_ff_compile_ps_params {
  const void *key;
  uint8_t texcoord_count;
  const char *func_name;
  void *args;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct d3d9_ff_compile_vs_params32 {
  uint32_t key;
  uint32_t elements;
  uint32_t num_elements;
  uint32_t slot_mask;
  uint32_t func_name;
  uint32_t args;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(d3d9_ff_compile_vs_params, 36)

struct d3d9_ff_compile_ps_params32 {
  uint32_t key;
  uint8_t texcoord_count;
  uint32_t func_name;
  uint32_t args;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(d3d9_ff_compile_ps_params, 28)

#define UNIX_CALL(code, params) WINE_UNIX_CALL(unix_##code, params)

#endif
