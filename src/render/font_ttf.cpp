#include "omc/render/font_ttf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace omc::render {
namespace {

std::uint16_t rd_u16(const std::uint8_t* p, std::size_t n, std::size_t off) {
    if (off + 2 > n) return 0;
    return std::uint16_t(std::uint16_t(p[off]) << 8 | p[off + 1]);
}
std::int16_t rd_i16(const std::uint8_t* p, std::size_t n, std::size_t off) {
    return std::int16_t(rd_u16(p, n, off));
}
std::uint32_t rd_u32(const std::uint8_t* p, std::size_t n, std::size_t off) {
    if (off + 4 > n) return 0;
    return std::uint32_t(p[off]) << 24 | std::uint32_t(p[off + 1]) << 16 |
           std::uint32_t(p[off + 2]) << 8 | p[off + 3];
}

constexpr std::uint16_t kArgsAreWords = 0x0001;
constexpr std::uint16_t kArgsAreXY = 0x0002;
constexpr std::uint16_t kWeHaveScale = 0x0008;
constexpr std::uint16_t kMoreComponents = 0x0020;
constexpr std::uint16_t kXYScale = 0x0040;
constexpr std::uint16_t k2x2 = 0x0080;

}

bool OverlayFont::load(const std::vector<std::uint8_t>& sfnt) {
    valid_ = false;
    *this = OverlayFont{};
    const std::uint8_t* d = sfnt.data();
    const std::size_t n = sfnt.size();
    if (n < 12) return false;

    std::uint32_t tag = rd_u32(d, n, 0);
    if (tag != 0x00010000u && tag != 0x74727565u   && tag != 0x4F54544Fu  )
        return false;

    std::uint16_t num_tables = rd_u16(d, n, 4);
    std::size_t dir = 12;
    struct Tbl { std::uint32_t off = 0, len = 0; };
    Tbl head, hhea, maxp, hmtx, loca, glyf, cmap, cff;
    bool has_head = false, has_hhea = false, has_maxp = false, has_hmtx = false, has_loca = false,
         has_glyf = false, has_cmap = false, has_cff = false;
    for (std::uint16_t i = 0; i < num_tables; ++i) {
        std::size_t e = dir + std::size_t(i) * 16;
        if (e + 16 > n) return false;
        std::uint32_t t = rd_u32(d, n, e);
        Tbl v{rd_u32(d, n, e + 8), rd_u32(d, n, e + 12)};
        if (std::size_t(v.off) + v.len > n) continue;
        switch (t) {
            case 0x68656164: head = v; has_head = true; break;
            case 0x68686561: hhea = v; has_hhea = true; break;
            case 0x6D617870: maxp = v; has_maxp = true; break;
            case 0x686D7478: hmtx = v; has_hmtx = true; break;
            case 0x6C6F6361: loca = v; has_loca = true; break;
            case 0x676C7966: glyf = v; has_glyf = true; break;
            case 0x636D6170: cmap = v; has_cmap = true; break;
            case 0x43464620: cff = v; has_cff = true; break;
            default: break;
        }
    }
    bool use_glyf = has_glyf && has_loca;
    is_cff_ = !use_glyf && has_cff;
    if (!has_head || !has_hhea || !has_maxp || !has_hmtx || !has_cmap) return false;
    if (!use_glyf && !is_cff_) return false;

    units_per_em_ = rd_u16(d, n, head.off + 18);
    loc_long_ = rd_i16(d, n, head.off + 50);
    if (units_per_em_ <= 0) return false;

    num_glyphs_ = rd_u16(d, n, maxp.off + 4);
    if (num_glyphs_ <= 0) return false;

    ascent_ = rd_i16(d, n, hhea.off + 4);
    descent_ = rd_i16(d, n, hhea.off + 6);
    line_gap_ = rd_i16(d, n, hhea.off + 8);
    num_h_metrics_ = rd_u16(d, n, hhea.off + 34);
    if (num_h_metrics_ <= 0) return false;

    advance_w_.resize(std::size_t(num_h_metrics_));
    for (int i = 0; i < num_h_metrics_; ++i)
        advance_w_[std::size_t(i)] = rd_u16(d, n, hmtx.off + std::size_t(i) * 4);
    last_advance_ = advance_w_.empty() ? 0 : advance_w_.back();

    if (use_glyf) {
        loca_.resize(std::size_t(num_glyphs_) + 1);
        for (int i = 0; i <= num_glyphs_; ++i) {
            if (loc_long_)
                loca_[std::size_t(i)] = rd_u32(d, n, loca.off + std::size_t(i) * 4);
            else
                loca_[std::size_t(i)] =
                    std::uint32_t(rd_u16(d, n, loca.off + std::size_t(i) * 2)) * 2;
        }
        glyf_.assign(d + glyf.off, d + glyf.off + glyf.len);
    } else {
        cff_ = std::make_shared<CffFont>();
        if (!cff_->parse(d, n, cff.off, cff.len)) return false;
    }

    {
        std::size_t base = cmap.off;
        std::uint16_t nsub = rd_u16(d, n, base + 2);
        std::uint32_t best = 0;
        int best_score = -1;
        for (std::uint16_t i = 0; i < nsub; ++i) {
            std::size_t rec = base + 4 + std::size_t(i) * 8;
            std::uint16_t plat = rd_u16(d, n, rec);
            std::uint16_t enc = rd_u16(d, n, rec + 2);
            std::uint32_t so = rd_u32(d, n, rec + 4);
            int score = -1;
            if (plat == 3 && enc == 10) score = 4;
            else if (plat == 3 && enc == 1) score = 3;
            else if (plat == 0) score = 2;
            else if (plat == 3 && enc == 0) score = 1;
            if (score > best_score) { best_score = score; best = so; }
        }
        if (best_score >= 0) {
            std::size_t so = base + best;
            std::uint16_t fmt = rd_u16(d, n, so);
            if (fmt == 4) {
                std::uint16_t segx2 = rd_u16(d, n, so + 6);
                std::uint16_t segc = segx2 / 2;
                std::size_t end_o = so + 14;
                std::size_t start_o = end_o + std::size_t(segx2) + 2;
                std::size_t delta_o = start_o + std::size_t(segx2);
                std::size_t range_o = delta_o + std::size_t(segx2);
                for (std::uint16_t s = 0; s < segc; ++s) {
                    std::uint16_t end = rd_u16(d, n, end_o + std::size_t(s) * 2);
                    std::uint16_t start = rd_u16(d, n, start_o + std::size_t(s) * 2);
                    std::int16_t delta = rd_i16(d, n, delta_o + std::size_t(s) * 2);
                    std::uint16_t rng = rd_u16(d, n, range_o + std::size_t(s) * 2);
                    if (start > end) continue;
                    for (std::uint32_t c = start; c <= end; ++c) {
                        if (c == 0xFFFF) break;
                        std::uint16_t gid;
                        if (rng == 0) {
                            gid = std::uint16_t((c + delta) & 0xFFFF);
                        } else {
                            std::size_t gi = range_o + std::size_t(s) * 2 + rng +
                                             std::size_t(c - start) * 2;
                            std::uint16_t g = rd_u16(d, n, gi);
                            gid = g ? std::uint16_t((g + delta) & 0xFFFF) : 0;
                        }
                        if (gid) cmap_[c] = gid;
                    }
                }
            } else if (fmt == 12) {
                std::uint32_t ngroups = rd_u32(d, n, so + 12);
                for (std::uint32_t g = 0; g < ngroups; ++g) {
                    std::size_t go = so + 16 + std::size_t(g) * 12;
                    std::uint32_t sc = rd_u32(d, n, go);
                    std::uint32_t ec = rd_u32(d, n, go + 4);
                    std::uint32_t sg = rd_u32(d, n, go + 8);
                    if (sc > ec || ec - sc > 0x10000u) continue;
                    for (std::uint32_t c = sc; c <= ec; ++c)
                        cmap_[c] = std::uint16_t((sg + (c - sc)) & 0xFFFF);
                }
            } else if (fmt == 6) {
                std::uint16_t first = rd_u16(d, n, so + 6);
                std::uint16_t count = rd_u16(d, n, so + 8);
                for (std::uint16_t i = 0; i < count; ++i)
                    cmap_[std::uint32_t(first) + i] = rd_u16(d, n, so + 10 + std::size_t(i) * 2);
            }
        }
    }
    if (cmap_.empty()) return false;

    valid_ = true;
    return true;
}

std::uint16_t OverlayFont::glyph_id(std::uint32_t cp) const {
    auto it = cmap_.find(cp);
    return it == cmap_.end() ? 0 : it->second;
}

std::uint16_t OverlayFont::advance_units(int gid) const {
    if (gid < 0) return 0;
    if (gid < num_h_metrics_) return advance_w_[std::size_t(gid)];
    return last_advance_;
}

int OverlayFont::raw_advance_units(char ch) const {
    if (!valid_) return 0;
    return advance_units(glyph_id(static_cast<unsigned char>(ch)));
}

int OverlayFont::ascent_px(int pixel_height) const {
    if (!valid_ || pixel_height <= 0) return 0;
    return int(std::lround(double(ascent_) * height_px(pixel_height) / units_per_em_));
}

int OverlayFont::line_height_px(int pixel_height) const {
    if (!valid_ || pixel_height <= 0) return 0;
    long h = std::lround(double(ascent_ - descent_ + line_gap_) * height_px(pixel_height) / units_per_em_);
    return int(h);
}

void OverlayFont::build_outline(int gid, Outline& out, int depth) const {
    if (is_cff_) {
        if (!cff_ || gid < 0) return;
        std::vector<CffFont::Contour> cs;
        cff_->glyph_contours(gid, cs);
        for (auto& c : cs) {
            if (c.size() < 2) continue;
            std::vector<Point> contour;
            contour.reserve(c.size());
            for (auto& pr : c) contour.push_back({pr.first, pr.second, true});
            out.contours.push_back(std::move(contour));
        }
        return;
    }
    if (depth > 5 || gid < 0 || gid + 1 >= int(loca_.size())) return;
    std::uint32_t go = loca_[std::size_t(gid)];
    std::uint32_t ge = loca_[std::size_t(gid) + 1];
    if (ge <= go || ge > glyf_.size()) return;
    const std::uint8_t* g = glyf_.data();
    const std::size_t gn = glyf_.size();
    std::int16_t ncont = rd_i16(g, gn, go + 0);

    if (ncont >= 0) {
        std::size_t p = go + 10;
        std::vector<std::uint16_t> ends(static_cast<std::size_t>(ncont));
        for (int i = 0; i < ncont; ++i) { ends[std::size_t(i)] = rd_u16(g, gn, p); p += 2; }
        int npts = ncont ? int(ends.back()) + 1 : 0;
        if (npts <= 0 || npts > 20000) return;
        std::uint16_t instr_len = rd_u16(g, gn, p); p += 2 + instr_len;

        std::vector<std::uint8_t> flags(static_cast<std::size_t>(npts));
        for (int i = 0; i < npts;) {
            if (p >= gn) return;
            std::uint8_t f = g[p++];
            flags[std::size_t(i++)] = f;
            if (f & 0x08) {
                if (p >= gn) return;
                std::uint8_t r = g[p++];
                while (r-- && i < npts) flags[std::size_t(i++)] = f;
            }
        }
        std::vector<int> xs(static_cast<std::size_t>(npts)), ys(static_cast<std::size_t>(npts));
        int x = 0;
        for (int i = 0; i < npts; ++i) {
            std::uint8_t f = flags[std::size_t(i)];
            if (f & 0x02) {
                if (p >= gn) return;
                int dx = g[p++];
                x += (f & 0x10) ? dx : -dx;
            } else if (!(f & 0x10)) {
                x += rd_i16(g, gn, p); p += 2;
            }
            xs[std::size_t(i)] = x;
        }
        int y = 0;
        for (int i = 0; i < npts; ++i) {
            std::uint8_t f = flags[std::size_t(i)];
            if (f & 0x04) {
                if (p >= gn) return;
                int dy = g[p++];
                y += (f & 0x20) ? dy : -dy;
            } else if (!(f & 0x20)) {
                y += rd_i16(g, gn, p); p += 2;
            }
            ys[std::size_t(i)] = y;
        }
        int start = 0;
        for (int c = 0; c < ncont; ++c) {
            int end = int(ends[std::size_t(c)]);
            if (end < start || end >= npts) { start = end + 1; continue; }
            std::vector<Point> contour;
            for (int i = start; i <= end; ++i)
                contour.push_back({float(xs[std::size_t(i)]), float(ys[std::size_t(i)]),
                                   (flags[std::size_t(i)] & 0x01) != 0});
            out.contours.push_back(std::move(contour));
            start = end + 1;
        }
    } else {
        std::size_t p = go + 10;
        for (;;) {
            std::uint16_t flags = rd_u16(g, gn, p); p += 2;
            std::uint16_t cgid = rd_u16(g, gn, p); p += 2;
            float dx = 0, dy = 0;
            if (flags & kArgsAreWords) {
                if (flags & kArgsAreXY) { dx = rd_i16(g, gn, p); dy = rd_i16(g, gn, p + 2); }
                p += 4;
            } else {
                if ((flags & kArgsAreXY) && p + 2 <= gn) {
                    dx = std::int8_t(g[p]); dy = std::int8_t(g[p + 1]);
                }
                p += 2;
            }
            float a = 1, b = 0, cc = 0, dd = 1;
            if (flags & kWeHaveScale) {
                float s = rd_i16(g, gn, p) / 16384.0f; p += 2; a = dd = s;
            } else if (flags & kXYScale) {
                a = rd_i16(g, gn, p) / 16384.0f; dd = rd_i16(g, gn, p + 2) / 16384.0f; p += 4;
            } else if (flags & k2x2) {
                a = rd_i16(g, gn, p) / 16384.0f; b = rd_i16(g, gn, p + 2) / 16384.0f;
                cc = rd_i16(g, gn, p + 4) / 16384.0f; dd = rd_i16(g, gn, p + 6) / 16384.0f; p += 8;
            }
            Outline sub;
            build_outline(cgid, sub, depth + 1);
            for (auto& contour : sub.contours) {
                for (auto& pt : contour) {
                    float nx = a * pt.x + cc * pt.y + dx;
                    float ny = b * pt.x + dd * pt.y + dy;
                    pt.x = nx; pt.y = ny;
                }
                out.contours.push_back(std::move(contour));
            }
            if (!(flags & kMoreComponents)) break;
            if (p >= gn) break;
        }
    }
}

const OverlayFont::Outline& OverlayFont::outline(int gid) const {
    auto it = outline_cache_.find(gid);
    if (it != outline_cache_.end()) return it->second;
    Outline o;
    build_outline(gid, o, 0);
    return outline_cache_.emplace(gid, std::move(o)).first->second;
}

const GlyphBitmap& OverlayFont::glyph(char ch, int pixel_height) const {
    int key_ch = static_cast<unsigned char>(ch);
    auto key = std::make_pair(key_ch, pixel_height);
    auto cit = raster_cache_.find(key);
    if (cit != raster_cache_.end()) return cit->second;

    GlyphBitmap bmp;
    if (!valid_ || pixel_height <= 0) return raster_cache_.emplace(key, std::move(bmp)).first->second;

    std::uint16_t gid = glyph_id(std::uint32_t(key_ch));
    const double scale = height_px(pixel_height) / units_per_em_;
    bmp.advance = int(std::lround(advance_units(gid) * scale));

    const Outline& ol = outline(gid);
    struct Edge { float x0, y0, x1, y1; };
    std::vector<Edge> edges;
    float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
    auto acc = [&](float px, float py) {
        xmin = std::min(xmin, px); xmax = std::max(xmax, px);
        ymin = std::min(ymin, py); ymax = std::max(ymax, py);
    };

    for (const auto& c : ol.contours)
        for (const auto& p : c) acc(float(p.x * scale), float(p.y * scale));
    if (ol.contours.empty() || xmax < xmin) {
        return raster_cache_.emplace(key, std::move(bmp)).first->second;
    }

    const int pad = 1;
    int ix0 = int(std::floor(xmin)) - pad;
    int iy1 = int(std::ceil(ymax)) + pad;
    int W = int(std::ceil(xmax)) + pad - ix0;
    int H = iy1 - (int(std::floor(ymin)) - pad);
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) {
        return raster_cache_.emplace(key, std::move(bmp)).first->second;
    }

    auto mapx = [&](double fx) { return float(fx * scale) - float(ix0); };
    auto mapy = [&](double fy) { return float(iy1) - float(fy * scale); };

    for (const auto& c : ol.contours) {
        int m = int(c.size());
        if (m < 2) continue;
        std::vector<Point> pts;
        pts.reserve(std::size_t(m) * 2);
        for (int i = 0; i < m; ++i) {
            const Point& cur = c[std::size_t(i)];
            pts.push_back(cur);
            const Point& nxt = c[std::size_t((i + 1) % m)];
            if (!cur.on && !nxt.on)
                pts.push_back({(cur.x + nxt.x) * 0.5f, (cur.y + nxt.y) * 0.5f, true});
        }
        int startIdx = -1;
        for (int i = 0; i < int(pts.size()); ++i)
            if (pts[std::size_t(i)].on) { startIdx = i; break; }
        if (startIdx < 0) continue;

        int cnt = int(pts.size());
        float px = mapx(pts[std::size_t(startIdx)].x), py = mapy(pts[std::size_t(startIdx)].y);
        float sx = px, sy = py;
        int i = 1;
        while (i <= cnt) {
            const Point& p = pts[std::size_t((startIdx + i) % cnt)];
            if (p.on) {
                float nx = mapx(p.x), ny = mapy(p.y);
                edges.push_back({px, py, nx, ny});
                px = nx; py = ny;
                ++i;
            } else {
                const Point& e = pts[std::size_t((startIdx + i + 1) % cnt)];
                float cx = mapx(p.x), cy = mapy(p.y);
                float ex = mapx(e.x), ey = mapy(e.y);
                const int SEG = 8;
                float lx = px, ly = py;
                for (int s = 1; s <= SEG; ++s) {
                    float t = float(s) / SEG, u = 1.0f - t;
                    float qx = u * u * px + 2 * u * t * cx + t * t * ex;
                    float qy = u * u * py + 2 * u * t * cy + t * t * ey;
                    edges.push_back({lx, ly, qx, qy});
                    lx = qx; ly = qy;
                }
                px = ex; py = ey;
                i += 2;
            }
        }
        edges.push_back({px, py, sx, sy});
    }

    const int SS = 4;
    std::vector<std::uint16_t> accum(std::size_t(W) * H, 0);
    std::vector<std::pair<float, int>> xs;
    for (int row = 0; row < H; ++row) {
        for (int sub = 0; sub < SS; ++sub) {
            float yy = float(row) + (float(sub) + 0.5f) / SS;
            xs.clear();
            for (const auto& e : edges) {
                float y0 = e.y0, y1 = e.y1, x0 = e.x0, x1 = e.x1;
                int dir = 1;
                if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); dir = -1; }
                if (yy < y0 || yy >= y1) continue;
                float t = (yy - y0) / (y1 - y0);
                xs.push_back({x0 + t * (x1 - x0), dir});
            }
            if (xs.empty()) continue;
            std::sort(xs.begin(), xs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            int wind = 0;
            for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
                wind += xs[k].second;
                if (wind == 0) continue;
                float xa = xs[k].first, xb = xs[k + 1].first;
                int cs = std::max(0, int(std::floor(xa * SS)));
                int ce = std::min(W * SS - 1, int(std::ceil(xb * SS)));
                for (int scol = cs; scol <= ce; ++scol) {
                    float cx = (float(scol) + 0.5f) / SS;
                    if (cx >= xa && cx < xb) accum[std::size_t(row) * W + scol / SS] += 1;
                }
            }
        }
    }

    bmp.width = W;
    bmp.height = H;
    bmp.bearing_x = ix0;
    bmp.bearing_y = iy1;
    bmp.coverage.assign(std::size_t(W) * H, 0);
    const int maxc = SS * SS;
    for (std::size_t k = 0; k < bmp.coverage.size(); ++k)
        bmp.coverage[k] = std::uint8_t(std::min(255, int(accum[k]) * 255 / maxc));

    return raster_cache_.emplace(key, std::move(bmp)).first->second;
}

}
