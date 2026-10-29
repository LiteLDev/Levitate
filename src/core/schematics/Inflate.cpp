#include "core/schematics/Inflate.h"

#include <array>
#include <vector>

namespace lv::core::schematics {

namespace {

struct BitReader {
    std::string_view in;
    std::size_t      pos   = 0;
    std::uint32_t    bits  = 0;
    int              count = 0;
    bool             error = false;

    int bit() {
        if (count == 0) {
            if (pos >= in.size()) {
                error = true;
                return 0;
            }
            bits  = static_cast<std::uint8_t>(in[pos++]);
            count = 8;
        }
        int b = static_cast<int>(bits & 1u);
        bits >>= 1;
        --count;
        return b;
    }
    int get(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v |= bit() << i;
        return v;
    }
    void align() { count = 0; }
};

/// Canonical Huffman decoder (counts per length + symbols sorted by code).
struct Huffman {
    std::array<std::uint16_t, 16> counts{};
    std::vector<std::uint16_t>    symbols;

    bool build(std::uint8_t const* lengths, int n) {
        counts.fill(0);
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) return false; // over-subscribed
        }
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + counts[len]);
        symbols.assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
        return true;
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bit();
            int count = counts[len];
            if (code - count < first) return symbols[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (br.error) return -1;
        }
        return -1;
    }
};

constexpr std::array<std::uint16_t, 29> kLenBase{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29>  kLenExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase{1,    2,    3,    4,    5,    7,     9,     13,    17,  25,   33,   49,   65,   97,   129,
                                                  193,  257,  385,  513,  769,  1025,  1537,  2049,  3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30>  kDistExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, Huffman const& lit, Huffman const& dist, std::string& out, std::size_t maxOutput) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0 || br.error) return false;
        if (sym < 256) {
            if (out.size() >= maxOutput) return false;
            out.push_back(static_cast<char>(sym));
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = kLenBase[static_cast<std::size_t>(sym)] + br.get(kLenExtra[static_cast<std::size_t>(sym)]);
        int ds  = dist.decode(br);
        if (ds < 0 || ds >= 30) return false;
        std::size_t d = kDistBase[static_cast<std::size_t>(ds)] + static_cast<std::size_t>(br.get(kDistExtra[static_cast<std::size_t>(ds)]));
        if (br.error || d > out.size()) return false;
        if (out.size() + static_cast<std::size_t>(len) > maxOutput) return false;
        std::size_t from = out.size() - d;
        for (int i = 0; i < len; ++i) out.push_back(out[from + static_cast<std::size_t>(i)]);
    }
}

} // namespace

std::optional<std::string> inflateRaw(std::string_view data, std::size_t maxOutput, std::size_t* consumed) {
    BitReader   br{data};
    std::string out;
    int         last = 0;
    do {
        last     = br.bit();
        int type = br.get(2);
        if (br.error) return std::nullopt;
        if (type == 0) {
            br.align();
            if (br.pos + 4 > data.size()) return std::nullopt;
            auto b = [&](std::size_t i) { return static_cast<std::uint8_t>(data[br.pos + i]); };
            unsigned len  = b(0) | (b(1) << 8);
            unsigned nlen = b(2) | (b(3) << 8);
            if ((len ^ 0xFFFFu) != nlen) return std::nullopt;
            br.pos += 4;
            if (br.pos + len > data.size() || out.size() + len > maxOutput) return std::nullopt;
            out.append(data.substr(br.pos, len));
            br.pos += len;
        } else if (type == 1) {
            static Huffman const* fixedLit  = nullptr;
            static Huffman const* fixedDist = nullptr;
            static Huffman        sLit, sDist;
            if (!fixedLit) {
                std::array<std::uint8_t, 288> l{};
                for (int i = 0; i < 144; ++i) l[static_cast<std::size_t>(i)] = 8;
                for (int i = 144; i < 256; ++i) l[static_cast<std::size_t>(i)] = 9;
                for (int i = 256; i < 280; ++i) l[static_cast<std::size_t>(i)] = 7;
                for (int i = 280; i < 288; ++i) l[static_cast<std::size_t>(i)] = 8;
                sLit.build(l.data(), 288);
                std::array<std::uint8_t, 30> d{};
                d.fill(5);
                sDist.build(d.data(), 30);
                fixedLit  = &sLit;
                fixedDist = &sDist;
            }
            if (!inflateBlock(br, *fixedLit, *fixedDist, out, maxOutput)) return std::nullopt;
        } else if (type == 2) {
            int hlit  = br.get(5) + 257;
            int hdist = br.get(5) + 1;
            int hclen = br.get(4) + 4;
            if (br.error || hlit > 286 || hdist > 30) return std::nullopt;
            static constexpr std::array<std::uint8_t, 19> kOrder{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, 19> cl{};
            for (int i = 0; i < hclen; ++i) cl[kOrder[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(br.get(3));
            Huffman clh;
            if (!clh.build(cl.data(), 19)) return std::nullopt;
            std::array<std::uint8_t, 320> lens{};
            int                           n = 0;
            while (n < hlit + hdist) {
                int sym = clh.decode(br);
                if (sym < 0 || br.error) return std::nullopt;
                if (sym < 16) {
                    lens[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(sym);
                    continue;
                }
                int          rep = 0;
                std::uint8_t val = 0;
                if (sym == 16) {
                    if (n == 0) return std::nullopt;
                    val = lens[static_cast<std::size_t>(n - 1)];
                    rep = 3 + br.get(2);
                } else if (sym == 17) {
                    rep = 3 + br.get(3);
                } else {
                    rep = 11 + br.get(7);
                }
                if (n + rep > hlit + hdist) return std::nullopt;
                while (rep--) lens[static_cast<std::size_t>(n++)] = val;
            }
            if (lens[256] == 0) return std::nullopt;
            Huffman lit, dist;
            if (!lit.build(lens.data(), hlit) || !dist.build(lens.data() + hlit, hdist)) return std::nullopt;
            if (!inflateBlock(br, lit, dist, out, maxOutput)) return std::nullopt;
        } else {
            return std::nullopt;
        }
    } while (!last);
    if (consumed) *consumed = br.pos;
    return out;
}

std::uint32_t crc32(std::string_view data) {
    static std::array<std::uint32_t, 256> const table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (char ch : data) c = table[(c ^ static_cast<std::uint8_t>(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool isGzip(std::string_view data) {
    return data.size() >= 2 && static_cast<std::uint8_t>(data[0]) == 0x1F && static_cast<std::uint8_t>(data[1]) == 0x8B;
}

std::optional<std::string> gunzip(std::string_view data, std::size_t maxOutput) {
    if (data.size() < 18 || !isGzip(data) || static_cast<std::uint8_t>(data[2]) != 8) return std::nullopt;
    auto        flags = static_cast<std::uint8_t>(data[3]);
    std::size_t pos   = 10;
    if (flags & 4) { // FEXTRA
        if (pos + 2 > data.size()) return std::nullopt;
        std::size_t xlen = static_cast<std::uint8_t>(data[pos]) | (static_cast<std::uint8_t>(data[pos + 1]) << 8);
        pos += 2 + xlen;
    }
    for (int f : {8, 16}) { // FNAME, FCOMMENT
        if (!(flags & f)) continue;
        while (pos < data.size() && data[pos] != 0) ++pos;
        ++pos;
    }
    if (flags & 2) pos += 2; // FHCRC
    if (pos >= data.size()) return std::nullopt;
    std::size_t used = 0;
    auto        out  = inflateRaw(data.substr(pos), maxOutput, &used);
    if (!out) return std::nullopt;
    std::size_t tail = pos + used;
    if (tail + 8 > data.size()) return std::nullopt;
    auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at])) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 1])) << 8)
             | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 2])) << 16)
             | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 3])) << 24);
    };
    if (u32(tail) != crc32(*out)) return std::nullopt;
    if (u32(tail + 4) != static_cast<std::uint32_t>(out->size() & 0xFFFFFFFFu)) return std::nullopt;
    return out;
}

std::string gzipStored(std::string_view data) {
    std::string out{'\x1f', '\x8b', '\x08', '\0', '\0', '\0', '\0', '\0', '\0', '\xff'};
    std::size_t pos = 0;
    do {
        std::size_t n    = std::min<std::size_t>(data.size() - pos, 0xFFFF);
        bool        last = pos + n >= data.size();
        out.push_back(last ? '\x01' : '\x00');
        out.push_back(static_cast<char>(n & 0xFF));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(~n & 0xFF));
        out.push_back(static_cast<char>((~n >> 8) & 0xFF));
        out.append(data.substr(pos, n));
        pos += n;
    } while (pos < data.size());
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    put32(crc32(data));
    put32(static_cast<std::uint32_t>(data.size()));
    return out;
}

} // namespace lv::core::schematics
