#include "omc/render/font_cff.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace omc::render {
namespace {

std::uint8_t rd_u8(const std::uint8_t* p, std::size_t n, std::size_t off) {
    return off < n ? p[off] : 0;
}
std::uint16_t rd_u16(const std::uint8_t* p, std::size_t n, std::size_t off) {
    if (off + 2 > n) return 0;
    return std::uint16_t(std::uint16_t(p[off]) << 8 | p[off + 1]);
}
std::uint32_t rd_off(const std::uint8_t* p, std::size_t n, std::size_t off, int sz) {
    std::uint32_t v = 0;
    for (int i = 0; i < sz; ++i) v = (v << 8) | rd_u8(p, n, off + std::size_t(i));
    return v;
}

std::map<int, std::vector<double>> parse_dict(const std::uint8_t* d, std::size_t n, std::size_t a,
                                              std::size_t b) {
    std::map<int, std::vector<double>> ops;
    std::vector<double> st;
    std::size_t i = a;
    if (b > n) b = n;
    while (i < b) {
        std::uint8_t b0 = d[i];
        if (b0 <= 21) {
            int op = b0;
            ++i;
            if (b0 == 12) { op = 1200 + int(rd_u8(d, n, i)); ++i; }
            ops[op] = st;
            st.clear();
        } else if (b0 == 28) {
            st.push_back(double(std::int16_t(rd_u16(d, n, i + 1))));
            i += 3;
        } else if (b0 == 29) {
            st.push_back(double(std::int32_t(rd_off(d, n, i + 1, 4))));
            i += 5;
        } else if (b0 == 30) {
            ++i;
            std::string s;
            bool done = false;
            while (i < b && !done) {
                std::uint8_t by = d[i++];
                for (int h = 0; h < 2; ++h) {
                    int nib = h == 0 ? (by >> 4) : (by & 0xF);
                    if (nib <= 9) s += char('0' + nib);
                    else if (nib == 0xA) s += '.';
                    else if (nib == 0xB) s += 'E';
                    else if (nib == 0xC) s += "E-";
                    else if (nib == 0xE) s += '-';
                    else if (nib == 0xF) { done = true; break; }
                }
            }
            st.push_back(s.empty() ? 0.0 : std::atof(s.c_str()));
        } else if (b0 >= 32 && b0 <= 246) {
            st.push_back(double(int(b0) - 139));
            ++i;
        } else if (b0 >= 247 && b0 <= 250) {
            st.push_back(double((int(b0) - 247) * 256 + int(rd_u8(d, n, i + 1)) + 108));
            i += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            st.push_back(double(-(int(b0) - 251) * 256 - int(rd_u8(d, n, i + 1)) - 108));
            i += 2;
        } else {
            ++i;
        }
    }
    return ops;
}

constexpr int kFlattenSegments = 8;

}

int CffFont::bias_for(std::size_t n) {
    if (n < 1240) return 107;
    if (n < 33900) return 1131;
    return 32768;
}

bool CffFont::read_index(std::size_t pos, Index& out, std::size_t& end) const {
    const std::uint8_t* d = buf_.data();
    const std::size_t n = buf_.size();
    out.offsets.clear();
    if (pos + 2 > n) return false;
    std::uint16_t count = rd_u16(d, n, pos);
    if (count == 0) { end = pos + 2; return true; }
    if (pos + 3 > n) return false;
    int osz = rd_u8(d, n, pos + 2);
    if (osz < 1 || osz > 4) return false;
    std::size_t off_arr = pos + 3;
    std::size_t off_cnt = std::size_t(count) + 1;
    if (off_arr + off_cnt * std::size_t(osz) > n) return false;
    std::size_t data_base = off_arr + off_cnt * std::size_t(osz) - 1;
    out.offsets.resize(off_cnt);
    for (std::size_t i = 0; i < off_cnt; ++i) {
        std::uint32_t rel = rd_off(d, n, off_arr + i * std::size_t(osz), osz);
        std::size_t abs = data_base + rel;
        if (abs > n || rel == 0) return false;
        out.offsets[i] = std::uint32_t(abs);
    }
    for (std::size_t i = 1; i < off_cnt; ++i)
        if (out.offsets[i] < out.offsets[i - 1]) return false;
    end = out.offsets.back();
    return true;
}

bool CffFont::parse(const std::uint8_t* data, std::size_t n, std::uint32_t cff_off,
                    std::uint32_t cff_len) {
    valid_ = false;
    *this = CffFont{};
    if (!data || std::size_t(cff_off) + cff_len > n || cff_len < 4) return false;
    buf_.assign(data + cff_off, data + cff_off + cff_len);
    const std::uint8_t* d = buf_.data();
    const std::size_t bn = buf_.size();

    int hdr_size = rd_u8(d, bn, 2);
    if (hdr_size < 4 || std::size_t(hdr_size) > bn) return false;

    std::size_t pos = std::size_t(hdr_size), next = 0;
    Index name_idx, top_idx, string_idx;
    if (!read_index(pos, name_idx, next)) return false;
    pos = next;
    if (!read_index(pos, top_idx, next)) return false;
    pos = next;
    if (!read_index(pos, string_idx, next)) return false;
    pos = next;
    if (!read_index(pos, gsubrs_, next)) return false;
    gbias_ = bias_for(gsubrs_.count());

    if (top_idx.count() < 1) return false;
    auto tspan = top_idx.span(0);
    auto top = parse_dict(d, bn, tspan.first, tspan.second);

    auto cs = top.find(17);
    if (cs == top.end() || cs->second.empty()) return false;
    std::size_t cs_off = std::size_t(cs->second[0]);
    if (cs_off >= bn) return false;
    if (!read_index(cs_off, charstrings_, next)) return false;
    if (charstrings_.count() == 0) return false;

    is_cid_ = top.count(1230) != 0;

    auto read_private_local_subrs = [&](const std::map<int, std::vector<double>>& dict, Index& ls,
                                        int& lb) {
        ls.offsets.clear();
        lb = 0;
        auto pv = dict.find(18);
        if (pv == dict.end() || pv->second.size() < 2) return;
        std::size_t psz = std::size_t(std::max(0.0, pv->second[0]));
        std::size_t poff = std::size_t(std::max(0.0, pv->second[1]));
        if (poff >= bn || poff + psz > bn) return;
        auto priv = parse_dict(d, bn, poff, poff + psz);
        auto sr = priv.find(19);
        if (sr == priv.end() || sr->second.empty()) return;
        std::size_t so = poff + std::size_t(std::max(0.0, sr->second[0]));
        std::size_t e = 0;
        if (so < bn && read_index(so, ls, e)) lb = bias_for(ls.count());
        else ls.offsets.clear();
    };

    if (!is_cid_) {
        read_private_local_subrs(top, lsubrs_, lbias_);
    } else {
        auto fda = top.find(1236);
        auto fds = top.find(1237);
        if (fda == top.end() || fda->second.empty() || fds == top.end() || fds->second.empty())
            return false;
        std::size_t fda_off = std::size_t(fda->second[0]);
        Index fdarray;
        if (fda_off >= bn || !read_index(fda_off, fdarray, next)) return false;
        fd_lsubrs_.resize(fdarray.count());
        fd_lbias_.assign(fdarray.count(), 0);
        for (std::size_t f = 0; f < fdarray.count(); ++f) {
            auto sp = fdarray.span(f);
            auto fdict = parse_dict(d, bn, sp.first, sp.second);
            read_private_local_subrs(fdict, fd_lsubrs_[f], fd_lbias_[f]);
        }
        std::size_t fo = std::size_t(fds->second[0]);
        std::size_t ng = charstrings_.count();
        fdselect_.assign(ng, 0);
        if (fo < bn) {
            int fmt = rd_u8(d, bn, fo);
            if (fmt == 0) {
                for (std::size_t g = 0; g < ng; ++g) fdselect_[g] = rd_u8(d, bn, fo + 1 + g);
            } else if (fmt == 3) {
                std::uint16_t nr = rd_u16(d, bn, fo + 1);
                std::size_t rp = fo + 3;
                for (std::uint16_t r = 0; r < nr; ++r) {
                    std::uint16_t first = rd_u16(d, bn, rp + std::size_t(r) * 3);
                    std::uint8_t fd = rd_u8(d, bn, rp + std::size_t(r) * 3 + 2);
                    std::uint16_t nextfirst = rd_u16(d, bn, rp + std::size_t(r) * 3 + 3);
                    for (std::uint32_t g = first; g < nextfirst && g < ng; ++g)
                        fdselect_[g] = fd;
                }
            }
        }
    }

    valid_ = true;
    return true;
}

const CffFont::Index& CffFont::local_subrs(int gid, int& bias) const {
    if (is_cid_ && gid >= 0 && std::size_t(gid) < fdselect_.size()) {
        std::size_t fd = fdselect_[std::size_t(gid)];
        if (fd < fd_lsubrs_.size()) { bias = fd_lbias_[fd]; return fd_lsubrs_[fd]; }
    }
    bias = lbias_;
    return lsubrs_;
}

struct CffFont::Interp {
    std::vector<double> stack;
    std::vector<Contour>* out = nullptr;
    Contour cur;
    double x = 0, y = 0;
    int n_stems = 0;
    bool have_width = false;
    bool ended = false;
    const Index* lsubrs = nullptr;
    int lbias = 0;

    void close() {
        if (cur.size() >= 2) out->push_back(std::move(cur));
        cur.clear();
    }
    void move_to() {
        close();
        cur.push_back({float(x), float(y)});
    }
    void line_to() { cur.push_back({float(x), float(y)}); }
    void curve_to(double c1x, double c1y, double c2x, double c2y, double ex, double ey) {
        double x0 = x, y0 = y;
        for (int s = 1; s <= kFlattenSegments; ++s) {
            double t = double(s) / kFlattenSegments, u = 1.0 - t;
            double px = u * u * u * x0 + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * ex;
            double py = u * u * u * y0 + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * ey;
            cur.push_back({float(px), float(py)});
        }
        x = ex;
        y = ey;
    }
};

void CffFont::run_charstring(Interp& st, std::size_t begin, std::size_t end, int depth) const {
    if (depth > 60) return;
    const std::uint8_t* d = buf_.data();
    const std::size_t bn = buf_.size();
    std::size_t i = begin;
    if (end > bn) end = bn;
    auto& stk = st.stack;

    while (i < end && !st.ended) {
        std::uint8_t b0 = d[i++];
        if (b0 >= 32 || b0 == 28) {
            double v = 0;
            if (b0 == 28) { v = double(std::int16_t(rd_u16(d, bn, i))); i += 2; }
            else if (b0 <= 246) v = double(int(b0) - 139);
            else if (b0 <= 250) { v = double((int(b0) - 247) * 256 + int(rd_u8(d, bn, i)) + 108); i += 1; }
            else if (b0 <= 254) { v = double(-(int(b0) - 251) * 256 - int(rd_u8(d, bn, i)) - 108); i += 1; }
            else {
                std::int32_t f = std::int32_t(rd_off(d, bn, i, 4)); i += 4;
                v = double(f) / 65536.0;
            }
            stk.push_back(v);
            continue;
        }

        switch (b0) {
            case 1: case 3: case 18: case 23: {
                if (!st.have_width && (stk.size() & 1)) st.have_width = true;
                st.n_stems += int(stk.size() / 2);
                stk.clear();
                break;
            }
            case 19: case 20: {
                if (!st.have_width && (stk.size() & 1)) st.have_width = true;
                st.n_stems += int(stk.size() / 2);
                stk.clear();
                st.have_width = true;
                i += std::size_t((st.n_stems + 7) / 8);
                break;
            }
            case 21: {
                std::size_t base = (!st.have_width && stk.size() > 2) ? 1 : 0;
                st.have_width = true;
                if (stk.size() >= base + 2) { st.x += stk[base]; st.y += stk[base + 1]; }
                st.move_to();
                stk.clear();
                break;
            }
            case 22: {
                std::size_t base = (!st.have_width && stk.size() > 1) ? 1 : 0;
                st.have_width = true;
                if (stk.size() >= base + 1) st.x += stk[base];
                st.move_to();
                stk.clear();
                break;
            }
            case 4: {
                std::size_t base = (!st.have_width && stk.size() > 1) ? 1 : 0;
                st.have_width = true;
                if (stk.size() >= base + 1) st.y += stk[base];
                st.move_to();
                stk.clear();
                break;
            }
            case 5: {
                for (std::size_t k = 0; k + 2 <= stk.size(); k += 2) {
                    st.x += stk[k]; st.y += stk[k + 1]; st.line_to();
                }
                stk.clear();
                break;
            }
            case 6: case 7: {
                bool horiz = (b0 == 6);
                for (std::size_t k = 0; k < stk.size(); ++k) {
                    if (horiz) st.x += stk[k]; else st.y += stk[k];
                    st.line_to();
                    horiz = !horiz;
                }
                stk.clear();
                break;
            }
            case 8: {
                for (std::size_t k = 0; k + 6 <= stk.size(); k += 6) {
                    double c1x = st.x + stk[k], c1y = st.y + stk[k + 1];
                    double c2x = c1x + stk[k + 2], c2y = c1y + stk[k + 3];
                    double ex = c2x + stk[k + 4], ey = c2y + stk[k + 5];
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                }
                stk.clear();
                break;
            }
            case 24: {
                std::size_t k = 0, ncur = stk.size();
                while (ncur - k >= 8) {
                    double c1x = st.x + stk[k], c1y = st.y + stk[k + 1];
                    double c2x = c1x + stk[k + 2], c2y = c1y + stk[k + 3];
                    double ex = c2x + stk[k + 4], ey = c2y + stk[k + 5];
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                    k += 6;
                }
                if (ncur - k >= 2) { st.x += stk[k]; st.y += stk[k + 1]; st.line_to(); }
                stk.clear();
                break;
            }
            case 25: {
                std::size_t k = 0, ncur = stk.size();
                while (ncur - k > 6) { st.x += stk[k]; st.y += stk[k + 1]; st.line_to(); k += 2; }
                if (ncur - k >= 6) {
                    double c1x = st.x + stk[k], c1y = st.y + stk[k + 1];
                    double c2x = c1x + stk[k + 2], c2y = c1y + stk[k + 3];
                    double ex = c2x + stk[k + 4], ey = c2y + stk[k + 5];
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                }
                stk.clear();
                break;
            }
            case 26: {
                std::size_t k = 0, ncur = stk.size();
                double dx1 = 0;
                if (ncur & 1) { dx1 = stk[0]; k = 1; }
                for (; k + 4 <= ncur; k += 4) {
                    double c1x = st.x + dx1, c1y = st.y + stk[k];
                    double c2x = c1x + stk[k + 1], c2y = c1y + stk[k + 2];
                    double ex = c2x, ey = c2y + stk[k + 3];
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                    dx1 = 0;
                }
                stk.clear();
                break;
            }
            case 27: {
                std::size_t k = 0, ncur = stk.size();
                double dy1 = 0;
                if (ncur & 1) { dy1 = stk[0]; k = 1; }
                for (; k + 4 <= ncur; k += 4) {
                    double c1x = st.x + stk[k], c1y = st.y + dy1;
                    double c2x = c1x + stk[k + 1], c2y = c1y + stk[k + 2];
                    double ex = c2x + stk[k + 3], ey = c2y;
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                    dy1 = 0;
                }
                stk.clear();
                break;
            }
            case 30: case 31: {
                bool horiz = (b0 == 31);
                std::size_t k = 0, ncur = stk.size();
                while (ncur - k >= 4) {
                    bool last = (ncur - k == 5);
                    double c1x, c1y, c2x, c2y, ex, ey;
                    if (horiz) {
                        c1x = st.x + stk[k]; c1y = st.y;
                        c2x = c1x + stk[k + 1]; c2y = c1y + stk[k + 2];
                        ey = c2y + stk[k + 3];
                        ex = last ? c2x + stk[k + 4] : c2x;
                    } else {
                        c1x = st.x; c1y = st.y + stk[k];
                        c2x = c1x + stk[k + 1]; c2y = c1y + stk[k + 2];
                        ex = c2x + stk[k + 3];
                        ey = last ? c2y + stk[k + 4] : c2y;
                    }
                    st.curve_to(c1x, c1y, c2x, c2y, ex, ey);
                    k += 4;
                    horiz = !horiz;
                }
                stk.clear();
                break;
            }
            case 10: case 29: {
                if (stk.empty()) break;
                int idx = int(std::lround(stk.back()));
                stk.pop_back();
                const Index& subrs = (b0 == 10) ? *st.lsubrs : gsubrs_;
                int bias = (b0 == 10) ? st.lbias : gbias_;
                std::size_t si = std::size_t(idx + bias);
                if (si + 1 < subrs.offsets.size()) {
                    auto sp = subrs.span(si);
                    run_charstring(st, sp.first, sp.second, depth + 1);
                }
                break;
            }
            case 11:
                return;
            case 14: {
                if (!st.have_width && !stk.empty() && stk.size() != 4) st.have_width = true;
                st.close();
                st.ended = true;
                return;
            }
            case 12: {
                std::uint8_t b1 = rd_u8(d, bn, i);
                ++i;
                auto& s = stk;
                if (b1 == 34 && s.size() >= 7) {
                    double y0 = st.y;
                    double c1x = st.x + s[0], c1y = st.y;
                    double c2x = c1x + s[1], c2y = c1y + s[2];
                    double e1x = c2x + s[3], e1y = c2y;
                    st.curve_to(c1x, c1y, c2x, c2y, e1x, e1y);
                    double c3x = st.x + s[4], c3y = st.y;
                    double c4x = c3x + s[5], c4y = y0;
                    double e2x = c4x + s[6], e2y = y0;
                    st.curve_to(c3x, c3y, c4x, c4y, e2x, e2y);
                } else if (b1 == 35 && s.size() >= 12) {
                    double c1x = st.x + s[0], c1y = st.y + s[1];
                    double c2x = c1x + s[2], c2y = c1y + s[3];
                    double e1x = c2x + s[4], e1y = c2y + s[5];
                    st.curve_to(c1x, c1y, c2x, c2y, e1x, e1y);
                    double c3x = st.x + s[6], c3y = st.y + s[7];
                    double c4x = c3x + s[8], c4y = c3y + s[9];
                    double e2x = c4x + s[10], e2y = c4y + s[11];
                    st.curve_to(c3x, c3y, c4x, c4y, e2x, e2y);
                } else if (b1 == 36 && s.size() >= 9) {
                    double y0 = st.y;
                    double c1x = st.x + s[0], c1y = st.y + s[1];
                    double c2x = c1x + s[2], c2y = c1y + s[3];
                    double e1x = c2x + s[4], e1y = c2y;
                    st.curve_to(c1x, c1y, c2x, c2y, e1x, e1y);
                    double c3x = st.x + s[5], c3y = st.y;
                    double c4x = c3x + s[6], c4y = c3y + s[7];
                    double e2x = c4x + s[8], e2y = y0;
                    st.curve_to(c3x, c3y, c4x, c4y, e2x, e2y);
                } else if (b1 == 37 && s.size() >= 11) {
                    double x0 = st.x, y0 = st.y;
                    double dxsum = s[0] + s[2] + s[4] + s[6] + s[8];
                    double dysum = s[1] + s[3] + s[5] + s[7] + s[9];
                    double c1x = st.x + s[0], c1y = st.y + s[1];
                    double c2x = c1x + s[2], c2y = c1y + s[3];
                    double e1x = c2x + s[4], e1y = c2y + s[5];
                    st.curve_to(c1x, c1y, c2x, c2y, e1x, e1y);
                    double c3x = st.x + s[6], c3y = st.y + s[7];
                    double c4x = c3x + s[8], c4y = c3y + s[9];
                    double e2x, e2y;
                    if (std::fabs(dxsum) > std::fabs(dysum)) { e2x = c4x + s[10]; e2y = y0; }
                    else { e2x = x0; e2y = c4y + s[10]; }
                    st.curve_to(c3x, c3y, c4x, c4y, e2x, e2y);
                }
                stk.clear();
                break;
            }
            default:
                stk.clear();
                break;
        }
    }
}

void CffFont::glyph_contours(int gid, std::vector<Contour>& out) const {
    if (!valid_ || gid < 0 || std::size_t(gid) >= charstrings_.count()) return;
    auto sp = charstrings_.span(std::size_t(gid));
    if (sp.second <= sp.first) return;
    Interp st;
    st.out = &out;
    int bias = 0;
    st.lsubrs = &local_subrs(gid, bias);
    st.lbias = bias;
    run_charstring(st, sp.first, sp.second, 0);
    st.close();
}

}
