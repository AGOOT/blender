/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The ray-tracing module class handles ray generation, scheduling, tracing and denoising.
 */

#include <fstream>
#include <iostream>

#include "BKE_global.h"

#include "eevee_instance.hh"

#include "eevee_raytrace.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Raytracing
 *
 * \{ */

void RayTraceModule::init()
{
  const SceneEEVEE &sce_eevee = inst_.scene->eevee;

  ray_tracing_options_ = sce_eevee.ray_tracing_options;
  tracing_method_ = RaytraceEEVEE_Method(sce_eevee.ray_tracing_method);
}

void RayTraceModule::sync()
{
  Texture &depth_tx = inst_.render_buffers.depth_tx;

#define SHADER_VARIATION(_shader_name, _index) \
  ((_index == 0) ? _shader_name##REFLECT : \
   (_index == 1) ? _shader_name##REFRACT : \
                   _shader_name##DIFFUSE)

#define PASS_VARIATION(_pass_name, _index, _suffix) \
  ((_index == 0) ? _pass_name##reflect##_suffix : \
   (_index == 1) ? _pass_name##refract##_suffix : \
                   _pass_name##diffuse##_suffix)

  /* Setup. */
  {
    PassSimple &pass = tile_classify_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(RAY_TILE_CLASSIFY));
    pass.bind_image("tile_raytrace_denoise_img", &tile_raytrace_denoise_tx_);
    pass.bind_image("tile_raytrace_tracing_img", &tile_raytrace_tracing_tx_);
    pass.bind_image("tile_horizon_denoise_img", &tile_horizon_denoise_tx_);
    pass.bind_image("tile_horizon_tracing_img", &tile_horizon_tracing_tx_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(&tile_classify_dispatch_size_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_SHADER_STORAGE);
  }
  {
    PassSimple &pass = tile_compact_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(RAY_TILE_COMPACT));
    pass.bind_image("tile_raytrace_denoise_img", &tile_raytrace_denoise_tx_);
    pass.bind_image("tile_raytrace_tracing_img", &tile_raytrace_tracing_tx_);
    pass.bind_image("tile_horizon_denoise_img", &tile_horizon_denoise_tx_);
    pass.bind_image("tile_horizon_tracing_img", &tile_horizon_tracing_tx_);
    pass.bind_ssbo("raytrace_tracing_dispatch_buf", &raytrace_tracing_dispatch_buf_);
    pass.bind_ssbo("raytrace_denoise_dispatch_buf", &raytrace_denoise_dispatch_buf_);
    pass.bind_ssbo("horizon_tracing_dispatch_buf", &horizon_tracing_dispatch_buf_);
    pass.bind_ssbo("horizon_denoise_dispatch_buf", &horizon_denoise_dispatch_buf_);
    pass.bind_ssbo("raytrace_tracing_tiles_buf", &raytrace_tracing_tiles_buf_);
    pass.bind_ssbo("raytrace_denoise_tiles_buf", &raytrace_denoise_tiles_buf_);
    pass.bind_ssbo("horizon_tracing_tiles_buf", &horizon_tracing_tiles_buf_);
    pass.bind_ssbo("horizon_denoise_tiles_buf", &horizon_denoise_tiles_buf_);
    pass.bind_resources(inst_.uniform_data);
    pass.dispatch(&tile_compact_dispatch_size_);
    pass.barrier(GPU_BARRIER_SHADER_STORAGE);
  }
  for (auto type : IndexRange(3)) {
    PassSimple &pass = PASS_VARIATION(generate_, type, _ps_);
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(SHADER_VARIATION(RAY_GENERATE_, type)));
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_image("out_ray_data_img", &ray_data_tx_);
    pass.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(raytrace_tracing_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_TEXTURE_FETCH |
                 GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  /* Tracing. */
  for (auto type : IndexRange(3)) {
    PassSimple &pass = PASS_VARIATION(trace_, type, _ps_);
    pass.init();
    if (inst_.planar_probes.enabled() && (&pass == &trace_reflect_ps_)) {
      /* Inject planar tracing in the same pass as reflection tracing. */
      PassSimple::Sub &sub = pass.sub("Trace.Planar");
      sub.shader_set(inst_.shaders.static_shader_get(RAY_TRACE_PLANAR));
      sub.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
      sub.bind_image("ray_data_img", &ray_data_tx_);
      sub.bind_image("ray_time_img", &ray_time_tx_);
      sub.bind_image("ray_radiance_img", &ray_radiance_tx_);
      sub.bind_texture("depth_tx", &depth_tx);
      sub.bind_resources(inst_.uniform_data);
      sub.bind_resources(inst_.planar_probes);
      sub.bind_resources(inst_.irradiance_cache);
      sub.bind_resources(inst_.reflection_probes);
      /* TODO(@fclem): Use another dispatch with only tiles that touches planar captures. */
      sub.dispatch(raytrace_tracing_dispatch_buf_);
      sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
    pass.shader_set(inst_.shaders.static_shader_get(SHADER_VARIATION(RAY_TRACE_SCREEN_, type)));
    pass.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
    pass.bind_image("ray_data_img", &ray_data_tx_);
    pass.bind_image("ray_time_img", &ray_time_tx_);
    pass.bind_texture("screen_radiance_tx", &screen_radiance_tx_);
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_image("ray_radiance_img", &ray_radiance_tx_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources((&pass == &trace_refract_ps_) ? inst_.hiz_buffer.back :
                                                        inst_.hiz_buffer.front);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.irradiance_cache);
    pass.bind_resources(inst_.reflection_probes);
    pass.dispatch(raytrace_tracing_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  {
    PassSimple &pass = trace_fallback_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(RAY_TRACE_FALLBACK));
    pass.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
    pass.bind_image("ray_data_img", &ray_data_tx_);
    pass.bind_image("ray_time_img", &ray_time_tx_);
    pass.bind_image("ray_radiance_img", &ray_radiance_tx_);
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.irradiance_cache);
    pass.bind_resources(inst_.reflection_probes);
    pass.bind_resources(inst_.sampling);
    pass.dispatch(raytrace_tracing_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  /* Denoise. */
  for (auto type : IndexRange(3)) {
    PassSimple &pass = PASS_VARIATION(denoise_spatial_, type, _ps_);
    GPUShader *sh = inst_.shaders.static_shader_get(SHADER_VARIATION(RAY_DENOISE_SPATIAL_, type));
    pass.init();
    pass.specialize_constant(sh, "raytrace_resolution_scale", &data_.resolution_scale);
    pass.specialize_constant(sh, "skip_denoise", reinterpret_cast<bool *>(&data_.skip_denoise));
    pass.shader_set(sh);
    pass.bind_ssbo("tiles_coord_buf", &raytrace_denoise_tiles_buf_);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_image("ray_data_img", &ray_data_tx_);
    pass.bind_image("ray_time_img", &ray_time_tx_);
    pass.bind_image("ray_radiance_img", &ray_radiance_tx_);
    pass.bind_image("out_radiance_img", &denoised_spatial_tx_);
    pass.bind_image("out_variance_img", &hit_variance_tx_);
    pass.bind_image("out_hit_depth_img", &hit_depth_tx_);
    pass.bind_image("tile_mask_img", &tile_raytrace_denoise_tx_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(raytrace_denoise_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  {
    PassSimple &pass = denoise_temporal_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(RAY_DENOISE_TEMPORAL));
    pass.bind_resources(inst_.uniform_data);
    pass.bind_texture("radiance_history_tx", &radiance_history_tx_);
    pass.bind_texture("variance_history_tx", &variance_history_tx_);
    pass.bind_texture("tilemask_history_tx", &tilemask_history_tx_);
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_image("hit_depth_img", &hit_depth_tx_);
    pass.bind_image("in_radiance_img", &denoised_spatial_tx_);
    pass.bind_image("out_radiance_img", &denoised_temporal_tx_);
    pass.bind_image("in_variance_img", &hit_variance_tx_);
    pass.bind_image("out_variance_img", &denoise_variance_tx_);
    pass.bind_ssbo("tiles_coord_buf", &raytrace_denoise_tiles_buf_);
    pass.bind_resources(inst_.sampling);
    pass.dispatch(raytrace_denoise_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  for (auto type : IndexRange(3)) {
    PassSimple &pass = PASS_VARIATION(denoise_bilateral_, type, _ps_);
    pass.init();
    pass.shader_set(
        inst_.shaders.static_shader_get(SHADER_VARIATION(RAY_DENOISE_BILATERAL_, type)));
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_image("in_radiance_img", &denoised_temporal_tx_);
    pass.bind_image("out_radiance_img", &denoised_bilateral_tx_);
    pass.bind_image("in_variance_img", &denoise_variance_tx_);
    pass.bind_image("tile_mask_img", &tile_raytrace_denoise_tx_);
    pass.bind_ssbo("tiles_coord_buf", &raytrace_denoise_tiles_buf_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(raytrace_denoise_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  {
    PassSimple &pass = horizon_setup_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(HORIZON_SETUP));
    pass.bind_resources(inst_.uniform_data);
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_texture("in_radiance_tx", &screen_radiance_tx_, GPUSamplerState::default_sampler());
    pass.bind_image("out_radiance_img", &downsampled_in_radiance_tx_);
    pass.bind_image("out_normal_img", &downsampled_in_normal_tx_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(&tracing_dispatch_size_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  for (auto type : IndexRange(3)) {
    PassSimple &pass = PASS_VARIATION(horizon_scan_, type, _ps_);
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(SHADER_VARIATION(HORIZON_SCAN_, type)));
    pass.bind_image("horizon_radiance_img", &horizon_radiance_tx_);
    pass.bind_image("horizon_occlusion_img", &horizon_occlusion_tx_);
    pass.bind_ssbo("tiles_coord_buf", &horizon_tracing_tiles_buf_);
    pass.bind_texture("screen_radiance_tx", &downsampled_in_radiance_tx_);
    pass.bind_texture("screen_normal_tx", &downsampled_in_normal_tx_);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.hiz_buffer.front);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.gbuffer);
    pass.dispatch(horizon_tracing_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
  {
    PassSimple &pass = horizon_denoise_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(HORIZON_DENOISE));
    pass.bind_texture("depth_tx", &depth_tx);
    pass.bind_image("horizon_radiance_img", &horizon_radiance_tx_);
    pass.bind_image("horizon_occlusion_img", &horizon_occlusion_tx_);
    pass.bind_image("radiance_img", &horizon_scan_output_tx_);
    pass.bind_image("tile_mask_img", &tile_horizon_denoise_tx_);
    pass.bind_ssbo("tiles_coord_buf", &horizon_denoise_tiles_buf_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.gbuffer);
    pass.bind_resources(inst_.irradiance_cache);
    pass.bind_resources(inst_.reflection_probes);
    pass.dispatch(horizon_denoise_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
#undef SHADER_VARIATION
#undef PASS_VARIATION
}

void RayTraceModule::debug_pass_sync() {}

void RayTraceModule::debug_draw(View & /*view*/, GPUFrameBuffer * /*view_fb*/) {}

RayTraceResult RayTraceModule::render(RayTraceBuffer &rt_buffer,
                                      GPUTexture *screen_radiance_back_tx,
                                      GPUTexture *screen_radiance_front_tx,
                                      const float4x4 &screen_radiance_persmat,
                                      eClosureBits active_closures,
                                      /* TODO(fclem): Maybe wrap these two in some other class. */
                                      View &main_view,
                                      View &render_view,
                                      bool do_refraction_tracing)
{
  using namespace blender::math;

  RaytraceEEVEE options = ray_tracing_options_;

  bool use_horizon_scan = options.screen_trace_max_roughness < 1.0f;
  if (ELEM(active_closures, CLOSURE_REFRACTION, CLOSURE_NONE)) {
    /* Disable horizon scan if there is only a refraction closure. Avoid the setup cost. */
    use_horizon_scan = false;
  }

  const int resolution_scale = max_ii(1, power_of_2_max_i(options.resolution_scale));

  const int2 extent = inst_.film.render_extent_get();
  const int2 tracing_res = math::divide_ceil(extent, int2(resolution_scale));
  const int2 dummy_extent(1, 1);
  const int2 group_size(RAYTRACE_GROUP_SIZE);

  const int2 denoise_tiles = divide_ceil(extent, group_size);
  const int2 raytrace_tiles = divide_ceil(tracing_res, group_size);
  const int denoise_tile_count = denoise_tiles.x * denoise_tiles.y;
  const int raytrace_tile_count = raytrace_tiles.x * raytrace_tiles.y;
  tile_classify_dispatch_size_ = int3(denoise_tiles, 1);
  tile_compact_dispatch_size_ = int3(divide_ceil(raytrace_tiles, group_size), 1);
  tracing_dispatch_size_ = int3(divide_ceil(tracing_res, group_size), 1);

  const int closure_count = 3;
  eGPUTextureFormat format = RAYTRACE_TILEMASK_FORMAT;
  eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  tile_raytrace_denoise_tx_.ensure_2d_array(format, denoise_tiles, closure_count, usage_rw);
  tile_raytrace_tracing_tx_.ensure_2d_array(format, raytrace_tiles, closure_count, usage_rw);
  tile_horizon_denoise_tx_.ensure_2d_array(format, denoise_tiles, closure_count, usage_rw);
  tile_horizon_tracing_tx_.ensure_2d_array(format, raytrace_tiles, closure_count, usage_rw);

  tile_raytrace_denoise_tx_.clear(uint4(0u));
  tile_raytrace_tracing_tx_.clear(uint4(0u));
  tile_horizon_denoise_tx_.clear(uint4(0u));
  tile_horizon_tracing_tx_.clear(uint4(0u));

  horizon_tracing_tiles_buf_.resize(ceil_to_multiple_u(raytrace_tile_count, 512));
  horizon_denoise_tiles_buf_.resize(ceil_to_multiple_u(denoise_tile_count, 512));
  raytrace_tracing_tiles_buf_.resize(ceil_to_multiple_u(raytrace_tile_count, 512));
  raytrace_denoise_tiles_buf_.resize(ceil_to_multiple_u(denoise_tile_count, 512));

  /* Data for tile classification. */
  float roughness_mask_start = options.screen_trace_max_roughness;
  float roughness_mask_fade = 0.2f;
  data_.roughness_mask_scale = 1.0 / roughness_mask_fade;
  data_.roughness_mask_bias = data_.roughness_mask_scale * roughness_mask_start;

  /* Data for the radiance setup. */
  data_.brightness_clamp = (options.sample_clamp > 0.0) ? options.sample_clamp : 1e20;
  data_.resolution_scale = resolution_scale;
  data_.resolution_bias = int2(inst_.sampling.rng_2d_get(SAMPLING_RAYTRACE_V) * resolution_scale);
  data_.radiance_persmat = screen_radiance_persmat;
  data_.full_resolution = extent;
  data_.full_resolution_inv = 1.0f / float2(extent);

  /* TODO(fclem): Eventually all uniform data is setup here. */

  inst_.uniform_data.push_update();

  RayTraceResult result;

  DRW_stats_group_start("Raytracing");

  if (use_horizon_scan) {
    downsampled_in_radiance_tx_.acquire(tracing_res, RAYTRACE_RADIANCE_FORMAT, usage_rw);
    downsampled_in_normal_tx_.acquire(tracing_res, GPU_RGBA8, usage_rw);

    screen_radiance_tx_ = screen_radiance_front_tx;
    inst_.manager->submit(horizon_setup_ps_, render_view);
  }

  if (active_closures != CLOSURE_NONE) {
    inst_.manager->submit(tile_classify_ps_);
  }

  result.diffuse = trace("Diffuse",
                         options,
                         rt_buffer,
                         screen_radiance_front_tx,
                         screen_radiance_persmat,
                         active_closures,
                         CLOSURE_DIFFUSE,
                         main_view,
                         render_view,
                         use_horizon_scan,
                         false);

  result.reflect = trace("Reflection",
                         options,
                         rt_buffer,
                         screen_radiance_front_tx,
                         screen_radiance_persmat,
                         active_closures,
                         CLOSURE_REFLECTION,
                         main_view,
                         render_view,
                         use_horizon_scan,
                         false);

  result.refract = trace("Refraction",
                         options,
                         rt_buffer,
                         screen_radiance_back_tx,
                         render_view.persmat(),
                         active_closures,
                         CLOSURE_REFRACTION,
                         main_view,
                         render_view,
                         false, /* Not yet supported */
                         !do_refraction_tracing);

  downsampled_in_radiance_tx_.release();
  downsampled_in_normal_tx_.release();

  DRW_stats_group_end();

  return result;
}

RayTraceResultTexture RayTraceModule::trace(
    const char *debug_pass_name,
    RaytraceEEVEE options,
    RayTraceBuffer &rt_buffer,
    GPUTexture *screen_radiance_tx,
    const float4x4 &screen_radiance_persmat,
    eClosureBits active_closures,
    eClosureBits raytrace_closure,
    /* TODO(fclem): Maybe wrap these two in some other class. */
    View &main_view,
    View &render_view,
    bool use_horizon_scan,
    bool force_no_tracing)
{
  BLI_assert_msg(count_bits_i(raytrace_closure) == 1,
                 "Only one closure type can be raytraced at a time.");
  BLI_assert_msg(raytrace_closure == (raytrace_closure &
                                      (CLOSURE_REFLECTION | CLOSURE_REFRACTION | CLOSURE_DIFFUSE)),
                 "Only reflection and refraction are implemented.");

  if (tracing_method_ == RAYTRACE_EEVEE_METHOD_NONE) {
    force_no_tracing = true;
  }

  screen_radiance_tx_ = screen_radiance_tx;

  PassSimple *generate_ray_ps = nullptr;
  PassSimple *trace_ray_ps = nullptr;
  PassSimple *denoise_spatial_ps = nullptr;
  PassSimple *denoise_bilateral_ps = nullptr;
  PassSimple *horizon_scan_ps = nullptr;
  RayTraceBuffer::DenoiseBuffer *denoise_buf = nullptr;
  int closure_index = 0;

  if (raytrace_closure == CLOSURE_DIFFUSE) {
    generate_ray_ps = &generate_diffuse_ps_;
    trace_ray_ps = force_no_tracing ? &trace_fallback_ps_ : &trace_diffuse_ps_;
    denoise_spatial_ps = &denoise_spatial_diffuse_ps_;
    denoise_bilateral_ps = &denoise_bilateral_diffuse_ps_;
    denoise_buf = &rt_buffer.diffuse;
    horizon_scan_ps = &horizon_scan_diffuse_ps_;
    closure_index = 0;
  }
  else if (raytrace_closure == CLOSURE_REFLECTION) {
    generate_ray_ps = &generate_reflect_ps_;
    trace_ray_ps = force_no_tracing ? &trace_fallback_ps_ : &trace_reflect_ps_;
    denoise_spatial_ps = &denoise_spatial_reflect_ps_;
    denoise_bilateral_ps = &denoise_bilateral_reflect_ps_;
    denoise_buf = &rt_buffer.reflection;
    horizon_scan_ps = &horizon_scan_reflect_ps_;
    closure_index = 1;
  }
  else if (raytrace_closure == CLOSURE_REFRACTION) {
    generate_ray_ps = &generate_refract_ps_;
    trace_ray_ps = force_no_tracing ? &trace_fallback_ps_ : &trace_refract_ps_;
    denoise_spatial_ps = &denoise_spatial_refract_ps_;
    denoise_bilateral_ps = &denoise_bilateral_refract_ps_;
    denoise_buf = &rt_buffer.refraction;
    horizon_scan_ps = &horizon_scan_refract_ps_;
    closure_index = 2;
  }

  if ((active_closures & raytrace_closure) == 0) {
    /* Early out. Release persistent buffers. Still acquire one dummy resource for validation. */
    denoise_buf->denoised_spatial_tx.acquire(int2(1), RAYTRACE_RADIANCE_FORMAT);
    denoise_buf->radiance_history_tx.free();
    denoise_buf->variance_history_tx.free();
    denoise_buf->tilemask_history_tx.free();
    return {denoise_buf->denoised_spatial_tx};
  }

  const int resolution_scale = max_ii(1, power_of_2_max_i(options.resolution_scale));

  const int2 extent = inst_.film.render_extent_get();
  const int2 tracing_res = math::divide_ceil(extent, int2(resolution_scale));

  renderbuf_depth_view_ = inst_.render_buffers.depth_tx;

  const bool use_denoise = (options.flag & RAYTRACE_EEVEE_USE_DENOISE);
  const bool use_spatial_denoise = (options.denoise_stages & RAYTRACE_EEVEE_DENOISE_SPATIAL) &&
                                   use_denoise;
  const bool use_temporal_denoise = (options.denoise_stages & RAYTRACE_EEVEE_DENOISE_TEMPORAL) &&
                                    use_spatial_denoise;
  const bool use_bilateral_denoise = (options.denoise_stages & RAYTRACE_EEVEE_DENOISE_BILATERAL) &&
                                     use_temporal_denoise;

  eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;

  DRW_stats_group_start(debug_pass_name);

  data_.thickness = options.screen_trace_thickness;
  data_.quality = 1.0f - 0.95f * options.screen_trace_quality;
  data_.brightness_clamp = (options.sample_clamp > 0.0) ? options.sample_clamp : 1e20;

  float roughness_mask_start = options.screen_trace_max_roughness;
  float roughness_mask_fade = 0.2f;
  data_.roughness_mask_scale = 1.0 / roughness_mask_fade;
  data_.roughness_mask_bias = data_.roughness_mask_scale * roughness_mask_start;

  data_.resolution_scale = resolution_scale;
  data_.closure_active = raytrace_closure;
  data_.resolution_bias = int2(inst_.sampling.rng_2d_get(SAMPLING_RAYTRACE_V) * resolution_scale);
  data_.history_persmat = denoise_buf->history_persmat;
  data_.radiance_persmat = screen_radiance_persmat;
  data_.full_resolution = extent;
  data_.full_resolution_inv = 1.0f / float2(extent);
  data_.skip_denoise = !use_spatial_denoise;
  data_.closure_index = closure_index;
  inst_.uniform_data.push_update();

  /* Ray setup. */
  raytrace_tracing_dispatch_buf_.clear_to_zero();
  raytrace_denoise_dispatch_buf_.clear_to_zero();
  horizon_tracing_dispatch_buf_.clear_to_zero();
  horizon_denoise_dispatch_buf_.clear_to_zero();
  inst_.manager->submit(tile_compact_ps_);

  {
    /* Tracing rays. */
    ray_data_tx_.acquire(tracing_res, GPU_RGBA16F);
    ray_time_tx_.acquire(tracing_res, RAYTRACE_RAYTIME_FORMAT);
    ray_radiance_tx_.acquire(tracing_res, RAYTRACE_RADIANCE_FORMAT);

    inst_.manager->submit(*generate_ray_ps, render_view);
    inst_.manager->submit(*trace_ray_ps, render_view);
  }

  RayTraceResultTexture result;

  /* Spatial denoise pass is required to resolve at least one ray per pixel. */
  {
    denoise_buf->denoised_spatial_tx.acquire(extent, RAYTRACE_RADIANCE_FORMAT);
    hit_variance_tx_.acquire(use_temporal_denoise ? extent : int2(1), RAYTRACE_VARIANCE_FORMAT);
    hit_depth_tx_.acquire(use_temporal_denoise ? extent : int2(1), GPU_R32F);
    denoised_spatial_tx_ = denoise_buf->denoised_spatial_tx;

    inst_.manager->submit(*denoise_spatial_ps, render_view);

    result = {denoise_buf->denoised_spatial_tx};
  }

  ray_data_tx_.release();
  ray_time_tx_.release();
  ray_radiance_tx_.release();

  if (use_temporal_denoise) {
    denoise_buf->denoised_temporal_tx.acquire(extent, RAYTRACE_RADIANCE_FORMAT, usage_rw);
    denoise_variance_tx_.acquire(
        use_bilateral_denoise ? extent : int2(1), RAYTRACE_VARIANCE_FORMAT, usage_rw);
    denoise_buf->variance_history_tx.ensure_2d(
        RAYTRACE_VARIANCE_FORMAT, use_bilateral_denoise ? extent : int2(1), usage_rw);
    denoise_buf->tilemask_history_tx.ensure_2d_array(RAYTRACE_TILEMASK_FORMAT,
                                                     tile_raytrace_denoise_tx_.size().xy(),
                                                     tile_raytrace_denoise_tx_.size().z,
                                                     usage_rw);

    if (denoise_buf->radiance_history_tx.ensure_2d(RAYTRACE_RADIANCE_FORMAT, extent, usage_rw) ||
        denoise_buf->valid_history == false)
    {
      /* If viewport resolution changes, do not try to use history. */
      denoise_buf->tilemask_history_tx.clear(uint4(0u));
    }

    radiance_history_tx_ = denoise_buf->radiance_history_tx;
    variance_history_tx_ = denoise_buf->variance_history_tx;
    tilemask_history_tx_ = denoise_buf->tilemask_history_tx;
    denoised_temporal_tx_ = denoise_buf->denoised_temporal_tx;

    inst_.manager->submit(denoise_temporal_ps_, render_view);

    /* Save view-projection matrix for next reprojection. */
    denoise_buf->history_persmat = main_view.persmat();
    /* Radiance will be swapped with history in #RayTraceResult::release().
     * Variance is swapped with history after bilateral denoise.
     * It keeps data-flow easier to follow. */
    result = {denoise_buf->denoised_temporal_tx, denoise_buf->radiance_history_tx};
    /* Not referenced by result anymore. */
    denoise_buf->denoised_spatial_tx.release();

    GPU_texture_copy(denoise_buf->tilemask_history_tx, tile_raytrace_denoise_tx_);
  }

  /* Only use history buffer for the next frame if temporal denoise was used by the current one. */
  denoise_buf->valid_history = use_temporal_denoise;

  hit_variance_tx_.release();
  hit_depth_tx_.release();

  if (use_bilateral_denoise) {
    denoise_buf->denoised_bilateral_tx.acquire(extent, RAYTRACE_RADIANCE_FORMAT, usage_rw);
    denoised_bilateral_tx_ = denoise_buf->denoised_bilateral_tx;

    inst_.manager->submit(*denoise_bilateral_ps, render_view);

    /* Swap after last use. */
    TextureFromPool::swap(denoise_buf->denoised_temporal_tx, denoise_buf->radiance_history_tx);
    TextureFromPool::swap(denoise_variance_tx_, denoise_buf->variance_history_tx);

    result = {denoise_buf->denoised_bilateral_tx};
    /* Not referenced by result anymore. */
    denoise_buf->denoised_temporal_tx.release();
  }

  denoise_variance_tx_.release();

  if (use_horizon_scan) {
    horizon_occlusion_tx_.acquire(tracing_res, GPU_R8, usage_rw);
    horizon_radiance_tx_.acquire(tracing_res, RAYTRACE_RADIANCE_FORMAT, usage_rw);

    inst_.manager->submit(*horizon_scan_ps, render_view);

    horizon_scan_output_tx_ = result.get();

    inst_.manager->submit(horizon_denoise_ps_, render_view);

    horizon_occlusion_tx_.release();
    horizon_radiance_tx_.release();
  }

  DRW_stats_group_end();

  return result;
}

/** \} */

}  // namespace blender::eevee
