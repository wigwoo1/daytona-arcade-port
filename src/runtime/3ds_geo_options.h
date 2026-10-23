#pragma once
#if defined(__3DS__)
namespace rt::geo_options_3ds {
// Experimental; preserve all native raster state transitions and cull flags.
inline bool skip_culled_lighting=false;
inline bool far_model_distance=false,verify_far_model_distance=false;
inline unsigned long long far_models=0,far_polygons=0,far_verified=0;
inline bool direct_texture_reads=false,verify_texture_reads=false;
inline unsigned long long texture_read_checks=0;
inline bool reuse_polygon_scratch=false,verify_polygon_scratch=false;
inline unsigned long long scratch_poisoned_polygons=0,scratch_nested_fallbacks=0;
}
#endif