#include "omc/audio/vorbis.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "omc/audio/ogg.hpp"

namespace omc::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct BitReader {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t byte = 0;
    int bit = 0;
    bool overrun = false;

    BitReader(const std::uint8_t* d, std::size_t sz) : p(d), n(sz) {}

    int read1() {
        if (byte >= n) {
            overrun = true;
            return 0;
        }
        int b = (p[byte] >> bit) & 1;
        if (++bit == 8) {
            bit = 0;
            ++byte;
        }
        return b;
    }
    std::uint32_t read(int bits) {
        std::uint32_t v = 0;
        for (int i = 0; i < bits; ++i) v |= static_cast<std::uint32_t>(read1()) << i;
        return v;
    }
    bool eof() const { return byte >= n; }
};

int ilog(std::uint32_t x) {
    int r = 0;
    while (x > 0) {
        ++r;
        x >>= 1;
    }
    return r;
}

double float32_unpack(std::uint32_t x) {
    std::uint32_t mant = x & 0x1fffffu;
    std::uint32_t sign = x & 0x80000000u;
    std::uint32_t expo = (x & 0x7fe00000u) >> 21;
    double m = static_cast<double>(mant);
    if (sign) m = -m;
    return std::ldexp(m, static_cast<int>(expo) - 788);
}

std::uint32_t lookup1_values(std::uint32_t entries, int dim) {
    std::uint32_t v = 0;
    for (;;) {
        std::uint32_t next = v + 1;
        long double prod = 1.0L;
        bool overflow = false;
        for (int i = 0; i < dim; ++i) {
            prod *= next;
            if (prod > static_cast<long double>(entries)) {
                overflow = true;
                break;
            }
        }
        if (overflow) break;
        v = next;
    }
    return v;
}

struct Codebook {
    int dimensions = 0;
    std::uint32_t entries = 0;
    int lookup_type = 0;
    std::vector<float> value_vectors;

    struct Node {
        int child[2] = {-2, -2};
        int leaf = -1;
    };
    std::vector<Node> tree;

    int new_node() {
        tree.emplace_back();
        return static_cast<int>(tree.size()) - 1;
    }

    void insert(std::uint32_t code, int len, int e) {
        if (tree.empty()) new_node();
        int node = 0;
        for (int i = len - 1; i >= 0; --i) {
            int b = (code >> i) & 1;
            if (tree[node].child[b] < 0) {
                int nn = new_node();
                tree[node].child[b] = nn;
            }
            node = tree[node].child[b];
        }
        tree[node].leaf = e;
    }

    int decode_scalar(BitReader& br) const {
        if (tree.empty()) return -1;
        int node = 0;
        for (;;) {
            if (tree[node].leaf >= 0) return tree[node].leaf;
            int b = br.read1();
            int nxt = tree[node].child[b];
            if (nxt < 0 || br.overrun) return -1;
            node = nxt;
        }
    }
};

void build_huffman(Codebook& cb, const std::vector<int>& lengths) {
    const int n = static_cast<int>(lengths.size());
    int first = -1;
    for (int i = 0; i < n; ++i)
        if (lengths[i] > 0) {
            first = i;
            break;
        }
    if (first < 0) return;

    std::uint32_t available[33] = {0};
    cb.insert(0, lengths[first], first);
    for (int i = 1; i <= lengths[first]; ++i) available[i] = 1u << (32 - i);

    for (int i = first + 1; i < n; ++i) {
        int len = lengths[i];
        if (len <= 0) continue;
        int z = len;
        while (z > 0 && available[z] == 0) --z;
        if (z == 0) throw VorbisError("vorbis: over-specified Huffman codebook");
        std::uint32_t res = available[z];
        available[z] = 0;
        std::uint32_t code = res >> (32 - len);
        cb.insert(code, len, i);
        if (z != len) {
            for (int y = len; y > z; --y) available[y] = res + (1u << (32 - y));
        }
    }
}

void decode_codebook(BitReader& br, Codebook& cb) {
    std::uint32_t sync = br.read(24);
    if (sync != 0x564342u) throw VorbisError("vorbis: bad codebook sync");
    cb.dimensions = static_cast<int>(br.read(16));
    cb.entries = br.read(24);
    if (cb.dimensions <= 0 || cb.entries == 0) throw VorbisError("vorbis: bad codebook shape");

    std::vector<int> lengths(cb.entries, 0);
    int ordered = br.read1();
    if (!ordered) {
        int sparse = br.read1();
        for (std::uint32_t i = 0; i < cb.entries; ++i) {
            if (sparse) {
                if (br.read1())
                    lengths[i] = static_cast<int>(br.read(5)) + 1;
                else
                    lengths[i] = 0;
            } else {
                lengths[i] = static_cast<int>(br.read(5)) + 1;
            }
        }
    } else {
        int current_length = static_cast<int>(br.read(5)) + 1;
        std::uint32_t current_entry = 0;
        while (current_entry < cb.entries) {
            int num = static_cast<int>(br.read(ilog(cb.entries - current_entry)));
            for (int j = 0; j < num && current_entry < cb.entries; ++j)
                lengths[current_entry++] = current_length;
            ++current_length;
        }
    }

    build_huffman(cb, lengths);

    cb.lookup_type = static_cast<int>(br.read(4));
    if (cb.lookup_type == 0) {
    } else if (cb.lookup_type == 1 || cb.lookup_type == 2) {
        double minimum = float32_unpack(br.read(32));
        double delta = float32_unpack(br.read(32));
        int value_bits = static_cast<int>(br.read(4)) + 1;
        int sequence_p = br.read1();
        std::uint32_t lookup_values = (cb.lookup_type == 1)
                                          ? lookup1_values(cb.entries, cb.dimensions)
                                          : cb.entries * static_cast<std::uint32_t>(cb.dimensions);
        std::vector<std::uint32_t> mult(lookup_values);
        for (std::uint32_t i = 0; i < lookup_values; ++i)
            mult[i] = br.read(value_bits);
        if (br.overrun) throw VorbisError("vorbis: codebook VQ overrun");

        cb.value_vectors.assign(static_cast<std::size_t>(cb.entries) * cb.dimensions, 0.0f);
        for (std::uint32_t e = 0; e < cb.entries; ++e) {
            double last = 0.0;
            if (cb.lookup_type == 1) {
                std::uint32_t index_divisor = 1;
                for (int j = 0; j < cb.dimensions; ++j) {
                    std::uint32_t off = (e / index_divisor) % lookup_values;
                    double val = static_cast<double>(mult[off]) * delta + minimum + last;
                    cb.value_vectors[static_cast<std::size_t>(e) * cb.dimensions + j] =
                        static_cast<float>(val);
                    if (sequence_p) last = val;
                    index_divisor *= lookup_values;
                }
            } else {
                std::uint32_t off = e * static_cast<std::uint32_t>(cb.dimensions);
                for (int j = 0; j < cb.dimensions; ++j) {
                    double val = static_cast<double>(mult[off]) * delta + minimum + last;
                    cb.value_vectors[static_cast<std::size_t>(e) * cb.dimensions + j] =
                        static_cast<float>(val);
                    if (sequence_p) last = val;
                    ++off;
                }
            }
        }
    } else {
        throw VorbisError("vorbis: reserved codebook lookup type");
    }
    if (br.overrun) throw VorbisError("vorbis: codebook overrun");
}

struct Floor1 {
    int partitions = 0;
    std::vector<int> partition_class;
    std::vector<int> class_dimensions;
    std::vector<int> class_subclasses;
    std::vector<int> class_masterbooks;
    std::vector<std::vector<int>> subclass_books;
    int multiplier = 0;
    std::vector<int> x_list;
    std::vector<int> x_sort;
    int values = 0;
};

struct Residue {
    int type = 0;
    std::uint32_t begin = 0, end = 0, partition_size = 0;
    int classifications = 0;
    int classbook = 0;
    std::vector<int> cascade;
    std::vector<std::array<int, 8>> books;
};

struct Mapping {
    int submaps = 1;
    int coupling_steps = 0;
    std::vector<int> mag, ang;
    std::vector<int> mux;
    std::vector<int> submap_floor;
    std::vector<int> submap_residue;
};

struct Mode {
    int blockflag = 0;
    int mapping = 0;
};

struct VorbisSetup {
    VorbisIdent ident;
    std::vector<Codebook> codebooks;
    std::vector<int> floor_types;
    std::vector<Floor1> floor1s;
    std::vector<Residue> residues;
    std::vector<Mapping> mappings;
    std::vector<Mode> modes;
};

const std::array<float, 256>& inverse_db_table() {
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        const double lo = std::log(1.0649863e-07);
        const double hi = std::log(1.0);
        for (int i = 0; i < 256; ++i)
            a[i] = static_cast<float>(std::exp(lo + (hi - lo) * (i / 255.0)));
        return a;
    }();
    return t;
}

bool check_vorbis_tag(BitReader& br) {
    const char tag[6] = {'v', 'o', 'r', 'b', 'i', 's'};
    for (int i = 0; i < 6; ++i)
        if (static_cast<char>(br.read(8)) != tag[i]) return false;
    return true;
}

void decode_ident(const ogg::Packet& pk, VorbisIdent& id) {
    BitReader br(pk.data.data(), pk.data.size());
    if (br.read(8) != 1) throw VorbisError("vorbis: expected identification header");
    if (!check_vorbis_tag(br)) throw VorbisError("vorbis: bad ident tag");
    std::uint32_t ver = br.read(32);
    if (ver != 0) throw VorbisError("vorbis: unsupported bitstream version");
    id.channels = static_cast<int>(br.read(8));
    id.rate = br.read(32);
    br.read(32);
    br.read(32);
    br.read(32);
    id.blocksize0 = 1 << br.read(4);
    id.blocksize1 = 1 << br.read(4);
    int framing = br.read1();
    if (id.channels < 1 || id.rate == 0 || framing != 1)
        throw VorbisError("vorbis: bad identification header");
    if (id.blocksize0 < 64 || id.blocksize1 < id.blocksize0 || id.blocksize1 > 8192)
        throw VorbisError("vorbis: bad block sizes");
}

void decode_setup(const ogg::Packet& pk, VorbisSetup& s) {
    BitReader br(pk.data.data(), pk.data.size());
    if (br.read(8) != 5) throw VorbisError("vorbis: expected setup header");
    if (!check_vorbis_tag(br)) throw VorbisError("vorbis: bad setup tag");

    int codebook_count = static_cast<int>(br.read(8)) + 1;
    s.codebooks.resize(codebook_count);
    for (int i = 0; i < codebook_count; ++i) decode_codebook(br, s.codebooks[i]);

    int time_count = static_cast<int>(br.read(6)) + 1;
    for (int i = 0; i < time_count; ++i)
        if (br.read(16) != 0) throw VorbisError("vorbis: nonzero time transform");

    int floor_count = static_cast<int>(br.read(6)) + 1;
    s.floor_types.resize(floor_count);
    s.floor1s.resize(floor_count);
    for (int fi = 0; fi < floor_count; ++fi) {
        int type = static_cast<int>(br.read(16));
        s.floor_types[fi] = type;
        if (type == 1) {
            Floor1& f = s.floor1s[fi];
            f.partitions = static_cast<int>(br.read(5));
            f.partition_class.resize(f.partitions);
            int max_class = -1;
            for (int i = 0; i < f.partitions; ++i) {
                f.partition_class[i] = static_cast<int>(br.read(4));
                if (f.partition_class[i] > max_class) max_class = f.partition_class[i];
            }
            f.class_dimensions.assign(max_class + 1, 0);
            f.class_subclasses.assign(max_class + 1, 0);
            f.class_masterbooks.assign(max_class + 1, 0);
            f.subclass_books.assign(max_class + 1, {});
            for (int c = 0; c <= max_class; ++c) {
                f.class_dimensions[c] = static_cast<int>(br.read(3)) + 1;
                f.class_subclasses[c] = static_cast<int>(br.read(2));
                if (f.class_subclasses[c] > 0)
                    f.class_masterbooks[c] = static_cast<int>(br.read(8));
                int nb = 1 << f.class_subclasses[c];
                f.subclass_books[c].resize(nb);
                for (int j = 0; j < nb; ++j)
                    f.subclass_books[c][j] = static_cast<int>(br.read(8)) - 1;
            }
            f.multiplier = static_cast<int>(br.read(2)) + 1;
            int rangebits = static_cast<int>(br.read(4));
            f.x_list.push_back(0);
            f.x_list.push_back(1 << rangebits);
            for (int i = 0; i < f.partitions; ++i) {
                int cls = f.partition_class[i];
                for (int j = 0; j < f.class_dimensions[cls]; ++j)
                    f.x_list.push_back(static_cast<int>(br.read(rangebits)));
            }
            f.values = static_cast<int>(f.x_list.size());
            f.x_sort.resize(f.values);
            for (int i = 0; i < f.values; ++i) f.x_sort[i] = i;
            for (int i = 1; i < f.values; ++i) {
                int k = f.x_sort[i], j = i - 1;
                while (j >= 0 && f.x_list[f.x_sort[j]] > f.x_list[k]) {
                    f.x_sort[j + 1] = f.x_sort[j];
                    --j;
                }
                f.x_sort[j + 1] = k;
            }
        } else if (type == 0) {
            br.read(8);
            br.read(16);
            br.read(16);
            br.read(6);
            br.read(8);
            int books = static_cast<int>(br.read(4)) + 1;
            for (int i = 0; i < books; ++i) br.read(8);
        } else {
            throw VorbisError("vorbis: unknown floor type");
        }
    }

    int residue_count = static_cast<int>(br.read(6)) + 1;
    s.residues.resize(residue_count);
    for (int ri = 0; ri < residue_count; ++ri) {
        Residue& r = s.residues[ri];
        r.type = static_cast<int>(br.read(16));
        if (r.type > 2) throw VorbisError("vorbis: unknown residue type");
        r.begin = br.read(24);
        r.end = br.read(24);
        r.partition_size = br.read(24) + 1;
        r.classifications = static_cast<int>(br.read(6)) + 1;
        r.classbook = static_cast<int>(br.read(8));
        r.cascade.assign(r.classifications, 0);
        for (int i = 0; i < r.classifications; ++i) {
            int high = 0;
            int low = static_cast<int>(br.read(3));
            int flag = br.read1();
            if (flag) high = static_cast<int>(br.read(5));
            r.cascade[i] = high * 8 + low;
        }
        r.books.assign(r.classifications, {});
        for (int i = 0; i < r.classifications; ++i)
            for (int j = 0; j < 8; ++j)
                r.books[i][j] = (r.cascade[i] & (1 << j)) ? static_cast<int>(br.read(8)) : -1;
    }

    int mapping_count = static_cast<int>(br.read(6)) + 1;
    s.mappings.resize(mapping_count);
    const int ch = s.ident.channels;
    for (int mi = 0; mi < mapping_count; ++mi) {
        Mapping& m = s.mappings[mi];
        if (br.read(16) != 0) throw VorbisError("vorbis: unknown mapping type");
        m.submaps = br.read1() ? static_cast<int>(br.read(4)) + 1 : 1;
        if (br.read1()) {
            m.coupling_steps = static_cast<int>(br.read(8)) + 1;
            m.mag.resize(m.coupling_steps);
            m.ang.resize(m.coupling_steps);
            for (int i = 0; i < m.coupling_steps; ++i) {
                m.mag[i] = static_cast<int>(br.read(ilog(ch - 1)));
                m.ang[i] = static_cast<int>(br.read(ilog(ch - 1)));
                if (m.mag[i] >= ch || m.ang[i] >= ch || m.mag[i] == m.ang[i])
                    throw VorbisError("vorbis: bad coupling channel");
            }
        }
        if (br.read(2) != 0) throw VorbisError("vorbis: nonzero mapping reserved field");
        m.mux.assign(ch, 0);
        if (m.submaps > 1)
            for (int i = 0; i < ch; ++i) {
                m.mux[i] = static_cast<int>(br.read(4));
                if (m.mux[i] >= m.submaps) throw VorbisError("vorbis: bad mapping mux");
            }
        m.submap_floor.resize(m.submaps);
        m.submap_residue.resize(m.submaps);
        for (int i = 0; i < m.submaps; ++i) {
            br.read(8);
            m.submap_floor[i] = static_cast<int>(br.read(8));
            m.submap_residue[i] = static_cast<int>(br.read(8));
        }
    }

    int mode_count = static_cast<int>(br.read(6)) + 1;
    s.modes.resize(mode_count);
    for (int i = 0; i < mode_count; ++i) {
        s.modes[i].blockflag = br.read1();
        if (br.read(16) != 0) throw VorbisError("vorbis: nonzero mode window type");
        if (br.read(16) != 0) throw VorbisError("vorbis: nonzero mode transform type");
        s.modes[i].mapping = static_cast<int>(br.read(8));
    }
    if (br.read1() != 1) throw VorbisError("vorbis: setup framing bit not set");
    if (br.overrun) throw VorbisError("vorbis: setup header overrun");
}

int render_point(int x0, int y0, int x1, int y1, int X) {
    int dy = y1 - y0;
    int adx = x1 - x0;
    int ady = std::abs(dy);
    int err = ady * (X - x0);
    int off = err / adx;
    return (dy < 0) ? (y0 - off) : (y0 + off);
}

void render_line(int x0, int y0, int x1, int y1, std::vector<float>& out, int cap,
                 const std::array<float, 256>& db) {
    int dy = y1 - y0;
    int adx = x1 - x0;
    int ady = std::abs(dy);
    int base = (adx != 0) ? dy / adx : 0;
    int sy = (dy < 0) ? base - 1 : base + 1;
    ady = ady - std::abs(base) * adx;
    int x = x0;
    int y = y0;
    if (x0 < cap) out[x0] = db[y0 & 0xff];
    int err = 0;
    for (x = x0 + 1; x < x1; ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        if (x < cap) out[x] = db[y & 0xff];
    }
}

int low_neighbor(const std::vector<int>& v, int x) {
    int best = -1, best_val = -1;
    for (int i = 0; i < x; ++i)
        if (v[i] < v[x] && v[i] > best_val) {
            best_val = v[i];
            best = i;
        }
    return best;
}
int high_neighbor(const std::vector<int>& v, int x) {
    int best = -1, best_val = 0x7fffffff;
    for (int i = 0; i < x; ++i)
        if (v[i] > v[x] && v[i] < best_val) {
            best_val = v[i];
            best = i;
        }
    return best;
}

bool floor1_decode(BitReader& br, const Floor1& f, const std::vector<Codebook>& books,
                   std::vector<float>& curve, int half) {
    static const int range_of[4] = {256, 128, 86, 64};
    int nonzero = br.read1();
    if (!nonzero) return false;
    int range = range_of[f.multiplier - 1];

    std::vector<int> Y;
    Y.reserve(f.values);
    Y.push_back(static_cast<int>(br.read(ilog(range - 1))));
    Y.push_back(static_cast<int>(br.read(ilog(range - 1))));

    for (int i = 0; i < f.partitions; ++i) {
        int cls = f.partition_class[i];
        int cdim = f.class_dimensions[cls];
        int cbits = f.class_subclasses[cls];
        int csub = (1 << cbits) - 1;
        int cval = 0;
        if (cbits > 0) {
            int bk = f.class_masterbooks[cls];
            cval = books[bk].decode_scalar(br);
            if (cval < 0) throw VorbisError("vorbis: floor1 class decode failed");
        }
        for (int j = 0; j < cdim; ++j) {
            int book = f.subclass_books[cls][cval & csub];
            cval >>= cbits;
            if (book >= 0) {
                int y = books[book].decode_scalar(br);
                if (y < 0) throw VorbisError("vorbis: floor1 Y decode failed");
                Y.push_back(y);
            } else {
                Y.push_back(0);
            }
        }
    }
    if (br.overrun) throw VorbisError("vorbis: floor1 overrun");

    std::vector<int> final_Y(f.values, 0);
    std::vector<char> step2(f.values, 0);
    step2[0] = 1;
    step2[1] = 1;
    final_Y[0] = Y[0];
    final_Y[1] = Y[1];
    for (int i = 2; i < f.values; ++i) {
        int low = low_neighbor(f.x_list, i);
        int high = high_neighbor(f.x_list, i);
        int predicted = render_point(f.x_list[low], final_Y[low], f.x_list[high],
                                      final_Y[high], f.x_list[i]);
        int val = Y[i];
        int highroom = range - predicted;
        int lowroom = predicted;
        int room = (highroom < lowroom ? highroom : lowroom) * 2;
        if (val != 0) {
            step2[low] = 1;
            step2[high] = 1;
            step2[i] = 1;
            if (val >= room) {
                if (highroom > lowroom)
                    final_Y[i] = val - lowroom + predicted;
                else
                    final_Y[i] = predicted - val + highroom - 1;
            } else {
                if (val & 1)
                    final_Y[i] = predicted - ((val + 1) >> 1);
                else
                    final_Y[i] = predicted + (val >> 1);
            }
        } else {
            step2[i] = 0;
            final_Y[i] = predicted;
        }
    }

    const auto& db = inverse_db_table();
    curve.assign(half, 0.0f);
    int hx = 0, hy = 0, lx = 0, ly = final_Y[f.x_sort[0]] * f.multiplier;
    for (int si = 1; si < f.values; ++si) {
        int i = f.x_sort[si];
        if (!step2[i]) continue;
        hy = final_Y[i] * f.multiplier;
        hx = f.x_list[i];
        if (lx < hx) render_line(lx, ly, hx, hy, curve, half, db);
        lx = hx;
        ly = hy;
    }
    if (hx < half) {
        for (int x = hx; x < half; ++x) curve[x] = db[hy & 0xff];
    }
    if (lx == 0) curve[0] = db[ly & 0xff];
    return true;
}

void residue_partition(BitReader& br, const Codebook& book, int type,
                       std::vector<float>& v, std::uint32_t offset, std::uint32_t size) {
    if (type == 0) {
        int step = static_cast<int>(size) / book.dimensions;
        for (int i = 0; i < step; ++i) {
            int entry = book.decode_scalar(br);
            if (entry < 0) throw VorbisError("vorbis: residue0 decode failed");
            const float* vv = &book.value_vectors[static_cast<std::size_t>(entry) * book.dimensions];
            for (int j = 0; j < book.dimensions; ++j)
                v[offset + i + static_cast<std::uint32_t>(j) * step] += vv[j];
        }
    } else {
        std::uint32_t i = 0;
        while (i < size) {
            int entry = book.decode_scalar(br);
            if (entry < 0) throw VorbisError("vorbis: residue1 decode failed");
            const float* vv = &book.value_vectors[static_cast<std::size_t>(entry) * book.dimensions];
            for (int j = 0; j < book.dimensions && i < size; ++j) v[offset + i++] += vv[j];
        }
    }
}

void residue_core(BitReader& br, const Residue& r, const std::vector<Codebook>& books,
                  int type, std::vector<std::vector<float>>& vecs,
                  const std::vector<char>& do_not_decode, std::uint32_t vec_size) {
    const Codebook& classbook = books[r.classbook];
    int classwords = classbook.dimensions;
    std::uint32_t actual_end = r.end <= vec_size ? r.end : vec_size;
    if (actual_end <= r.begin) return;
    std::uint32_t n_to_read = actual_end - r.begin;
    std::uint32_t parts = n_to_read / r.partition_size;
    if (parts == 0) return;

    const int ch = static_cast<int>(vecs.size());
    std::vector<std::vector<int>> classes(ch, std::vector<int>(parts + classwords, 0));

    for (int pass = 0; pass < 8; ++pass) {
        std::uint32_t partition_count = 0;
        while (partition_count < parts) {
            if (pass == 0) {
                for (int j = 0; j < ch; ++j) {
                    if (do_not_decode[j]) continue;
                    int temp = classbook.decode_scalar(br);
                    if (temp < 0) throw VorbisError("vorbis: residue class decode failed");
                    for (int k = classwords - 1; k >= 0; --k) {
                        classes[j][partition_count + k] = temp % r.classifications;
                        temp /= r.classifications;
                    }
                }
            }
            for (int k = 0; k < classwords && partition_count < parts; ++k) {
                for (int j = 0; j < ch; ++j) {
                    if (do_not_decode[j]) continue;
                    int vqclass = classes[j][partition_count];
                    int vqbook = r.books[vqclass][pass];
                    if (vqbook >= 0) {
                        std::uint32_t off = r.begin + partition_count * r.partition_size;
                        residue_partition(br, books[vqbook], type, vecs[j], off, r.partition_size);
                    }
                }
                ++partition_count;
            }
        }
    }
}

void decode_residue(BitReader& br, const Residue& r, const std::vector<Codebook>& books,
                    std::vector<std::vector<float>>& out, const std::vector<char>& do_not_decode,
                    int half) {
    const int ch = static_cast<int>(out.size());
    if (r.type == 2) {
        bool any = false;
        for (int j = 0; j < ch; ++j)
            if (!do_not_decode[j]) any = true;
        if (!any) return;
        std::uint32_t big = static_cast<std::uint32_t>(half) * ch;
        std::vector<std::vector<float>> single(1, std::vector<float>(big, 0.0f));
        std::vector<char> dnd_one(1, 0);
        residue_core(br, r, books, 2, single, dnd_one, big);
        const std::vector<float>& v = single[0];
        for (std::uint32_t i = 0; i < big; ++i) out[i % ch][i / ch] += v[i];
    } else {
        residue_core(br, r, books, r.type, out, do_not_decode,
                     static_cast<std::uint32_t>(half));
    }
}

const std::vector<double>& cos_table(int n) {
    static std::map<int, std::vector<double>> cache;
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;
    int mod = 4 * n;
    std::vector<double> tab(mod);
    for (int j = 0; j < mod; ++j) tab[j] = std::cos((kPi / (2.0 * n)) * j);
    return cache.emplace(n, std::move(tab)).first->second;
}

void imdct(const std::vector<float>& X, std::vector<float>& y, int n) {
    int half = n / 2;
    int mod = 4 * n;
    const std::vector<double>& c = cos_table(n);
    y.assign(n, 0.0f);
    for (int m = 0; m < n; ++m) {
        long long A = 2LL * m + 1 + half;
        double acc = 0.0;
        for (int k = 0; k < half; ++k) {
            long long idx = (A * (2LL * k + 1)) % mod;
            acc += static_cast<double>(X[k]) * c[static_cast<std::size_t>(idx)];
        }
        y[m] = static_cast<float>(acc);
    }
}

struct WindowShape {
    int lws, lwe, left_n;
    int rws, rwe, right_n;
    int n;
};

WindowShape window_shape(int n, int bs0, bool blockflag, bool prev_flag, bool next_flag) {
    WindowShape w;
    w.n = n;
    int center = n / 2;
    if (blockflag && !prev_flag) {
        w.lws = n / 4 - bs0 / 4;
        w.lwe = n / 4 + bs0 / 4;
        w.left_n = bs0 / 2;
    } else {
        w.lws = 0;
        w.lwe = center;
        w.left_n = n / 2;
    }
    if (blockflag && !next_flag) {
        w.rws = (n * 3) / 4 - bs0 / 4;
        w.rwe = (n * 3) / 4 + bs0 / 4;
        w.right_n = bs0 / 2;
    } else {
        w.rws = center;
        w.rwe = n;
        w.right_n = n / 2;
    }
    return w;
}

float window_at(const WindowShape& w, int i) {
    if (i < w.lws) return 0.0f;
    if (i < w.lwe) {
        double x = (kPi / (2.0 * w.left_n)) * (i - w.lws + 0.5);
        double s = std::sin(x);
        return static_cast<float>(std::sin(kPi / 2.0 * s * s));
    }
    if (i < w.rws) return 1.0f;
    if (i < w.rwe) {
        double x = kPi / 2.0 + (kPi / (2.0 * w.right_n)) * (i - w.rws + 0.5);
        double s = std::sin(x);
        return static_cast<float>(std::sin(kPi / 2.0 * s * s));
    }
    return 0.0f;
}

}

VorbisIdent probe_ogg_vorbis(const std::vector<std::uint8_t>& ogg_bytes) {
    ogg::Demux dm = ogg::demux(ogg_bytes);
    if (dm.packets.empty()) throw VorbisError("vorbis: no packets");
    VorbisIdent id;
    decode_ident(dm.packets[0], id);
    return id;
}

VorbisPcm decode_ogg_vorbis(const std::vector<std::uint8_t>& ogg_bytes) {
    ogg::Demux dm = ogg::demux(ogg_bytes);
    if (dm.packets.size() < 3) throw VorbisError("vorbis: missing header packets");

    VorbisSetup s;
    decode_ident(dm.packets[0], s.ident);
    {
        BitReader br(dm.packets[1].data.data(), dm.packets[1].data.size());
        if (br.read(8) != 3) throw VorbisError("vorbis: expected comment header");
        if (!check_vorbis_tag(br)) throw VorbisError("vorbis: bad comment tag");
    }
    decode_setup(dm.packets[2], s);

    const int ch = s.ident.channels;
    const int bs0 = s.ident.blocksize0;
    const int mode_bits = ilog(static_cast<std::uint32_t>(s.modes.size()) - 1);

    VorbisPcm pcm;
    pcm.channels = ch;
    pcm.rate = s.ident.rate;
    pcm.final_granule = dm.last_granule;

    std::vector<std::vector<double>> acc(ch);
    int wpos = 0;
    int prev_wpos = 0;
    int prev_n = 0, prev_rws = 0;
    bool have_prev = false;
    int first_center = 0;
    int last_center = 0;

    auto ensure = [&](int upto) {
        for (int c = 0; c < ch; ++c)
            if (static_cast<int>(acc[c].size()) < upto) acc[c].resize(upto, 0.0);
    };

    for (std::size_t pi = 3; pi < dm.packets.size(); ++pi) {
        const ogg::Packet& pk = dm.packets[pi];
        if (pk.data.empty()) continue;
        BitReader br(pk.data.data(), pk.data.size());
        if (br.read1() != 0) continue;

        int mode_number = static_cast<int>(br.read(mode_bits));
        if (mode_number < 0 || mode_number >= static_cast<int>(s.modes.size()))
            throw VorbisError("vorbis: bad mode number");
        const Mode& mode = s.modes[mode_number];
        bool blockflag = mode.blockflag != 0;
        int n = blockflag ? s.ident.blocksize1 : s.ident.blocksize0;
        int half = n / 2;

        bool prev_flag = false, next_flag = false;
        if (blockflag) {
            prev_flag = br.read1() != 0;
            next_flag = br.read1() != 0;
        }

        const Mapping& map = s.mappings[mode.mapping];

        std::vector<std::vector<float>> curves(ch);
        std::vector<char> no_residue(ch, 0);
        for (int c = 0; c < ch; ++c) {
            int submap = map.mux[c];
            int fnum = map.submap_floor[submap];
            if (s.floor_types[fnum] != 1)
                throw VorbisUnsupported("vorbis: floor type 0 selected (LSP not modeled)");
            bool used = floor1_decode(br, s.floor1s[fnum], s.codebooks, curves[c], half);
            if (!used) {
                no_residue[c] = 1;
                curves[c].assign(half, 0.0f);
            }
        }

        for (int i = 0; i < map.coupling_steps; ++i) {
            if (!no_residue[map.mag[i]] || !no_residue[map.ang[i]]) {
                no_residue[map.mag[i]] = 0;
                no_residue[map.ang[i]] = 0;
            }
        }

        std::vector<std::vector<float>> residue(ch, std::vector<float>(half, 0.0f));
        for (int sm = 0; sm < map.submaps; ++sm) {
            std::vector<int> chlist;
            for (int c = 0; c < ch; ++c)
                if (map.mux[c] == sm) chlist.push_back(c);
            if (chlist.empty()) continue;
            std::vector<std::vector<float>> sub(chlist.size());
            std::vector<char> dnd(chlist.size());
            for (std::size_t k = 0; k < chlist.size(); ++k) {
                sub[k].assign(half, 0.0f);
                dnd[k] = no_residue[chlist[k]];
            }
            const Residue& r = s.residues[map.submap_residue[sm]];
            decode_residue(br, r, s.codebooks, sub, dnd, half);
            for (std::size_t k = 0; k < chlist.size(); ++k) residue[chlist[k]].swap(sub[k]);
        }

        for (int i = map.coupling_steps - 1; i >= 0; --i) {
            std::vector<float>& M = residue[map.mag[i]];
            std::vector<float>& A = residue[map.ang[i]];
            for (int k = 0; k < half; ++k) {
                float m = M[k], a = A[k], nm, na;
                if (m > 0) {
                    if (a > 0) { nm = m; na = m - a; }
                    else { na = m; nm = m + a; }
                } else {
                    if (a > 0) { nm = m; na = m + a; }
                    else { na = m; nm = m - a; }
                }
                M[k] = nm;
                A[k] = na;
            }
        }

        WindowShape wsh = window_shape(n, bs0, blockflag, prev_flag, next_flag);

        if (have_prev) {
            wpos = prev_wpos + prev_rws - wsh.lws;
        } else {
            wpos = 0;
            first_center = n / 2;
        }
        ensure(wpos + n);

        std::vector<float> spec(half), time;
        for (int c = 0; c < ch; ++c) {
            if (no_residue[c]) {
                continue;
            }
            for (int k = 0; k < half; ++k) spec[k] = curves[c][k] * residue[c][k];
            imdct(spec, time, n);
            for (int i = 0; i < n; ++i)
                acc[c][wpos + i] += static_cast<double>(time[i]) * window_at(wsh, i);
        }

        last_center = wpos + n / 2;
        prev_wpos = wpos;
        prev_n = n;
        prev_rws = wsh.rws;
        have_prev = true;
        (void)prev_n;
    }

    if (!have_prev) return pcm;

    int start = first_center;
    long long T;
    if (dm.last_granule > 0)
        T = dm.last_granule;
    else
        T = static_cast<long long>(last_center) - start;
    if (T < 0) T = 0;
    int avail = (ch > 0 && !acc[0].empty()) ? static_cast<int>(acc[0].size()) - start : 0;
    if (avail < 0) avail = 0;
    if (T > avail) T = avail;

    pcm.interleaved.resize(static_cast<std::size_t>(T) * ch);
    for (long long i = 0; i < T; ++i) {
        for (int c = 0; c < ch; ++c) {
            double v = acc[c][start + i] * 32768.0;
            double r = (v >= 0.0) ? std::floor(v + 0.5) : std::ceil(v - 0.5);
            if (r > 32767.0) r = 32767.0;
            if (r < -32768.0) r = -32768.0;
            pcm.interleaved[static_cast<std::size_t>(i) * ch + c] = static_cast<std::int16_t>(r);
        }
    }
    return pcm;
}

}
