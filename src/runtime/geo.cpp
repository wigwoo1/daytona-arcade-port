// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer (HLE) and rasterizer front end, transplanted from
// MAME's src/mame/sega/model2_v.cpp at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notice above kept as the licence requires). The code below
// is MAME's with these changes only: raw pointers are bounds-checked cursors
// (GeoPtr, GeoPtr16); kept polygons go to a flat list (Geo::polys) instead of
// MAME's per-z buckets; logerror is dropped and fatalerror throws. See
// src/runtime/geo.h and THIRD_PARTY.md.

#include "runtime/geo.h"
#include "runtime/3ds_geo_profile.h"
#include "runtime/3ds_geo_options.h"
#if defined(__3DS__)
#include "runtime/3ds_far_models.h"
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace rt {

namespace {
inline float u2f(uint32_t v) { return std::bit_cast<float>(v); }
inline uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
using u8 = uint8_t;
using u64 = uint64_t;
} // namespace

#define pz      p[0]
#define pu      p[1]
#define pv      p[2]

struct quad_m2 {
    GeoVertex v[4];
    uint16_t z = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
};

#if defined(__3DS__)
// One workspace per CPU thread. Every active vertex is fully overwritten before
// use; inactive slots remain valid objects and are never part of a polygon.
struct PolygonScratch3ds {
    quad_m2 object;
    std::array<std::array<GeoVertex,8>,2> clipped;
    bool busy=false;
};
static constinit thread_local PolygonScratch3ds polygon_scratch_3ds;
struct PolygonScratchLease3ds {
    PolygonScratch3ds *scratch=nullptr;
    explicit PolygonScratchLease3ds(bool requested) {
        if(!requested) return;
        if(polygon_scratch_3ds.busy) {
            if(geo_options_3ds::verify_polygon_scratch) ++geo_options_3ds::scratch_nested_fallbacks;
            return;
        }
        scratch=&polygon_scratch_3ds;scratch->busy=true;
        if(geo_options_3ds::verify_polygon_scratch) {
            const float n=std::bit_cast<float>(uint32_t(0x7fc12345));
            const GeoVertex poison{n,n,{n,n,n}};
            std::fill(std::begin(scratch->object.v),std::end(scratch->object.v),poison);
            scratch->object.z=0xa5a5;scratch->object.luma=0xa5;scratch->object.texlod=-123456789;
            std::fill(std::begin(scratch->object.texheader),std::end(scratch->object.texheader),uint16_t(0xa5a5));
            for(auto &vertices:scratch->clipped) vertices.fill(poison);
            ++geo_options_3ds::scratch_poisoned_polygons;
        }
    }
    ~PolygonScratchLease3ds(){if(scratch) scratch->busy=false;}
    PolygonScratchLease3ds(const PolygonScratchLease3ds&)=delete;
    PolygonScratchLease3ds& operator=(const PolygonScratchLease3ds&)=delete;
};
#endif
Geo::Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer)
    : polygon_rom_(polygons.size() / 4), texture_rom_(textures.size() / 2), buffer_(buffer) {
    if (polygon_rom_.empty() || (polygon_rom_.size() & (polygon_rom_.size() - 1)) || texture_rom_.empty() ||
        (texture_rom_.size() & (texture_rom_.size() - 1)))
        throw GeoFatal("bad polygon or texture ROM image");
    std::memcpy(polygon_rom_.data(), polygons.data(), polygon_rom_.size() * 4);
    std::memcpy(texture_rom_.data(), textures.data(), texture_rom_.size() * 2);
    polygon_rom_words_ = polygon_rom_.size();
    texture_rom_words_ = texture_rom_.size();
    bind_roms();
}

Geo::Geo(std::vector<uint8_t> &polygons, std::vector<uint8_t> &textures, uint32_t *buffer, bool consume)
    : buffer_(buffer) {
    if (polygons.empty() || (polygons.size() & (polygons.size() - 1)) ||
        textures.empty() || (textures.size() & (textures.size() - 1)) ||
        polygons.size() < 4 || textures.size() < 2)
        throw GeoFatal("bad polygon or texture ROM image");
    polygon_rom_.resize(polygons.size() / 4);
    std::memcpy(polygon_rom_.data(), polygons.data(), polygons.size());
    if (consume) std::vector<uint8_t>().swap(polygons);
    texture_rom_.resize(textures.size() / 2);
    std::memcpy(texture_rom_.data(), textures.data(), textures.size());
    if (consume) std::vector<uint8_t>().swap(textures);
    polygon_rom_words_ = polygon_rom_.size();
    texture_rom_words_ = texture_rom_.size();
    bind_roms();
}

Geo::Geo(const uint32_t *polygons, size_t polygon_words, const uint16_t *textures, size_t texture_words,
         uint32_t *buffer)
    : polygon_rom_data_(polygons), texture_rom_data_(textures), polygon_rom_words_(polygon_words),
      texture_rom_words_(texture_words), buffer_(buffer) {
    if (!polygons || !textures || !polygon_words || (polygon_words & (polygon_words - 1)) ||
        !texture_words || (texture_words & (texture_words - 1)))
        throw GeoFatal("bad mapped polygon or texture ROM image");
    bind_roms();
}

Geo::Geo(GeoRomRead polygon_read, void *polygon_context, size_t polygon_words,
         GeoRomRead texture_read, void *texture_context, size_t texture_words, uint32_t *buffer)
    : polygon_rom_read_(polygon_read), texture_rom_read_(texture_read),
      polygon_rom_context_(polygon_context), texture_rom_context_(texture_context),
      polygon_rom_words_(polygon_words), texture_rom_words_(texture_words), buffer_(buffer) {
    if (!polygon_read || !texture_read || !polygon_words || (polygon_words & (polygon_words - 1)) ||
        !texture_words || (texture_words & (texture_words - 1)))
        throw GeoFatal("bad chunked polygon or texture ROM image");
    bind_roms();
}

void Geo::bind_roms() {
#if defined(__3DS__)
    far_models_3ds::clear_cache();
#endif
    raster_.texture_rom = texture_rom_data_ ? texture_rom_data_ : texture_rom_.data();
    raster_.texture_read = texture_rom_read_;
    raster_.texture_context = texture_rom_context_;
    raster_.texture_rom_mask = uint32_t(texture_rom_words_ - 1);
    geo_.raster = &raster_;
    geo_.polygon_rom = polygon_rom_data_ ? polygon_rom_data_ : polygon_rom_.data();
    geo_.polygon_read = polygon_rom_read_;
    geo_.polygon_context = polygon_rom_context_;
    geo_.polygon_rom_mask = uint32_t(polygon_rom_words_ - 1);
#if !defined(DAYTONA_N64_LOW_MEMORY)
    render_reference_depths_.reserve(8192);
#endif
}

void Geo::set_render_distance(float depth) {
    if (!std::isfinite(depth) || depth < 0)
        throw std::invalid_argument("invalid geometry render distance");
    render_distance_ = depth;
}

static inline void transform_point(GeoVertex *point, float *matrix)
{
	float tx = (point->x * matrix[0]) + (point->y * matrix[3]) + (point->pz * matrix[6]) + (matrix[9]);
	float ty = (point->x * matrix[1]) + (point->y * matrix[4]) + (point->pz * matrix[7]) + (matrix[10]);
	float tz = (point->x * matrix[2]) + (point->y * matrix[5]) + (point->pz * matrix[8]) + (matrix[11]);

	point->x = tx;
	point->y = ty;
	point->pz = tz;
}

static inline void transform_vector(GeoVertex *vector, float *matrix)
{
	float tx = (vector->x * matrix[0]) + (vector->y * matrix[3]) + (vector->pz * matrix[6]);
	float ty = (vector->x * matrix[1]) + (vector->y * matrix[4]) + (vector->pz * matrix[7]);
	float tz = (vector->x * matrix[2]) + (vector->y * matrix[5]) + (vector->pz * matrix[8]);

	vector->x = tx;
	vector->y = ty;
	vector->pz = tz;
}

static inline void normalize_vector(GeoVertex *vector)
{
	const float n = sqrt((vector->x * vector->x) + (vector->y * vector->y) + (vector->pz * vector->pz));

	if (n)
	{
		float oon = 1.0f / n;
		vector->x *= oon;
		vector->y *= oon;
		vector->pz *= oon;
	}
}

static inline float dot_product(const GeoVertex &v1, const GeoVertex &v2)
{
	return (v1.x * v2.x) + (v1.y * v2.y) + (v1.pz * v2.pz);
}

static inline void vector_cross3(GeoVertex *dst, const GeoVertex *v0, const GeoVertex *v1, const GeoVertex *v2)
{
	GeoVertex p1, p2;

	p1.x = v1->x - v0->x;   p1.y = v1->y - v0->y;   p1.pz = v1->pz - v0->pz;
	p2.x = v2->x - v0->x;   p2.y = v2->y - v0->y;   p2.pz = v2->pz - v0->pz;

	dst->x = (p1.y * p2.pz) - (p1.pz * p2.y);
	dst->y = (p1.pz * p2.x) - (p1.x * p2.pz);
	dst->pz = (p1.x * p2.y) - (p1.y * p2.x);
}

static inline void apply_focus(Geo::geo_state *geo, GeoVertex *p0)
{
	p0->x *= geo->focus.x;
	p0->y *= geo->focus.y;
}


/* 1.8.23 float to 4.12 float converter, courtesy of Aaron Giles */
inline uint16_t Geo::float_to_zval(float floatval, int32_t z_adjust)
{
	int32_t fpint = f2u(floatval);
	int32_t exponent = ((fpint >> 23) & 0xff) - ((z_adjust >> 23) & 0xff);
	uint32_t mantissa = fpint & 0x7fffff;

	/* round the low bits and reduce to 12 */
	mantissa += 0x400;
	if (mantissa > 0x7fffff)
	{
		exponent++;
		mantissa = (mantissa & 0x7fffff) >> 1;
	}
	mantissa >>= 11;

	// if negative, clamp to 0
	if (fpint < 0)
		return 0x0000;

	// the rest depends on the exponent
	if (exponent < -12)
		return 0x0000; // less than -12 is too small, return 0
	else if (exponent < 0)
		return (mantissa | 0x1000) >> -exponent; // between -12 and 0 create a denormal with exponent of 0
	else if (exponent < 15)
		return ((exponent + 1) << 12) | mantissa; // between 0 and 14 create a FP value with exponent + 1
	else
		return 0xffff; // above 14 is too large
}

static int32_t clip_polygon(GeoVertex *v, int32_t num_vertices, GeoVertex *vout, Geo::plane clip_plane)
{
	int32_t outcount = 0;

	const GeoVertex *cur = v;
	GeoVertex *out = vout;

	float curdot = dot_product(*cur, clip_plane.normal);
	int32_t curin = (curdot >= clip_plane.distance) ? 1 : 0;

	for (int32_t i = 0; i < num_vertices; i++)
	{
		const int32_t nextvert = (i + 1 == num_vertices) ? 0 : i + 1;

		/* if the current point is inside the plane, add it */
		if (curin)
			out[outcount++] = *cur;

		const float nextdot = dot_product(v[nextvert], clip_plane.normal);
		const int32_t nextin = (nextdot >= clip_plane.distance) ? 1 : 0;

		/* Add a clipped vertex if one end of the current edge is inside the plane and the other is outside */
		if ((curin != nextin) && !std::isnan(curdot) && !std::isnan(nextdot))
		{
			const float scale = (clip_plane.distance - curdot) / (nextdot - curdot);

			out[outcount].x = cur->x + ((v[nextvert].x - cur->x) * scale);
			out[outcount].y = cur->y + ((v[nextvert].y - cur->y) * scale);
			out[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
			out[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
			out[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
			outcount++;
		}

		curdot = nextdot;
		curin = nextin;
		cur++;
	}

	return outcount;
}

#if defined(__3DS__)
void Geo::retain_native_candidate(const GeoVertex *vertices, unsigned count, uint16_t z, const plane *planes) {
    static_assert(sizeof(plane)==6*sizeof(float));
    // Snapshot the actual planes: window/center commands may change them later.
    if (native_planes_.empty() ||
        std::memcmp(native_planes_.back().data(),planes,4*sizeof(plane)) != 0) {
        std::array<plane,4> snapshot;
        std::copy_n(planes,4,snapshot.begin());
        native_planes_.push_back(snapshot);
    }
    NativeCandidate candidate;
    std::copy_n(vertices,count,candidate.vertices);
    candidate.count=uint8_t(count);candidate.z=z;
    candidate.planes=uint32_t(native_planes_.size()-1);
    native_candidates_.push_back(candidate);
    native_count_valid_=false;
}
uint32_t Geo::native_polygon_count() {
    if (!lazy_native_active_) return uint32_t(polys.size());
    if (!native_count_valid_) {
        ++native_count_computations;
        native_count_=0;native_min_z_=0xffff;native_max_z_=0;
        for (const auto &candidate:native_candidates_) {
            GeoVertex storage[2][8];
            GeoVertex *in=storage[0],*out=storage[1];
            std::copy_n(candidate.vertices,candidate.count,in);
            int count=candidate.count;
            for (const auto &plane:native_planes_[candidate.planes]) {
                if (!count) break;
                count=clip_polygon(in,count,out,plane);
                std::swap(in,out);
            }
            if (count>2) {
                if (++native_count_>=32768) throw GeoFatal("SEGA 3D: Max polygon limit exceeded");
                native_min_z_=std::min(native_min_z_,candidate.z);
                native_max_z_=std::max(native_max_z_,candidate.z);
            }
        }
        if (verify_lazy_native && (native_count_!=polys.size() ||
            native_count_!=raster_.poly_list_index || native_min_z_!=raster_.min_z ||
            native_max_z_!=raster_.max_z))
            throw GeoFatal("Deferred native count/depth differs from original clipper");
        raster_.poly_list_index=native_count_;
        raster_.min_z=native_min_z_;raster_.max_z=native_max_z_;
        native_count_valid_=true;
    }
    return native_count_;
}
static int32_t clip_polygon_classified(GeoVertex *v, int32_t num_vertices, GeoVertex *vout, Geo::plane clip_plane)
{
	    // -1 means the caller can retain its input buffer unchanged.
    float dots[8];
    int inside = 0;
    for (int i = 0; i < num_vertices; ++i) {
        dots[i] = dot_product(v[i], clip_plane.normal);
        inside += dots[i] >= clip_plane.distance;
    }
    if (inside == num_vertices) return -1;
    if (!inside) return 0;
    int32_t outcount = 0;

	const GeoVertex *cur = v;
	GeoVertex *out = vout;

	float curdot = dots[0];
	int32_t curin = (curdot >= clip_plane.distance) ? 1 : 0;

	for (int32_t i = 0; i < num_vertices; i++)
	{
		const int32_t nextvert = (i + 1 == num_vertices) ? 0 : i + 1;

		/* if the current point is inside the plane, add it */
		if (curin)
			out[outcount++] = *cur;

		const float nextdot = dots[nextvert];
		const int32_t nextin = (nextdot >= clip_plane.distance) ? 1 : 0;

		/* Add a clipped vertex if one end of the current edge is inside the plane and the other is outside */
		if ((curin != nextin) && !std::isnan(curdot) && !std::isnan(nextdot))
		{
			const float scale = (clip_plane.distance - curdot) / (nextdot - curdot);

			out[outcount].x = cur->x + ((v[nextvert].x - cur->x) * scale);
			out[outcount].y = cur->y + ((v[nextvert].y - cur->y) * scale);
			out[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
			out[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
			out[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
			outcount++;
		}

		curdot = nextdot;
		curin = nextin;
		cur++;
	}

	return outcount;
}

#endif

inline bool Geo::check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z)
{
	/* if doubleside is disabled */
	if (((attr >> 17) & 1) == 0)
	{
		/* if it's the backface, cull it */
		if (raster->command_buffer[9] & 0x00800000)
			return true;
	}

	/* if the linktype is 0, then we can also cull it */
	if (((attr >> 8) & 3) == 0)
		return true;

	/* if the minimum z value is bigger than the master z clip value, don't render */
	if (raster->master_z_clip != 0xff && (int32_t)(1.0 / min_z) > raster->master_z_clip)
		return true;

	/* if the maximum z value is < 0 then we can safely clip the entire polygon */
	if (max_z < 0)
		return true;

	return false;
}

#if defined(__3DS__)
// Read the same wrapped u16 words, avoiding a copied generic cursor per word.
// Callback-backed ROMs and missing memory retain the original reader/errors.
static void read_texture_words(GeoPtr16 cursor,uint16_t *out,unsigned count) {
    if(cursor.base && cursor.size) {
        for(unsigned i=0;i<count;++i) out[i]=cursor.base[(cursor.i+i)&(cursor.size-1)];
        if(geo_options_3ds::verify_texture_reads) for(unsigned i=0;i<count;++i) {
            ++geo_options_3ds::texture_read_checks;
            if(out[i]!=*cursor++) throw GeoFatal("Direct texture words differ from original reader");
        }
    } else {
        for(unsigned i=0;i<count;++i) out[i]=*cursor++;
    }
}
#endif

template <unsigned NumVerts>
void Geo::model2_3d_process_polygon(raster_state *raster, uint32_t attr)
{
#if defined(__3DS__)
    const bool profile_polygon=geo_profile_3ds::active &&
        (++geo_profile_3ds::stats.polygons, geo_profile_3ds::dense || geo_profile_3ds::stats.polygons % 64 == 1);
    if(profile_polygon) ++geo_profile_3ds::stats.polygon_samples;
    geo_profile_3ds::Scope profile_scope(profile_polygon,geo_profile_3ds::stats.polygon_ticks);
#endif
#if defined(__3DS__)
    PolygonScratchLease3ds workspace(geo_options_3ds::reuse_polygon_scratch);
    std::optional<quad_m2> local_object;
    if(!workspace.scratch) local_object.emplace();
    quad_m2 &object=workspace.scratch?workspace.scratch->object:*local_object;
#else
    quad_m2 object;
#endif
	GeoPtr16 th, tp;
	int32_t tho;
	uint32_t i;
	bool cull;
	float zvalue;
	float min_z, max_z;

	static_assert(NumVerts == 3 || NumVerts == 4, "Polygon must have 3 or 4 vertices");

	/* extract P0(n-1) */
	object.v[1].x = u2f(raster->command_buffer[2] << 8);
	object.v[1].y = u2f(raster->command_buffer[3] << 8);
	object.v[1].pz = u2f(raster->command_buffer[4] << 8);

	/* extract P1(n-1) */
	object.v[0].x = u2f(raster->command_buffer[5] << 8);
	object.v[0].y = u2f(raster->command_buffer[6] << 8);
	object.v[0].pz = u2f(raster->command_buffer[7] << 8);

	/* extract P0(n) */
	object.v[2].x = u2f(raster->command_buffer[11] << 8);
	object.v[2].y = u2f(raster->command_buffer[12] << 8);
	object.v[2].pz = u2f(raster->command_buffer[13] << 8);

	if (NumVerts == 4)
	{
		/* extract P1(n) */
		object.v[3].x = u2f(raster->command_buffer[14] << 8);
		object.v[3].y = u2f(raster->command_buffer[15] << 8);
		object.v[3].pz = u2f(raster->command_buffer[16] << 8);
	}
	else
	{
		/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
		raster->command_buffer[14] = raster->command_buffer[11];
		raster->command_buffer[15] = raster->command_buffer[12];
		raster->command_buffer[16] = raster->command_buffer[13];
	}

	/* always calculate the min z and max z value */
	min_z = object.v[0].pz;
	if (object.v[1].pz < min_z) min_z = object.v[1].pz;
	if (object.v[2].pz < min_z) min_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz < min_z) min_z = object.v[3].pz;

	max_z = object.v[0].pz;
	if (object.v[1].pz > max_z) max_z = object.v[1].pz;
	if (object.v[2].pz > max_z) max_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz > max_z) max_z = object.v[3].pz;

	/* determine whether we can cull this polygon */
	cull = check_culling(raster,attr,min_z,max_z);
    if (capture_reference_depth_ && !cull) for (uint32_t vertex = 0; vertex < NumVerts; ++vertex) {
        const float depth = object.v[vertex].pz;
        if (std::isfinite(depth) && depth > 0) render_reference_depths_.push_back(depth);
    }

	/* set the object's z value */
	switch ((attr >> 10) & 3)
	{
		case 0: // old value
			zvalue = raster->polygon_z;
			break;
		case 1: // min z
			zvalue = min_z;
			break;
		case 2: // max z
			zvalue = max_z;
			break;
		case 3: // error
		default:
			zvalue = 1e10;
			break;
	}

	raster->polygon_z = zvalue;

    bool beyond_render_distance = render_distance_ > 0;
    if (beyond_render_distance) {
        for (uint32_t vertex = 0; vertex < NumVerts; ++vertex) {
            const float depth = object.v[vertex].pz;
            if (!std::isfinite(depth) || depth <= render_distance_) {
                beyond_render_distance = false;
                break;
            }
        }
    }
    const bool omit_for_distance = beyond_render_distance && !record_unclipped;
    if (omit_for_distance && !cull) ++render_distance_culled_;

	/* Culled and far polygons still consume their texture cursors, but their
	 * texture data, LOD and shading state are not needed if raster output is skipped. */
	tho = (attr >> 12) & 0x1f;
	if (tho & 0x10) tho |= -16;
	if (cull || omit_for_distance) {
		raster->command_buffer[0] += NumVerts * 2;
		raster->command_buffer[1] += tho * 4;
	} else {
		/* texture point data */
		if (raster->command_buffer[0] & 0x800000)
			tp = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[0] & 0xffff};
		else
			tp = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1,
			              raster->command_buffer[0] & raster->texture_rom_mask,
			              raster->texture_read, raster->texture_context};
#if defined(__3DS__)
        if(geo_options_3ds::direct_texture_reads) {
            uint16_t words[NumVerts*2];
            read_texture_words(tp,words,NumVerts*2);
            for(unsigned v=0;v<NumVerts;++v) {
                object.v[v].pv=words[v*2];object.v[v].pu=words[v*2+1];
            }
        } else
#endif
        {
		object.v[0].pv = *tp++;
		object.v[0].pu = *tp++;
		object.v[1].pv = *tp++;
		object.v[1].pu = *tp++;
		object.v[2].pv = *tp++;
		object.v[2].pu = *tp++;
		if (NumVerts == 4) { object.v[3].pv = *tp++; object.v[3].pu = *tp++; }
        }
		raster->command_buffer[0] += NumVerts * 2;

		/* texture header data */
		if (raster->command_buffer[1] & 0x800000)
			th = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[1] & 0xffff};
		else
			th = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1,
			              raster->command_buffer[1] & raster->texture_rom_mask,
			              raster->texture_read, raster->texture_context};
#if defined(__3DS__)
        if(geo_options_3ds::direct_texture_reads) read_texture_words(th,object.texheader,4);
        else
#endif
        {
		object.texheader[0] = *th++; object.texheader[1] = *th++;
		object.texheader[2] = *th++; object.texheader[3] = *th++;
        }
		raster->command_buffer[1] += tho * 4;

		object.luma = (raster->command_buffer[9] >> 15) & 0xff;
		object.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
		object.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];
	}

	if (!cull && record_unclipped) {
        GeoPoly poly;
        GeoVertex captured_in[8], captured_out[8];
        int captured_count = NumVerts;
        for (int i = 0; i < NumVerts; ++i) captured_in[i] = object.v[i];
        if (record_vertical_clip) for (int plane = 2; plane < 4; ++plane) {
            captured_count = clip_polygon(captured_in, captured_count, captured_out, raster->clip_plane[raster->center_sel][plane]);
            for (int i = 0; i < captured_count; ++i) captured_in[i] = captured_out[i];
        }
        poly.num_vertices = captured_count;
        poly.center[0] = raster->center[raster->center_sel][0];
        poly.center[1] = raster->center[raster->center_sel][1];
        poly.window = raster->cur_window;
        for (int i = 0; i < 4; ++i) poly.texheader[i] = object.texheader[i];
        for (int i = 0; i < captured_count; ++i) poly.v[i] = captured_in[i];
        if (captured_count > 2) unclipped_polys.push_back(poly);
    }
	// The second clipping pass is presentation-only: it must not alter the
	// polygon count or depth bounds returned to the original game.
#if defined(__3DS__)
    bool native_unchanged = false;
    if (lazy_native_active_ && !cull && !omit_for_distance) {
        retain_native_candidate(object.v,NumVerts,float_to_zval(zvalue,raster->z_adjust),
                                raster->clip_plane[raster->center_sel]);
        // Most frames need no native count. Preserve the original hard limit
        // even if the game never reads the register on a pathological list.
        if (!verify_lazy_native && native_candidates_.size()>=32768) native_polygon_count();
    }
#endif
	if (cull == false && !omit_for_distance)
	for (int wide = 0; wide <= int(capture_widescreen); ++wide)
	{
#if defined(__3DS__)
        if (!wide && lazy_native_active_ && !verify_lazy_native) continue;
#endif
        const bool expand = wide && raster->viewport[2] - raster->viewport[0] >= 400;
#if defined(__3DS__)
        bool pass_unchanged = true;
        // Only the wide presentation list may bypass CPU clipping. Keep the
        // native list exact for board feedback, and retain the original path
        // for any polygon crossing the camera or containing invalid values.
        // Keep quads on the original clipper: clipping before fan splitting can
        // change their UV interpolation. The stereo-off regression capture
        // differed at 19,167 pixels for quads, versus 32 one-level differences
        // when only triangles used hardware clipping (2026-10-15).
        bool hardware_clip = NumVerts == 3 && wide && gpu_presentation_clip &&
            !record_pushes && !record_unclipped && !applying_model_overrides_ &&
            model_overrides.empty() && track_reference_models.empty() && extra_track_objects.empty();
        if (hardware_clip) {
            for (unsigned v = 0; v < NumVerts; ++v)
                if (!(object.v[v].pz > 0) || !std::isfinite(object.v[v].pz) ||
                    !std::isfinite(object.v[v].x) || !std::isfinite(object.v[v].y))
                    hardware_clip = false;
        }
        if (wide && gpu_presentation_clip) {
            if (hardware_clip) ++gpu_clip_submitted;
            else ++gpu_clip_fallback;
        }
#endif
		int32_t clipped_verts;
#if defined(__3DS__)
        std::optional<std::array<std::array<GeoVertex,8>,2>> local_storage;
        auto &storage=workspace.scratch?workspace.scratch->clipped:local_storage.emplace();
        GeoVertex *verts_in=storage[0].data(),*verts_out=storage[1].data();
#else
        GeoVertex storage[2][8];
        GeoVertex *verts_in=storage[0],*verts_out=storage[1];
#endif

		for (int i = 0; i < NumVerts; i++)
			verts_in[i] = object.v[i];

		clipped_verts = NumVerts;

        {
#if defined(__3DS__)
        if(profile_polygon) ++geo_profile_3ds::stats.clip_samples[wide];
        geo_profile_3ds::Scope clip_scope(profile_polygon,geo_profile_3ds::stats.clip_ticks[wide]);
#endif
        /* Hardware clipping is presentation-only; the native pass always runs. */
#if defined(__3DS__)
        if (!hardware_clip)
#endif
        for (int i = 0; i < 4 && clipped_verts > 0; i++)
		{
			auto plane = raster->clip_plane[raster->center_sel][i];
            if (expand && i < 2) {
                const float distance = i == 0 ? float(raster->center[raster->center_sel][0] - raster->viewport[0] + 83) :
                    float(raster->viewport[2] - raster->center[raster->center_sel][0] + 83);
#if defined(__3DS__)
                auto &cached = raster->wide_clip_cache[raster->center_sel][i];
                if (!cached.valid || cached.distance != distance) {
                    const float plane_length = std::hypot(1.0f, distance);
                    cached.normal_x = (i == 0 ? 1.0f : -1.0f) / plane_length;
                    cached.normal_z = distance / plane_length;
                    cached.distance = distance;
                    cached.valid = true;
                }
                plane.normal.x = cached.normal_x;
                plane.normal.pz = cached.normal_z;
#else
                const float plane_length = std::hypot(1.0f, distance);
                plane.normal.x = (i == 0 ? 1.0f : -1.0f) / plane_length;
                plane.normal.pz = distance / plane_length;
#endif
            }
#if defined(__3DS__)
            if (fast_clip_accept) {
                // The vertical planes are identical in both views. Reuse their
                // native acceptance only while the wide pass still has exactly
                // the original vertices. Wide horizontal planes are checked
                // normally; unchanged vertex count alone is never sufficient.
                const bool reuse_native = wide && native_unchanged && pass_unchanged && (i >= 2 || !expand);
                const int count = reuse_native ? -1 : clip_polygon_classified(verts_in, clipped_verts, verts_out, plane);
                if (verify_fast_clip) {
                    GeoVertex reference[16];
                    const int reference_count = clip_polygon(verts_in, clipped_verts, reference, plane);
                    const int actual_count = count < 0 ? clipped_verts : count;
                    const GeoVertex *actual = count < 0 ? verts_in : verts_out;
                    if (actual_count != reference_count ||
                        std::memcmp(actual, reference, actual_count * sizeof(GeoVertex)))
                        throw GeoFatal("Fast clipping differs from reference");
                }
                if (count < 0) continue;
                pass_unchanged = false;
                clipped_verts = count;
            } else
#endif
                clipped_verts = clip_polygon(verts_in, clipped_verts, verts_out, plane);
            std::swap(verts_in, verts_out);
		}
        }
#if defined(__3DS__)
        if (!wide) native_unchanged = fast_clip_accept && pass_unchanged && clipped_verts > 2;
#endif

		if (clipped_verts > 2)
		{
			/* adjust and set the object z-sort value */
			object.z = float_to_zval(zvalue, raster->z_adjust);

			/* go through the clipped vertex list, adding polygons */
			if (!wide) raster->poly_list_index++;
			if (raster->poly_list_index >= 32768) // MAME MAX_POLYGONS
				throw GeoFatal("SEGA 3D: Max polygon limit exceeded");
			auto &output = wide ? wide_polys : polys;
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
            // Wait until the first real polygon, after startup self-tests free
            // their temporary buffers. Avoid holding the old 400 KiB list and
            // a doubled 800 KiB list at the same time on the 8 MiB N64.
            if (!output.capacity()) output.reserve(4096);
#endif
            output.emplace_back();
			GeoPoly *poly = &output.back();
			poly->reverse = raster->reverse;

			/* copy the object information */
			poly->z = object.z;
			poly->texheader[0] = object.texheader[0];
			poly->texheader[1] = object.texheader[1];
			poly->texheader[2] = object.texheader[2];
			poly->texheader[3] = object.texheader[3];
			poly->luma = object.luma;
			poly->texlod = object.texlod;

			/* set the viewport */
			poly->viewport[0] = raster->viewport[0] - (expand ? 83 : 0);
			poly->viewport[1] = raster->viewport[1];
			poly->viewport[2] = raster->viewport[2] + (expand ? 83 : 0);
			poly->viewport[3] = raster->viewport[3];

			/* set the center */
			poly->center[0] = raster->center[raster->center_sel][0];
			poly->center[1] = raster->center[raster->center_sel][1];

			/* set the window */
			poly->window = raster->cur_window;

			poly->num_vertices = clipped_verts;

			for (int i = 0; i < clipped_verts; i++)
				poly->v[i] = verts_in[i];

			// (MAME links it into a per-z bucket here; the display list keeps
			// the order added, from which the bucket order follows.)

			/* keep around the min and max z values for this frame */
			if (!wide && object.z < raster->min_z) raster->min_z = object.z;
			if (!wide && object.z > raster->max_z) raster->max_z = object.z;
		}
	}

	/* update linking */
	switch (((attr >> 8) & 3))
	{
		case 0:
		case 2:
		{
			/* reuse P0(n) and P1(n) */
			for (i = 0; i < 6; i++)                                        /* P0(n) -> P0(n-1) */
				raster->command_buffer[2+i] = raster->command_buffer[11+i]; /* P1(n) -> P1(n-1) */
		}
		break;

		case 1:
		{
			/* reuse P0(n-1) and P0(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[5+i] = raster->command_buffer[11+i]; /* P0(n) -> P1(n-1) */
		}
		break;

		case 3:
		{
			/* reuse P1(n-1) and P1(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[2+i] = raster->command_buffer[14+i]; /* P1(n) -> P1(n-1) */
		}
		break;
	}
}

/***********************************************************************************************/


/* 3D Rasterizer frame start: Resets frame variables */
void Geo::render_frame_start()
{
	raster_state *raster = &raster_;
	render_distance_culled_ = 0;

	/* reset the polygon list index */
	raster->poly_list_index = 0;
    polys.clear();
#if defined(__3DS__)
    gpu_clip_submitted = gpu_clip_fallback = 0;
    lazy_native_active_ = lazy_native_count && lazy_native_allowed && capture_widescreen &&
        !gpu_presentation_clip && !record_pushes && !record_unclipped && !record_objects &&
        !applying_model_overrides_ && model_overrides.empty() &&
        track_reference_models.empty() && extra_track_objects.empty();
    native_candidates_.clear();native_planes_.clear();native_count_valid_=false;
#endif
    unclipped_polys.clear();
	wide_polys.clear();
	object_submissions.clear();
    overridden_objects = 0;
	extended_polys.clear(); track_geo_.reset(); track_raster_.reset();
	pushed.clear();
    render_distance_culled_ = 0;
    render_reference_depth_ = 0;
    render_reference_depths_.clear();

	/* reset the min-max sortable Z values */
	raster->min_z = 0xffff;
	raster->max_z = 0;

	/* reset the polygon z value */
	// Zero Gunner sets backgrounds with "previous z value" mode at the start of the display list,
	// needs this to be this big in order to work properly
	raster->polygon_z = 1e10;

	raster->cur_window = 0;
}

/* 3D Rasterizer main data input port */
void Geo::model2_3d_push(raster_state *raster, uint32_t input)
{
	if (record_pushes) pushed.push_back(input);
	/* see if we have a command in progress */
	if (raster->cur_command != 0)
	{
		raster->command_buffer[raster->command_index++] = input;

		switch (raster->cur_command)
		{
			case 0x00:  /* NOP */
			break;

			case 0x01:  /* Polygon Data */
			{
				uint32_t  attr;

				/* start by looking if we have the basic input data */
				if (raster->command_index < 9)
					return;

				/* get the attributes */
				attr = raster->command_buffer[8];

				/* see if we're done */
				if ((attr & 3) == 0)
				{
					raster->cur_command = 0;
					return;
				}

				/* see if it's a quad or a triangle */
				if (attr & 1)
				{
					/* it's a quad, wait for the rest of the points */
					if (raster->command_index < 17)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<4>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
				else
				{
					/* it's a triangle, wait for the rest of the point */
					if (raster->command_index < 14)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<3>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
			}
			break;

			case 0x03:  /* Window Data */
			{
				uint32_t  i;

				/* make sure we have all the data */
				if (raster->command_index < 6)
					return;

				/* coordinates are 12 bit signed */

				/* extract the viewport start x */
				raster->viewport[0] = (raster->command_buffer[0] >> 12) & 0xfff;

				if (raster->viewport[0] & 0x800)
					raster->viewport[0] = -(0x800 - (raster->viewport[0] & 0x7ff));

				/* extract the viewport start y */
				raster->viewport[1] = raster->command_buffer[0] & 0xfff;

				if (raster->viewport[1] & 0x800)
					raster->viewport[1] = -(0x800 - (raster->viewport[1] & 0x7ff));

				/* extract the viewport end x */
				raster->viewport[2] = (raster->command_buffer[1] >> 12) & 0xfff;

				if (raster->viewport[2] & 0x800)
					raster->viewport[2] = -(0x800 - (raster->viewport[2] & 0x7ff));

				/* extract the viewport end y */
				raster->viewport[3] = raster->command_buffer[1] & 0xfff;

				if (raster->viewport[3] & 0x800)
					raster->viewport[3] = -(0x800 - (raster->viewport[3] & 0x7ff));

				/* extract the centers */
				for (i = 0; i < 4; i++)
				{
					/* center x */
					raster->center[i][0] = (raster->command_buffer[2+i] >> 12) & 0xfff;

					if (raster->center[i][0] & 0x800)
						raster->center[i][0] = -(0x800 - (raster->center[i][0] & 0x7ff));

					/* center y */
					raster->center[i][1] = raster->command_buffer[2+i] & 0xfff;

					if (raster->center[i][1] & 0x800)
						raster->center[i][1] = -(0x800 - (raster->center[i][1] & 0x7ff));

					// calculate clipping planes
					float left_plane = float(raster->center[i][0] - raster->viewport[0]);
					float right_plane = float(raster->viewport[2] - raster->center[i][0]);
					float top_plane = float(raster->viewport[3] - raster->center[i][1]);
					float bottom_plane = float(raster->center[i][1] - raster->viewport[1]);

					const float left_length = std::hypot(1.0f, left_plane);
					const float right_length = std::hypot(-1.0f, right_plane);
					const float top_length = std::hypot(-1.0f, top_plane);
					const float bottom_length = std::hypot(1.0f, bottom_plane);
					raster->clip_plane[i][0].normal.x = 1.0f / left_length;
					raster->clip_plane[i][0].normal.y = 0.0f;
					raster->clip_plane[i][0].normal.pz = left_plane / left_length;

					raster->clip_plane[i][1].normal.x = -1.0f / right_length;
					raster->clip_plane[i][1].normal.y = 0.0f;
					raster->clip_plane[i][1].normal.pz = right_plane / right_length;

					raster->clip_plane[i][2].normal.x = 0.0f;
					raster->clip_plane[i][2].normal.y = -1.0f / top_length;
					raster->clip_plane[i][2].normal.pz = top_plane / top_length;

					raster->clip_plane[i][3].normal.x = 0.0f;
					raster->clip_plane[i][3].normal.y = 1.0f / bottom_length;
					raster->clip_plane[i][3].normal.pz = bottom_plane / bottom_length;
				}

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			case 0x04:  /* Texture/Log Data write */
			{
				/* make sure we have enough data */
				if (raster->command_index < 2)
					return;

				/* see if the count is non-zero */
				if (raster->command_buffer[1] > 0)
				{
					/* see if we have data available */
					if (raster->command_index >= 3)
					{
						/* get the address */
						uint32_t  address = raster->command_buffer[0];

						/* do the write */
						if (address & 0x800000)
							raster->texture_ram[address & 0xffff] = raster->command_buffer[2];
						else
							raster->log_ram[address & 0x7fff] = raster->command_buffer[2];

						/* increment the address and decrease the count */
						raster->command_buffer[0]++;
						raster->command_buffer[1]--;

						/* decrease the index, so we keep placing data in the same slot */
						raster->command_index--;
					}
				}

				/* see if we're done with this command */
				if (raster->command_buffer[1] == 0)
					raster->cur_command = 0;
			}
			break;

			case 0x08:  /* ZSort mode */
			{
				/* save the zsort mode value */
				raster->z_adjust = raster->command_buffer[0] << 8;

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			default:
			{
				throw GeoFatal("SEGA 3D: Unknown rasterizer command");
			}
		}
	}
	else
	{
		/* new command */
		raster->cur_command = input & 0x0f;
		raster->command_index = 0;

		/* see if it's object data */
		if (raster->cur_command == 1)
		{
			/* extract reverse bit */
			raster->reverse = (input >> 4) & 1;

			/* extract center select */
			raster->center_sel = (input >> 6) & 3;
		}
	}
}

/***********************************************************************************************/


/* Parse Polygons: Normals Present, No Specular case */
inline void Geo::model2_3d_push_point(raster_state *raster, const GeoVertex &point) {
    const uint32_t x = f2u(point.x) >> 8;
    const uint32_t y = f2u(point.y) >> 8;
    const uint32_t z = f2u(point.pz) >> 8;
    const uint32_t index = raster->command_index;
    // In these vertex slots the first two words only populate the buffer.
    // Z still follows the normal path, including triangle/quad completion.
    // Recording keeps the original per-word path as a parity reference.
    if (!record_pushes && raster->cur_command == 1 &&
        (index == 2 || index == 5 || index == 11 || index == 14)) {
        raster->command_buffer[index] = x;
        raster->command_buffer[index + 1] = y;
        raster->command_index = index + 2;
    } else {
        model2_3d_push(raster, x);
        model2_3d_push(raster, y);
    }
    model2_3d_push(raster, z);
}

inline void Geo::model2_3d_push_lit_point(raster_state *raster, uint32_t luma, uint32_t distance,
                                          const GeoVertex &point) {
    const uint32_t index = raster->command_index;
    // After polygon attributes, luma, distance and the first vertex fill slots 9-13.
    // The final Z word still drives the original triangle/quad state transition.
    if (!record_pushes && raster->cur_command == 1 && index == 9) {
        raster->command_buffer[index] = luma;
        raster->command_buffer[index + 1] = distance;
        raster->command_buffer[index + 2] = f2u(point.x) >> 8;
        raster->command_buffer[index + 3] = f2u(point.y) >> 8;
        raster->command_index = index + 4;
        model2_3d_push(raster, f2u(point.pz) >> 8);
        return;
    }
    model2_3d_push(raster, luma);
    model2_3d_push(raster, distance);
    model2_3d_push_point(raster, point);
}

inline bool Geo::polygon_beyond_render_distance(const raster_state *raster, const GeoVertex &third,
                                                const GeoVertex *fourth) const {
    if (render_distance_ <= 0 || record_unclipped || record_pushes) return false;
    const auto beyond = [this](float depth) {
        const uint32_t encoded = f2u(depth) >> 8;
        const float raster_depth = u2f(encoded << 8);
        return std::isfinite(raster_depth) && raster_depth > render_distance_;
    };
    return beyond(u2f(raster->command_buffer[4] << 8)) &&
           beyond(u2f(raster->command_buffer[7] << 8)) && beyond(third.pz) &&
           (!fourth || beyond(fourth->pz));
}

void Geo::geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t               luma;
			texture_parameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			GeoVertex fourth_point{};
			const bool quad = (attr & 1) != 0;
			if (quad) {
				fourth_point.x = u2f(*input++);
				fourth_point.y = u2f(*input++);
				fourth_point.pz = u2f(*input++);
				transform_point(&fourth_point, geo->matrix);
				apply_focus(geo, &fourth_point);
			}
			const bool skip_lighting = polygon_beyond_render_distance(raster, point, quad ? &fourth_point : nullptr);
			if (!skip_lighting) transform_vector(&normal, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
			dotl = skip_lighting ? 0.0f : dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = skip_lighting ? 0.0f : dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (!skip_lighting && dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if (skip_lighting || (dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = skip_lighting ? 0 : (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = skip_lighting ? 0 : coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push_lit_point(raster, luma << 15, f2u(distance) >> 8, point);

			/* if it's a quad, push one more point */
			if (quad)
			{
				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, fourth_point);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: Normals Present, Specular case */
void Geo::geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			GeoVertex fourth_point{};
			const bool quad = (attr & 1) != 0;
			if (quad) {
				fourth_point.x = u2f(*input++);
				fourth_point.y = u2f(*input++);
				fourth_point.pz = u2f(*input++);
				transform_point(&fourth_point, geo->matrix);
				apply_focus(geo, &fourth_point);
			}
			const bool skip_lighting = polygon_beyond_render_distance(raster, point, quad ? &fourth_point : nullptr);
			if (!skip_lighting) transform_vector(&normal, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
            // Light-vector dot product is deferred until visibility is known.

			/* calculate the dot product of the normal and the point */
			dotp = skip_lighting ? 0.0f : dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (!skip_lighting && dotp >= 0) face = 0; /* front */

#if defined(__3DS__)
            const bool omit_lighting=geo_options_3ds::skip_culled_lighting &&
                !record_pushes && !record_unclipped &&
                ((((attr >> 8) & 3) == 0) || (!(attr & (1u << 17)) && face != 0));
            if(omit_lighting) {
                // check_culling consumes this face bit. Luma and LOD are never
                // read for these rejected polygons; keep the normal push path
                // so depth feedback, cursors and linked vertices still advance.
                luma=int32_t(face);
                distance=0;
            } else
#endif
            {
            dotl = skip_lighting ? 0.0f : dot_product(normal, geo->light);
			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if (skip_lighting || (dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = skip_lighting ? 0 : ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = skip_lighting ? 0 : (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = skip_lighting ? 0 : coef * fabs(dotp) * geo->lod;
            }

			/* push to the 3d rasterizer */
			model2_3d_push_lit_point(raster, luma << 15, f2u(distance) >> 8, point);

			/* if it's a quad, push one more point */
			if (quad)
			{
				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, fourth_point);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, No Specular case */
void Geo::geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push_lit_point(raster, luma << 15, f2u(distance) >> 8, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, Specular case */
void Geo::geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push_point(raster, point);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push_lit_point(raster, luma << 15, f2u(distance) >> 8, point);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push_point(raster, point);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/*******************************************
 *
 *  Geometry Engine Commands
 *
 *******************************************/

/* Command 00: NOP */
GeoPtr Geo::geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);

	return input;
}

/* Command 01: Object Data */
GeoPtr Geo::geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */
	uint32_t  oba = *input++;     /* Object Address */
	uint32_t  obc = *input++;     /* Object Count */
    if (applying_model_overrides_) {
        for (const auto &replacement : model_overrides) {
            if (oba == replacement.model && tpa == replacement.points && tha == replacement.headers) {
                oba = replacement.replacement; tpa = replacement.replacement_points;
                tha = replacement.replacement_headers; ++overridden_objects; break;
            }
        }
    }
	if (!track_geo_ && !extra_track_objects.empty() &&
		std::find(track_reference_models.begin(), track_reference_models.end(), oba) != track_reference_models.end()) {
		track_geo_ = std::make_unique<geo_state>(*geo);
		track_raster_ = std::make_unique<raster_state>(*raster);
	}
	if (record_objects) {
		ObjectSubmission submission{oba, obc, tpa, tha, {}};
		std::copy(std::begin(geo->matrix), std::end(geo->matrix), submission.matrix);
		object_submissions.push_back(submission);
	}

	GeoPtr obp;                /* Object Pointer */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* select where we're reading polygon information from */
	if (oba & 0x01000000)
	{
		/* Fast polygon RAM */
		obp = GeoPtr{geo->polygon_ram1, 0x8000, oba & 0x7fff};
	}
	else if (oba & 0x00800000)
	{
		/* Polygon ROM */
		obp = GeoPtr{const_cast<uint32_t *>(geo->polygon_rom), geo->polygon_rom_mask + 1,
		             oba & geo->polygon_rom_mask, geo->polygon_read, geo->polygon_context};
	}
	else
	{
		/* Slow Polygon RAM */
		obp = GeoPtr{geo->polygon_ram0, 0x8000, oba & 0x7fff};
	}

	// if count == 0 then rolls over to max size
	// Virtual On & Gunblade NY
	if (obc == 0)
		obc = 0xfffff;

#if defined(__3DS__)
    const unsigned profile_mode=geo->mode & 3;
    if(geo_profile_3ds::active) ++geo_profile_3ds::stats.object_count[profile_mode];
    geo_profile_3ds::Scope object_scope(geo_profile_3ds::active,geo_profile_3ds::stats.object_ticks[profile_mode]);
#endif
#if defined(__3DS__)
    struct FarPrediction {
        std::array<uint32_t,32> words;
        float z;
        uint64_t culled;
        size_t native_size,wide_size;
        uint32_t poly_index,end_index;
        uint16_t min_z,max_z;
    };
    std::optional<FarPrediction> far_prediction;
    if(geo_options_3ds::far_model_distance && (geo->mode&3)<2 &&
       (oba&0x01800000)==0x00800000 && geo->polygon_rom && !geo->polygon_read &&
       raster->cur_command==1 && raster->command_index==2 && raster->master_z_clip==255 &&
       !record_pushes && !record_unclipped && !capture_reference_depth_ &&
       !applying_model_overrides_ && model_overrides.empty() &&
       track_reference_models.empty() && extra_track_objects.empty() && render_distance_>0) {
        const auto &model=far_models_3ds::lookup(geo->polygon_rom,geo->polygon_rom_mask,oba&geo->polygon_rom_mask,obc);
        if(far_models_3ds::entirely_beyond(model,geo->matrix,render_distance_)) {
            auto &p=far_prediction.emplace();
            std::copy(std::begin(raster->command_buffer),std::end(raster->command_buffer),p.words.begin());
            auto transformed=[&](const far_models_3ds::Point &raw) {
                GeoVertex vertex{raw[0],raw[1],{raw[2],0,0}};
                transform_point(&vertex,geo->matrix);apply_focus(geo,&vertex);return vertex;
            };
            for(unsigned i=0;i<4;++i) {
                const auto vertex=transformed(model.tail[i]);
                const unsigned index=i<2?2+3*i:11+3*(i-2);
                p.words[index]=f2u(vertex.x)>>8;p.words[index+1]=f2u(vertex.y)>>8;
                p.words[index+2]=f2u(vertex.pz)>>8;
            }
            p.words[0]+=model.texture_words;p.words[1]+=model.header_words;
            p.words[8]=model.end_attribute;p.words[9]=0x00800000;p.words[10]=0;
            p.z=raster->polygon_z;
            if(model.sort_mode==3)p.z=1e10;
            else if(model.sort_mode) {
                float depth=u2f(f2u(transformed(model.sort_vertices[0]).pz)&0xffffff00u);
                for(unsigned v=1;v<model.sort_count;++v) {
                    const float next=u2f(f2u(transformed(model.sort_vertices[v]).pz)&0xffffff00u);
                    if(model.sort_mode==1?next<depth:next>depth)depth=next;
                }
                p.z=depth;
            }
            p.culled=render_distance_culled_+model.distance_culled;
            p.native_size=polys.size();p.wide_size=wide_polys.size();
            p.poly_index=raster->poly_list_index;p.min_z=raster->min_z;p.max_z=raster->max_z;
            p.end_index=model.end_index;
            ++geo_options_3ds::far_models;geo_options_3ds::far_polygons+=model.polygons;
            if(!geo_options_3ds::verify_far_model_distance) {
                std::copy(p.words.begin(),p.words.end(),std::begin(raster->command_buffer));
                raster->polygon_z=p.z;raster->cur_command=0;raster->command_index=p.end_index;
                render_distance_culled_=p.culled;
                return input;
            }
        }
    }
#endif
	switch (geo->mode & 3)
	{
		/* Normals present, No Specular */
		case 0: geo_parse_np_ns(geo, obp, obc); break;

		/* Normals present, Specular */
		case 1: geo_parse_np_s(geo, obp, obc); break;

		/* No Normals present, No Specular */
		case 2: geo_parse_nn_ns(geo, obp, obc); break;

		/* No Normals present, Specular */
		case 3: geo_parse_nn_s(geo, obp, obc); break;
	}

#if defined(__3DS__)
    if(far_prediction) {
        const auto &p=*far_prediction;
        if(!std::equal(p.words.begin(),p.words.end(),std::begin(raster->command_buffer)) ||
           f2u(p.z)!=f2u(raster->polygon_z) || p.culled!=render_distance_culled_ ||
           p.native_size!=polys.size() || p.wide_size!=wide_polys.size() ||
           p.poly_index!=raster->poly_list_index || p.min_z!=raster->min_z || p.max_z!=raster->max_z ||
           raster->cur_command!=0 || p.end_index!=raster->command_index)
            throw GeoFatal("Far model shortcut differs from original parser state");
        ++geo_options_3ds::far_verified;
    }
#endif
	/* move by 4 parameters */
	return input;
}

/* Command 02: Direct Data */
GeoPtr Geo::geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, (opcode >> 23) - 1);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* push the initial points */
	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	/* read in the attributes */
	uint32_t  attr;
	while (((attr = *input++) & 3) != 0)
	{
		/* push attributes */
		model2_3d_push(raster, attr & 0x00ffffff);

		/* push luma */
		model2_3d_push(raster, (*input++) >> 8);

		/* push distance */
		model2_3d_push(raster, (*input++) >> 8);

		/* push the next point */
		model2_3d_push(raster, (*input++) >> 8); /* x */
		model2_3d_push(raster, (*input++) >> 8); /* y */
		model2_3d_push(raster, (*input++) >> 8); /* z */

		/* if it's a quad, output another point */
		if (attr & 1)
		{
			model2_3d_push(raster, (*input++) >> 8); /* x */
			model2_3d_push(raster, (*input++) >> 8); /* y */
			model2_3d_push(raster, (*input++) >> 8); /* z */
		}
	}

	/* we're done */
	model2_3d_push(raster, 0);

	return input;
}

/* Command 03: Window Data */
GeoPtr Geo::geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	raster->cur_window++;

	/*
	    we're going to move 6 coordinates to the 3d rasterizer:
	    - starting coordinate
	    - completion coordinate
	    - vanishing point 0 (eye mode 0)
	    - vanishing point 1 (eye mode 1)
	    - vanishing point 2 (eye mode 2)
	    - vanishing point 3 (eye mode 3)
	*/

	for (uint32_t i = 0; i < 6; i++)
	{
		/* read in the coordinate */
		uint32_t y = *input++;

		/* convert to the 3d rasterizer format (00XXXYYY) */
		uint32_t x = (y & 0x0fff0000) >> 4 ;
		y &= 0xfff;

		/* push it */
		model2_3d_push(raster, x | y);
	}

	return input;
}

/* Command 04: Texture Data Write */
GeoPtr Geo::geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	uint32_t count = *input++;

	/* push the count */
	model2_3d_push(raster, count);

	/* loop and send the data */
	for (uint32_t i = 0; i < count; i++)
		model2_3d_push(raster, *input++);

	return input;
}

/* Command 05: Polygon Data */
GeoPtr Geo::geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;
	GeoPtr p;

	(void)opcode;

	/* read in the address */
	address = *input++;

	/* prepare the pointer */
	if (address & 0x01000000)
	{
		/* Fast polygon RAM */
		p = GeoPtr{geo->polygon_ram1, 0x8000, address & 0x7fff};
	}
	else
	{
		/* Slow Polygon RAM */
		p = GeoPtr{geo->polygon_ram0, 0x8000, address & 0x7fff};
	}

	/* read the count */
	count = *input++;

	/* move the data */
	for (i = 0; i < count; i++) {
		p.store(*input++);
		p++;
	}

	return input;
}

/* Command 06: Texture Parameters */
GeoPtr Geo::geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  index, count, i, param;

	(void)opcode;

	/* read in the index */
	index = (*input++) >> 2;

	/* read in the conut */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		/* read in the texture parameters */
		param = *input++;

		geo->texture_parameters[index].diffuse = float(param & 0xff);
		geo->texture_parameters[index].ambient = float((param >> 8) & 0xff);
		geo->texture_parameters[index].specular_control = (param >> 24) & 0xff;
		geo->texture_parameters[index].specular_scale = float((param >> 16) & 0xff);

		/* read in the distance coefficient */
		geo->coef_table[index] = u2f(*input++);

		index = (index + 1) & 0x1f;
	}

	return input;
}

/* Command 07: Geo Mode */
GeoPtr Geo::geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the mode */
	geo->mode = *input++;

	return input;
}

/* Command 08: ZSort Mode */
GeoPtr Geo::geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the mode */
	model2_3d_push(raster, (*input++) >> 8);

	return input;
}

/* Command 09: Focal Distance */
GeoPtr Geo::geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read the x focus value */
	geo->focus.x = u2f(*input++);

	/* read the y focus value */
	geo->focus.y = u2f(*input++);

	return input;
}

/* Command 0A: Light Source Vector Write */
GeoPtr Geo::geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read the x light value */
	geo->light.x = u2f(*input++);

	/* read the y light value */
	geo->light.y = u2f(*input++);

	/* read the z light value */
	geo->light.pz = u2f(*input++);

	return input;
}

/* Command 0B: Transformation Matrix Write */
GeoPtr Geo::geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the transformation matrix */
	for (i = 0; i < 12; i++)
		geo->matrix[i] = u2f(*input++);

	return input;
}

/* Command 0C: Parallel Transfer Vector Write */
GeoPtr Geo::geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the translation vector */
	for (i = 0; i < 3; i++)
		geo->matrix[i+9] = u2f(*input++);

	return input;
}

/* Command 0D: Geo Data Memory Push (undocumented, unsupported) */
GeoPtr Geo::geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;

	/*
	    This command pushes data stored in the Geometry DSP's RAM
	    to the hardware 3D rasterizer. Since we don't emulate the
	    DSP, we don't know what the RAM contents are.

	    Eventually, we could check for the address, and if it
	    happens to point to a polygon ROM, we could potentially
	    emulate it partially.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the address */
	address = *input++;

	/* read in the count */
	count = *input++;


	(void)i;
/*
    for (i = 0; i < count; i++)
        model2_3d_push(0);
*/

	return input;
}

/* Command 0E: Geo Test */
GeoPtr Geo::geo_test(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t      data, blocks, address, count, checksum, i;

	(void)opcode;

	/* fifo test */
	data = 1;

	for (i = 0; i < 32; i++)
	{
		if (*input++ != data)
		{
			/* TODO: Set Red LED on */
				}

		data <<= 1;
	}

	/* get the number of checksums we have to run */
	blocks = *input++;

	for (i = 0; i < blocks; i++)
	{
		uint32_t  sum_even, sum_odd, j;

		/* read in the address */
		address = (*input++) & 0x7fffff;

		/* read in the count */
		count = *input++;

		/* read in the checksum */
		checksum = *input++;

		/* reset the checksum counters */
		sum_even = 0;
		sum_odd = 0;

		for (j = 0; j < count; j++)
		{
			if (geo->polygon_read) {
				if (!geo->polygon_read(geo->polygon_context, size_t(address) * sizeof(data), &data, sizeof(data)))
					throw GeoFatal("geometrizer failed to read polygon ROM checksum data");
			} else {
				data = geo->polygon_rom[address];
			}
			address++;

			address &= geo->polygon_rom_mask;

			sum_even += data >> 16;
			sum_even &= 0xffff;

			sum_odd += data & 0xffff;
			sum_odd &= 0xffff;
		}

		sum_even += checksum >> 16;
		sum_even &= 0xffff;

		sum_odd += checksum & 0xffff;
		sum_odd &= 0xffff;

		if (sum_even != 0 || sum_odd != 0)
		{
			/* TODO: Set Green LED on */
				}
	}

	return input;
}

/* Command 0F: End */
GeoPtr Geo::geo_end(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	(void)opcode;

	/* signal the end of this data block the rasterizer */
	model2_3d_push(raster, 0xff000000);

	/* signal end by returning nullptr */
	return GeoPtr{};
}

/* Command 10: Dummy */
GeoPtr Geo::geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  data;
	(void)opcode;

	/* do the dummy read cycle */
//  data = *input++;
	input++;

	return input;
}

/* Command 14: Log Data Write */
GeoPtr Geo::geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  i, count;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	count = *input++;

	/* push the count */
	model2_3d_push(raster, count << 2);

	/* loop and send the data */
	for (i = 0; i < count; i++)
	{
		uint32_t  data = *input++;

		model2_3d_push(raster, data & 0xff);
		model2_3d_push(raster, (data >> 8) & 0xff);
		model2_3d_push(raster, (data >> 16) & 0xff);
		model2_3d_push(raster, (data >> 24) & 0xff);
	}

	return input;
}

/* Command 16: LOD */
GeoPtr Geo::geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the LOD */
	geo->lod = u2f(*input++);

	return input;
}

/* Command 1D: Code Upload  (undocumented, unsupported) */
GeoPtr Geo::geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  count, i;

	/*
	    This command uploads code to program memory and
	    optionally runs it. Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the flags */
//  flags = *input++;
	input++;

	/* read in the count */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		[[maybe_unused]] u64  code;

		/* read the top part of the opcode */
		code = *input++;

		code <<= 32;

		/* the bottom part comes in two pieces */
		code |= *input++;
		code |= (*input++) << 16;
	}

	/*
	    Bit 10 of flags indicate whether to run iummediately after upload
	*/

/*
    if (flags & 0x400)
        code_jump();
*/

	return input;
}

/* Command 1E: Code Jump (undocumented, unsupported) */
GeoPtr Geo::geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  address;

	/*
	    This command jumps to a specified address in program
	    memory. Code can be uploaded with function 1D.
	    Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

//  address = *input++ & 0x3ff;
	input++;

/*
    code_jump(address)
*/
	return input;
}

GeoPtr Geo::geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code)
{
	switch ((opcode >> 23) & 0x1f)
	{
		case 0x00: input = geo_nop(geo, opcode, input);                   break;
		case 0x01: input = geo_object_data(geo, opcode, input);           break;
		case 0x02: input = geo_direct_data(geo, opcode, input);           break;
		case 0x03: input = geo_window_data(geo, opcode, input);           break;
		case 0x04: input = geo_texture_data(geo, opcode, input);          break;
		case 0x05: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x06: input = geo_texture_parameters(geo, opcode, input);    break;
		case 0x07: input = geo_mode(geo, opcode, input);                  break;
		case 0x08: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x09: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x0a: input = geo_light_source(geo, opcode, input);          break;
		case 0x0b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x0c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x0d: input = geo_data_mem_push(geo, opcode, input);         break;
		case 0x0e: input = geo_test(geo, opcode, input);                  break;
		case 0x0f: input = geo_end(geo, opcode, input); *end_code = true; break;
		case 0x10: input = geo_dummy(geo, opcode, input);                 break;
		case 0x11: input = geo_object_data(geo, opcode, input);           break;
		case 0x12: input = geo_direct_data(geo, opcode, input);           break;
		case 0x13: input = geo_window_data(geo, opcode, input);           break;
		case 0x14: input = geo_log_data(geo, opcode, input);              break;
		case 0x15: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x16: input = geo_lod(geo, opcode, input);                   break;
		case 0x17: input = geo_mode(geo, opcode, input);                  break;
		case 0x18: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x19: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x1a: input = geo_light_source(geo, opcode, input);          break;
		case 0x1b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x1c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x1d: input = geo_code_upload(geo, opcode, input);           break;
		case 0x1e: input = geo_code_jump(geo, opcode, input);             break;
		case 0x1f: input = geo_end(geo, opcode, input); *end_code = true; break;
	}

	return input;
}

#if defined(__3DS__)
void Geo::parse_snapshot(uint32_t read_start,uint32_t *commands) {
    auto *original=buffer_;
    buffer_=commands;
    try { parse(read_start); }
    catch(...) { buffer_=original;throw; }
    buffer_=original;
}
#endif

void Geo::parse(uint32_t read_start)
{
#if defined(__3DS__)
    geo_profile_3ds::Scope parse_scope(geo_profile_3ds::active,geo_profile_3ds::stats.parse_ticks);
#endif
	uint32_t  address = (read_start & 0x1ffff)/4;
	GeoPtr input = buf(address);
	uint32_t  opcode;
	uint32_t  op_count = 0;
	bool end_code = false;

	// reset raster frame variables
	render_frame_start();

	while (end_code == false && input.i < 0x20000/4 && op_count++ < 0x8000)
	{
		/* read in the opcode */
		opcode = *input++;

		/* if it's a jump opcode, do the jump */
		if (opcode & 0x80000000)
		{
			/* get the address */
			address = (opcode & 0x1ffff) / 4;

			/* update our pointer */
			input = buf(address);

			/* go again */
			continue;
		}

		/* process it */
		input = geo_process_command(&geo_, opcode, input, &end_code);
	}
    if (!render_reference_depths_.empty()) {
        auto middle = render_reference_depths_.begin() + render_reference_depths_.size() / 2;
        std::nth_element(render_reference_depths_.begin(), middle, render_reference_depths_.end());
        render_reference_depth_ = *middle;
    }
}

void Geo::parse_model_presentation(uint32_t read_start, bool track, bool widescreen) {
    // The native parse has already completed. Replay the same display list into
    // private state, substituting only verified body model/texture tuples.
    auto saved_geo = std::make_unique<geo_state>(geo_);
    auto saved_raster = std::make_unique<raster_state>(raster_);
    auto native = std::move(polys), native_wide = std::move(wide_polys);
    auto native_pushes = std::move(pushed);
    auto native_objects = std::move(object_submissions);
    auto native_unclipped = std::move(unclipped_polys);
    auto native_track_geo = std::move(track_geo_);
    auto native_track_raster = std::move(track_raster_);
    const bool objects = record_objects, pushes = record_pushes;
    const bool unclipped = record_unclipped;
    record_unclipped = false;
    record_objects = record_pushes = false;
    applying_model_overrides_ = true; overridden_objects = 0;
    auto restore = [&] {
        polys = std::move(native); wide_polys = std::move(native_wide);
        pushed = std::move(native_pushes); object_submissions = std::move(native_objects);
        unclipped_polys = std::move(native_unclipped); record_unclipped = unclipped;
        geo_ = *saved_geo; raster_ = *saved_raster;
        track_geo_ = std::move(native_track_geo); track_raster_ = std::move(native_track_raster);
        record_objects = objects; record_pushes = pushes; applying_model_overrides_ = false;
    };
    try {
        parse(read_start);
        if (track) extend_track(widescreen);
        if (extended_polys.empty()) extended_polys = widescreen ? wide_polys : polys;
    } catch (...) { extended_polys.clear(); restore(); throw; }
    restore();
}

void Geo::extend_track(bool widescreen) {
    if (!track_geo_ || !track_raster_) return;
    auto saved_geo = std::make_unique<geo_state>(geo_);
    auto saved_raster = std::make_unique<raster_state>(raster_);
    auto native = std::move(polys), native_wide = std::move(wide_polys);
    const bool objects = record_objects, pushes = record_pushes;
    record_objects = record_pushes = false;
    geo_ = *track_geo_; raster_ = *track_raster_; geo_.raster = &raster_;
    // Build only additional presentation faces, then restore every native
    // state field and list used for hashes or feedback to the game.
    try {
    for (const auto &object : extra_track_objects) {
        uint32_t words[] = {object.texture_points, object.texture_headers, object.address, object.count};
        geo_object_data(&geo_, 1u << 23, GeoPtr{words, 4, 0});
    }
    extended_polys = widescreen ? native_wide : native;
    const auto &extra = widescreen ? wide_polys : polys;
    extended_polys.insert(extended_polys.end(), extra.begin(), extra.end());
    } catch (...) {
        polys = std::move(native); wide_polys = std::move(native_wide);
        geo_ = *saved_geo; raster_ = *saved_raster;
        record_objects = objects; record_pushes = pushes;
        extended_polys.clear();
        throw;
    }
    polys = std::move(native); wide_polys = std::move(native_wide);
    geo_ = *saved_geo; raster_ = *saved_raster;
    record_objects = objects; record_pushes = pushes;
}

/***********************************************************************************************/

} // namespace rt
