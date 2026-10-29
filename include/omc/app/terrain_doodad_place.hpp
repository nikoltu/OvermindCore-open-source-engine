#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "omc/app/melee_models.hpp"
#include "omc/casc/storage.hpp"
#include "omc/formats/m3.hpp"
#include "omc/formats/map_archive.hpp"

namespace omc::app {

inline std::size_t append_terrain_material_doodads(formats::Model& ground, casc::Storage& store, MeleeModels& models,
                                                   const formats::LoadedMap& map) {
    if (map.cliff.empty() || ground.vertex_count == 0) return 0;
    const std::vector<float> th = formats::tier_heights(map);
    const float off = formats::engine_z_offset(map);
    const bool has_normals = ground.normals.size() == ground.positions.size();
    std::size_t placed = 0;
    const std::uint32_t first_region = std::uint32_t(ground.regions.size());
    const auto append = [&](const formats::Model* pm, float px, float py, float rotation, float sx, float sy, float sz) {
        const float base_z = formats::vertex_ground_bilinear(map, th, off, px, py);
        const float cr = std::cos(rotation), sr = std::sin(rotation);
        for (const formats::M3Region& rg : pm->terrain_regions) {
            if (std::size_t(rg.first_vertex) + rg.num_vertices > pm->positions.size() / 3 ||
                std::size_t(rg.first_index) + rg.num_indices > pm->indices.size() || rg.num_vertices == 0 || rg.num_indices == 0)
                continue;
            if (std::getenv("OMC_TERRAIN_DOODAD_TRACE")) {
                float zlo = 1e9f, zhi = -1e9f, rmax = 0.0f;
                for (std::uint32_t v = 0; v < rg.num_vertices; ++v) {
                    const std::size_t s = (std::size_t(rg.first_vertex) + v) * 3;
                    zlo = std::min(zlo, pm->positions[s + 2] * sz);
                    zhi = std::max(zhi, pm->positions[s + 2] * sz);
                    rmax = std::max(rmax, std::sqrt(pm->positions[s] * pm->positions[s] + pm->positions[s + 1] * pm->positions[s + 1]) * sx);
                }
                std::printf("terrain doodad at (%.2f,%.2f) ground z %.2f: %u verts, local z [%.2f, %.2f], radius %.2f, model '%s'\n", double(px), double(py),
                            double(base_z), rg.num_vertices, double(zlo), double(zhi), double(rmax), pm->name.c_str());
                for (const float fr : {0.5f, 1.0f, 1.3f}) {
                    std::printf("    ground at %.1f r:", double(fr));
                    for (int k = 0; k < 8; ++k) {
                        const float a = float(k) * 0.7853982f;
                        std::printf(" %.2f", double(formats::vertex_ground_bilinear(map, th, off, px + std::cos(a) * rmax * fr, py + std::sin(a) * rmax * fr)));
                    }
                    std::printf("\n");
                }
                {
                    const bool sn = pm->normals.size() == pm->positions.size();
                    float nzlo = 1.0f, nzsum = 0.0f;
                    if (sn)
                        for (std::uint32_t v = 0; v < rg.num_vertices; ++v) {
                            const float nz = pm->normals[(std::size_t(rg.first_vertex) + v) * 3 + 2];
                            nzlo = std::min(nzlo, nz);
                            nzsum += nz;
                        }
                    std::printf("    ground has normals %d (%zu / %zu), model has normals %d, model normal z min %.3f mean %.3f\n", int(has_normals),
                                ground.normals.size(), ground.positions.size(), int(sn), double(nzlo), double(nzsum / float(rg.num_vertices)));
                    if (sn) {
                        int agree = 0, oppose = 0, up = 0, down = 0;
                        for (std::uint32_t t = 0; t + 2 < rg.num_indices; t += 3) {
                            std::size_t vi[3];
                            for (int k = 0; k < 3; ++k) vi[k] = std::size_t(rg.first_vertex) + pm->indices[std::size_t(rg.first_index) + t + std::uint32_t(k)];
                            const float* p0 = &pm->positions[vi[0] * 3];
                            const float* p1 = &pm->positions[vi[1] * 3];
                            const float* p2 = &pm->positions[vi[2] * 3];
                            const float ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
                            const float bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
                            const float fx = ay * bz - az * by, fy = az * bx - ax * bz, fz = ax * by - ay * bx;
                            float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                            for (int k = 0; k < 3; ++k) { vx += pm->normals[vi[k] * 3]; vy += pm->normals[vi[k] * 3 + 1]; vz += pm->normals[vi[k] * 3 + 2]; }
                            (fx * vx + fy * vy + fz * vz >= 0.0f ? agree : oppose) += 1;
                            (fz >= 0.0f ? up : down) += 1;
                        }
                        std::printf("    triangles: vertex normals with the winding %d, against it %d; winding faces up %d, down %d\n", agree, oppose, up, down);
                    }
                }
                std::printf("    model bounds [%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f], %u vertices in all, %zu own regions, placement scale (%.3f,%.3f,%.3f)\n",
                            double(pm->bounds_min[0]), double(pm->bounds_min[1]), double(pm->bounds_min[2]), double(pm->bounds_max[0]),
                            double(pm->bounds_max[1]), double(pm->bounds_max[2]), pm->vertex_count, pm->regions.size(), double(sx), double(sy), double(sz));
                if (pm->vertex_colors.size() == pm->positions.size() / 3 * 4) {
                    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                    double sum[4] = {0, 0, 0, 0};
                    for (std::uint32_t v = 0; v < rg.num_vertices; ++v)
                        for (int c = 0; c < 4; ++c) {
                            const int x = pm->vertex_colors[(std::size_t(rg.first_vertex) + v) * 4 + c];
                            lo[c] = std::min(lo[c], x);
                            hi[c] = std::max(hi[c], x);
                            sum[c] += x;
                        }
                    std::printf("    vertex colours R %d..%d (%.0f) G %d..%d (%.0f) B %d..%d (%.0f) A %d..%d (%.0f)\n", lo[0], hi[0], sum[0] / rg.num_vertices, lo[1],
                                hi[1], sum[1] / rg.num_vertices, lo[2], hi[2], sum[2] / rg.num_vertices, lo[3], hi[3], sum[3] / rg.num_vertices);
                } else {
                    std::printf("    vertex colours: none (%zu bytes)\n", pm->vertex_colors.size());
                }
                for (std::size_t b = 0; b < pm->bones.size() && b < 4; ++b) {
                    const formats::M3Bone& bn = pm->bones[b];
                    std::printf("    bone %zu '%s' parent %d loc (%.3f,%.3f,%.3f) rot (%.3f,%.3f,%.3f,%.3f) scale (%.3f,%.3f,%.3f)\n", b, bn.name.c_str(), bn.parent,
                                double(bn.location.init[0]), double(bn.location.init[1]), double(bn.location.init[2]), double(bn.rotation.init[0]),
                                double(bn.rotation.init[1]), double(bn.rotation.init[2]), double(bn.rotation.init[3]), double(bn.scale.init[0]),
                                double(bn.scale.init[1]), double(bn.scale.init[2]));
                    if (pm->inverse_bind.size() >= (b + 1) * 16) {
                        std::printf("      iref");
                        for (int k = 0; k < 16; ++k) std::printf(" %.3f", double(pm->inverse_bind[b * 16 + k]));
                        std::printf("\n");
                    }
                }
            }
            formats::M3Region out = rg;
            out.first_vertex = std::uint32_t(ground.positions.size() / 3);
            out.first_index = std::uint32_t(ground.indices.size());
            const bool src_normals = pm->normals.size() == pm->positions.size();
            for (std::uint32_t v = 0; v < rg.num_vertices; ++v) {
                const std::size_t s = (std::size_t(rg.first_vertex) + v) * 3;
                const float lx = pm->positions[s] * sx, ly = pm->positions[s + 1] * sy, lz = pm->positions[s + 2] * sz;
                const float wx = px + cr * lx - sr * ly, wy = py + sr * lx + cr * ly;
                static const bool kConform = std::getenv("OMC_TERRAIN_DOODAD_CONFORM") != nullptr;
                const float vz = kConform ? formats::vertex_ground_bilinear(map, th, off, wx, wy) : base_z;
                static const float kZScale = std::getenv("OMC_TERRAIN_DOODAD_ZSCALE") ? float(std::atof(std::getenv("OMC_TERRAIN_DOODAD_ZSCALE"))) : 1.0f;
                ground.positions.insert(ground.positions.end(), {wx, wy, vz + lz * kZScale});
                ground.uvs.push_back(0.0f);
                ground.uvs.push_back(0.0f);
                if (has_normals) {
                    float nx = 0.0f, ny = 0.0f, nz = 1.0f;
                    if (src_normals) {
                        const float ax = pm->normals[s], ay = pm->normals[s + 1], az = pm->normals[s + 2];
                        nx = cr * ax - sr * ay;
                        ny = sr * ax + cr * ay;
                        nz = az;
                        static const float kNGain = std::getenv("OMC_TERRAIN_DOODAD_NORMAL_GAIN") ? float(std::atof(std::getenv("OMC_TERRAIN_DOODAD_NORMAL_GAIN"))) : 1.0f;
                        nx *= kNGain;
                        ny *= kNGain;
                        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; } else { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
                    }
                    ground.normals.insert(ground.normals.end(), {nx, ny, nz});
                }
            }
            for (std::uint32_t i = 0; i < rg.num_indices; ++i) ground.indices.push_back(pm->indices[std::size_t(rg.first_index) + i]);
            ground.regions.push_back(out);
            if (!ground.region_material.empty()) ground.region_material.push_back(ground.region_material.front());
            ++placed;
        }
    };
    for (const formats::PlacedDoodad& d : map.objects.doodads) {
        const formats::Model* pm = models.doodad_terrain(store, d.type, d.variation);
        if (pm) append(pm, d.x, d.y, d.rotation, d.sx, d.sy, d.sz);
    }
    if (!std::getenv("OMC_NO_UNIT_TERRAIN_REGIONS"))
        for (const formats::PlacedUnit& u : map.objects.units) {
            static const bool kVar00 = std::getenv("OMC_UNIT_TERRAIN_VARIATION_00") != nullptr;
            const formats::Model* pm = (!kVar00 && u.variation >= 0) ? models.ensure_variation(store, u.unit_type, unsigned(u.variation)) : nullptr;
            if (!pm) pm = models.ensure(store, u.unit_type);
            if (pm && !pm->terrain_regions.empty()) {
                static const float kRotAdd = std::getenv("OMC_UNIT_TERRAIN_ROT_ADD") ? float(std::atof(std::getenv("OMC_UNIT_TERRAIN_ROT_ADD"))) : 0.0f;
                static const float kMirror = std::getenv("OMC_UNIT_TERRAIN_MIRROR_Y") ? -1.0f : 1.0f;
                append(pm, u.x, u.y, u.rotation + kRotAdd, 1.0f, kMirror, 1.0f);
            }
        }
    ground.vertex_count = std::uint32_t(ground.positions.size() / 3);
    if (placed > 0) {
        ground.caster_region_first = first_region;
        ground.caster_region_end = std::uint32_t(ground.regions.size());
    }
    return placed;
}

}
