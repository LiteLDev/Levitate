#include "core/materials/WorldGen.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::materials {

namespace {

std::uint64_t splitmix(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z               = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

constexpr float kPi = std::numbers::pi_v<float>;

int floorInt(double v) { return static_cast<int>(std::floor(v)); }

} // namespace

// ---- Rng -----------------------------------------------------------------------------------------

Rng::Rng(std::uint64_t seed) {
    for (auto& s : mS) s = splitmix(seed);
}

std::uint64_t Rng::next() {
    std::uint64_t const result = rotl(mS[1] * 5, 7) * 9;
    std::uint64_t const t      = mS[1] << 17;
    mS[2] ^= mS[0];
    mS[3] ^= mS[1];
    mS[1] ^= mS[2];
    mS[0] ^= mS[3];
    mS[2] ^= t;
    mS[3] = rotl(mS[3], 45);
    return result;
}

float  Rng::nextFloat() { return static_cast<float>(next() >> 40) * 0x1.0p-24f; }
double Rng::nextDouble() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
int    Rng::nextInt(int n) { return n <= 1 ? 0 : static_cast<int>((next() >> 33) % static_cast<std::uint64_t>(n)); }

std::uint64_t featureSeed(std::uint64_t worldSeed, int chunkX, int chunkZ, std::uint32_t salt) {
    std::uint64_t x = worldSeed;
    x ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunkX)) * 0x9E3779B97F4A7C15ull;
    x ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunkZ)) * 0xC2B2AE3D27D4EB4Full;
    x ^= static_cast<std::uint64_t>(salt) * 0x165667B19E3779F9ull;
    return splitmix(x);
}

// ---- Simplex noise (3D, after Gustavson's public domain reference) -------------------------------

namespace {
constexpr int kGrad3[12][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
};
double dot3(int const g[3], double x, double y, double z) { return g[0] * x + g[1] * y + g[2] * z; }
} // namespace

Simplex::Simplex(Rng& rng) {
    std::array<int, 256> p{};
    for (int i = 0; i < 256; ++i) p[i] = i;
    for (int i = 0; i < 256; ++i) {
        int j = rng.nextInt(256 - i) + i;
        std::swap(p[i], p[j]);
    }
    for (int i = 0; i < 512; ++i) mPerm[i] = p[i & 255];
}

double Simplex::value(double xin, double yin, double zin) const {
    constexpr double F3 = 1.0 / 3.0;
    constexpr double G3 = 1.0 / 6.0;
    double           s  = (xin + yin + zin) * F3;
    int              i = floorInt(xin + s), j = floorInt(yin + s), k = floorInt(zin + s);
    double           t  = (i + j + k) * G3;
    double           x0 = xin - (i - t), y0 = yin - (j - t), z0 = zin - (k - t);
    int              i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }
    double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
    double x2 = x0 - i2 + 2 * G3, y2 = y0 - j2 + 2 * G3, z2 = z0 - k2 + 2 * G3;
    double x3 = x0 - 1 + 3 * G3, y3 = y0 - 1 + 3 * G3, z3 = z0 - 1 + 3 * G3;
    int    ii = i & 255, jj = j & 255, kk = k & 255;
    auto   corner = [&](double x, double y, double z, int gi) {
        double tt = 0.6 - x * x - y * y - z * z;
        if (tt < 0) return 0.0;
        tt *= tt;
        return tt * tt * dot3(kGrad3[gi % 12], x, y, z);
    };
    double n0 = corner(x0, y0, z0, mPerm[ii + mPerm[jj + mPerm[kk]]]);
    double n1 = corner(x1, y1, z1, mPerm[ii + i1 + mPerm[jj + j1 + mPerm[kk + k1]]]);
    double n2 = corner(x2, y2, z2, mPerm[ii + i2 + mPerm[jj + j2 + mPerm[kk + k2]]]);
    double n3 = corner(x3, y3, z3, mPerm[ii + 1 + mPerm[jj + 1 + mPerm[kk + 1]]]);
    return 32.0 * (n0 + n1 + n2 + n3);
}

// ---- patterns ------------------------------------------------------------------------------------

namespace {
using G = Ground;
using M = Mat;

/// Layer.Builder.block(b): the same block in stone and deepslate.
std::vector<Target> both(Mat m) { return {{G::Stone, m}, {G::Deepslate, m}}; }
/// Layer.Builder.blocks(stone, deep)
std::vector<Target> pair(Mat stone, Mat deep) { return {{G::Stone, stone}, {G::Deepslate, deep}}; }
/// nether mode block(b)
std::vector<Target> nether(Mat m) { return {{G::Nether, m}}; }

Layer layer(std::vector<std::vector<Target>> targets, int minSize, int maxSize, int weight) {
    return Layer{std::move(targets), minSize, maxSize, weight};
}
} // namespace

std::vector<LayerPattern> const& overworldPatterns() {
    static std::vector<LayerPattern> const patterns = {
        // SCORIA
        {layer({both(M::Scoria)}, 2, 5, 2), layer({both(M::Tuff), both(M::Andesite)}, 2, 3, 2),
         layer({pair(M::Tuff, M::Andesite)}, 2, 2, 1), layer({both(M::Diorite)}, 1, 2, 1)},
        // CRIMSITE
        {layer({both(M::Crimsite)}, 2, 5, 2), layer({both(M::Tuff), both(M::Deepslate)}, 2, 3, 1),
         layer({pair(M::Deepslate, M::Tuff)}, 2, 2, 1), layer({both(M::Limestone)}, 1, 2, 1)},
        // ASURINE
        {layer({both(M::Asurine)}, 2, 5, 2), layer({both(M::Tuff), both(M::Deepslate)}, 2, 3, 1),
         layer({pair(M::Deepslate, M::Tuff)}, 2, 2, 1), layer({both(M::Calcite)}, 1, 2, 1)},
        // VERIDIUM
        {layer({both(M::Veridium)}, 2, 5, 4), layer({both(M::Tuff), both(M::Andesite)}, 2, 3, 2),
         layer({pair(M::Tuff, M::Andesite)}, 2, 2, 2), layer({both(M::SmoothBasalt)}, 1, 2, 3)},
        // LIMESTONE
        {layer({pair(M::Stone, M::Deepslate)}, 1, 1, 1), layer({both(M::Calcite)}, 1, 1, 2),
         layer({both(M::Diorite)}, 1, 1, 1), layer({both(M::Limestone)}, 2, 4, 2)},
        // OCHRUM
        {layer({both(M::Ochrum)}, 2, 5, 2), layer({both(M::Tuff), both(M::Deepslate)}, 2, 3, 2),
         layer({both(M::DripstoneBlock)}, 1, 2, 2)},
    };
    return patterns;
}

std::vector<LayerPattern> const& netherPatterns() {
    static std::vector<LayerPattern> const patterns = {
        // SCORIA_NETHER
        {layer({pair(M::Stone, M::Deepslate)}, 1, 1, 1), layer({nether(M::Scoria)}, 2, 5, 2),
         layer({nether(M::Blackstone)}, 1, 2, 1), layer({nether(M::Basalt), nether(M::SmoothBasalt)}, 2, 3, 1)},
        // SCORCHIA_NETHER
        {layer({pair(M::Stone, M::Deepslate)}, 1, 1, 4), layer({nether(M::Scorchia)}, 2, 5, 8),
         layer({nether(M::SoulSoil), nether(M::SoulSand)}, 1, 3, 4), layer({nether(M::MagmaBlock)}, 1, 1, 1),
         layer({nether(M::Basalt), nether(M::SmoothBasalt)}, 2, 3, 4)},
    };
    return patterns;
}

std::vector<Target> const& zincTargets() {
    static std::vector<Target> const targets = {{G::Stone, M::ZincOre}, {G::Deepslate, M::DeepslateZincOre}};
    return targets;
}

int rollNextLayer(LayerPattern const& pattern, int previous, Rng& rng) {
    int total = 0;
    for (int i = 0; i < static_cast<int>(pattern.size()); ++i)
        if (i != previous) total += pattern[i].weight;
    if (total <= 0) return previous;
    int rolled = rng.nextInt(total);
    for (int i = 0; i < static_cast<int>(pattern.size()); ++i) {
        if (i == previous) continue;
        rolled -= pattern[i].weight;
        if (rolled < 0) return i;
    }
    return previous;
}

// ---- features ------------------------------------------------------------------------------------

namespace {
bool tryPlace(World const& world, int x, int y, int z, std::vector<Target> const& targets) {
    if (y < world.minY || y >= world.maxY || !world.canWrite(x, y, z)) return false;
    Ground g = world.ground(x, y, z);
    if (g == Ground::Other) return false;
    for (auto const& t : targets) {
        if (t.ground != g) continue;
        world.set(x, y, z, t.block);
        return true;
    }
    return false;
}
} // namespace

int placeOre(World const& world, Rng& rng, int ox, int oy, int oz, int size, std::vector<Target> const& targets) {
    float  f  = rng.nextFloat() * kPi;
    float  g  = size / 8.0f;
    int    i  = static_cast<int>(std::ceil((size / 16.0f * 2.0f + 1.0f) / 2.0f));
    double d1 = ox + std::sin(f) * g, d2 = ox - std::sin(f) * g;
    double h1 = oz + std::cos(f) * g, h2 = oz - std::cos(f) * g;
    double l1 = oy + rng.nextInt(3) - 2, l2 = oy + rng.nextInt(3) - 2;
    int    cg = static_cast<int>(std::ceil(g));
    int    bx = ox - cg - i, by = oy - 2 - i, bz = oz - cg - i;
    int    width = 2 * (cg + i), height = 2 * (2 + i);
    bool   below = false;
    for (int sx = bx; sx <= bx + width && !below; ++sx)
        for (int sz = bz; sz <= bz + width; ++sz)
            if (by <= world.surface(sx, sz)) {
                below = true;
                break;
            }
    if (!below) return 0;

    std::vector<double> ds(static_cast<size_t>(size) * 4);
    for (int k = 0; k < size; ++k) {
        float  t    = static_cast<float>(k) / static_cast<float>(size);
        double x    = d1 + (d2 - d1) * t;
        double y    = l1 + (l2 - l1) * t;
        double z    = h1 + (h2 - h1) * t;
        double r    = rng.nextDouble() * size / 16.0;
        double rad  = ((std::sin(kPi * t) + 1.0) * r + 1.0) / 2.0;
        ds[k * 4]     = x;
        ds[k * 4 + 1] = y;
        ds[k * 4 + 2] = z;
        ds[k * 4 + 3] = rad;
    }
    for (int k = 0; k < size - 1; ++k) {
        if (ds[k * 4 + 3] <= 0) continue;
        for (int l = k + 1; l < size; ++l) {
            if (ds[l * 4 + 3] <= 0) continue;
            double dx = ds[k * 4] - ds[l * 4], dy = ds[k * 4 + 1] - ds[l * 4 + 1], dz = ds[k * 4 + 2] - ds[l * 4 + 2];
            double dr = ds[k * 4 + 3] - ds[l * 4 + 3];
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                if (dr > 0) ds[l * 4 + 3] = -1;
                else ds[k * 4 + 3] = -1;
            }
        }
    }
    std::vector<bool> done(static_cast<size_t>((width + 1) * (height + 1) * (width + 1)), false);
    int               placed = 0;
    for (int m = 0; m < size; ++m) {
        double r = ds[m * 4 + 3];
        if (r < 0) continue;
        double cx = ds[m * 4], cy = ds[m * 4 + 1], cz = ds[m * 4 + 2];
        int    x0 = std::max(floorInt(cx - r), bx), y0 = std::max(floorInt(cy - r), by), z0 = std::max(floorInt(cz - r), bz);
        int    x1 = std::max(floorInt(cx + r), x0), y1 = std::max(floorInt(cy + r), y0), z1 = std::max(floorInt(cz + r), z0);
        for (int x = x0; x <= x1; ++x) {
            double u = (x + 0.5 - cx) / r;
            if (u * u >= 1) continue;
            for (int y = y0; y <= y1; ++y) {
                double w = (y + 0.5 - cy) / r;
                if (u * u + w * w >= 1) continue;
                for (int z = z0; z <= z1; ++z) {
                    double v = (z + 0.5 - cz) / r;
                    if (u * u + w * w + v * v >= 1) continue;
                    int lx = x - bx, ly = y - by, lz = z - bz;
                    if (lx < 0 || ly < 0 || lz < 0 || lx > width || ly > height || lz > width) continue;
                    size_t idx = static_cast<size_t>(lx + ly * (width + 1) + lz * (width + 1) * (height + 1));
                    if (done[idx]) continue;
                    done[idx] = true;
                    if (tryPlace(world, x, y, z, targets)) ++placed;
                }
            }
        }
    }
    return placed;
}

int placeLayered(World const& world, Rng& rng, int ox, int oy, int oz, int configSize, std::vector<LayerPattern> const& patterns) {
    if (patterns.empty()) return 0;
    LayerPattern const& pattern = patterns[rng.nextInt(static_cast<int>(patterns.size()))];

    int   size        = configSize + 1;
    float radius      = configSize * 0.5f;
    int   radiusBound = static_cast<int>(std::ceil(radius)) - 1;
    if (oy >= world.surface(ox, oz)) return 0;

    struct Temp {
        int   layer;
        float size;
    };
    struct Resolved {
        int   layer;
        float radialThreshold;
        float rampStart;
    };
    std::vector<Temp> temp;
    float             total   = 0;
    int               current = -1;
    while (total < size) {
        int   next = rollNextLayer(pattern, current, rng);
        float sz   = rng.between(static_cast<float>(pattern[next].minSize), static_cast<float>(pattern[next].maxSize));
        temp.push_back({next, sz});
        total += sz;
        current = next;
        if (sz <= 0 && temp.size() > 4096) break; // degenerate pattern guard
    }
    std::vector<Resolved> resolved;
    float                 cumulative = -(total - size) * rng.nextFloat();
    for (auto const& t : temp) {
        float rampStart = resolved.empty() ? -INFINITY : cumulative * (2.0f / size) - 1.0f;
        cumulative += t.size;
        if (cumulative < 0) continue;
        float radial = rng.between(0.5f, 1.0f);
        resolved.push_back({t.layer, radial, rampStart});
    }
    if (resolved.empty()) return 0;

    float gy      = std::cbrt(rng.between(-1.0f, 1.0f)); // layers tend towards horizontal
    float xz      = std::sqrt(std::max(0.0f, 1.0f - gy * gy));
    float theta   = rng.nextFloat() * 2 * kPi;
    float gx      = std::cos(theta) * xz;
    float gz      = std::sin(theta) * xz;
    Simplex displacement(rng);
    Simplex radial(rng);

    constexpr float kMaxDisplacement = 1.75f;
    constexpr float kLayerFrequency  = 0.125f;
    constexpr float kMaxRadialCut    = 0.25f;
    constexpr float kRadialFrequency = 0.125f;

    int placed = 0;
    for (int bz = -radiusBound; bz <= radiusBound; ++bz) {
        float dz = bz * (1.0f / radius);
        if (dz * dz > 1) continue;
        for (int bx = -radiusBound; bx <= radiusBound; ++bx) {
            float dx = bx * (1.0f / radius);
            if (dz * dz + dx * dx > 1) continue;
            for (int by = -radiusBound; by <= radiusBound; ++by) {
                float dy   = by * (1.0f / radius);
                float dist = dz * dz + dx * dx + dy * dy;
                if (dist > 1) continue;
                int x = ox + bx, y = oy + by, z = oz + bz;
                if (y < world.minY || y >= world.maxY) continue;
                float ramp = gx * dx + gy * dy + gz * dz;
                ramp += static_cast<float>(displacement.value(x * kLayerFrequency, y * kLayerFrequency, z * kLayerFrequency))
                      * (kMaxDisplacement / size);
                // the last layer whose ramp start is <= ramp (Collections.binarySearch semantics)
                auto it = std::upper_bound(resolved.begin(), resolved.end(), ramp, [](float v, Resolved const& r) {
                    return v < r.rampStart;
                });
                Resolved const& entry = it == resolved.begin() ? resolved.front() : *(it - 1);
                if (dist > entry.radialThreshold) continue;
                float noise = static_cast<float>(radial.value(x * kRadialFrequency, y * kRadialFrequency, z * kRadialFrequency));
                float cut   = (1.0f - kMaxRadialCut) + (noise + 1.0f) * 0.5f * kMaxRadialCut; // map [-1,1] -> [0.75,1]
                if (dist > entry.radialThreshold * cut) continue;
                auto const& alternatives = pattern[entry.layer].targets;
                auto const& targets      = alternatives.size() == 1 ? alternatives[0] : alternatives[rng.nextInt(static_cast<int>(alternatives.size()))];
                if (tryPlace(world, x, y, z, targets)) ++placed;
            }
        }
    }
    return placed;
}

std::vector<Origin> zincOrigins(Rng& rng, int cx, int cz) {
    std::vector<Origin> out;
    for (int i = 0; i < 8; ++i) {
        int x = cx * 16 + rng.nextInt(16);
        int z = cz * 16 + rng.nextInt(16);
        int y = -63 + rng.nextInt(70 - -63 + 1);
        out.push_back({x, y, z});
    }
    return out;
}

std::vector<Origin> striatedOrigins(Rng& rng, int cx, int cz, bool nether) {
    std::vector<Origin> out;
    if (rng.nextFloat() >= 1.0f / 18.0f) return out;
    int x  = cx * 16 + rng.nextInt(16);
    int z  = cz * 16 + rng.nextInt(16);
    int lo = nether ? 40 : -30, hi = nether ? 90 : 70;
    int y  = lo + rng.nextInt(hi - lo + 1);
    out.push_back({x, y, z});
    return out;
}

int decorateChunk(World const& world, std::uint64_t worldSeed, int dim, int cx, int cz) {
    int placed = 0;
    if (dim == 0) {
        Rng zinc(featureSeed(worldSeed, cx, cz, 0x7A1C));
        for (auto const& o : zincOrigins(zinc, cx, cz)) placed += placeOre(world, zinc, o.x, o.y, o.z, 12, zincTargets());
        Rng layers(featureSeed(worldSeed, cx, cz, 0x57A7));
        for (auto const& o : striatedOrigins(layers, cx, cz, false))
            placed += placeLayered(world, layers, o.x, o.y, o.z, 32, overworldPatterns());
    } else if (dim == 1) {
        Rng layers(featureSeed(worldSeed, cx, cz, 0x4E7E));
        for (auto const& o : striatedOrigins(layers, cx, cz, true))
            placed += placeLayered(world, layers, o.x, o.y, o.z, 32, netherPatterns());
    }
    return placed;
}

} // namespace lv::core::materials
