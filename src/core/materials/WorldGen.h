#pragma once

// World generation of the materials unit (U9), game independent: Create 6.0.10's zinc ore veins
// (vanilla OreFeature, size 12) and striated stone layers (Create's LayeredOreFeature, size 32)
// with their placements. The game adapter supplies block lookups and writes.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace lv::core::materials {

/// Small deterministic generator (splitmix64 seeded xoshiro256**).
class Rng {
public:
    explicit Rng(std::uint64_t seed);
    std::uint64_t next();
    /// [0, 1)
    float  nextFloat();
    double nextDouble();
    /// [0, n), n > 0
    int nextInt(int n);
    /// Create's Mth.randomBetween: min + nextFloat() * (max - min)
    float between(float min, float max) { return min + nextFloat() * (max - min); }

private:
    std::array<std::uint64_t, 4> mS{};
};

/// Seed of one feature in one chunk (world seed, chunk, salt).
[[nodiscard]] std::uint64_t featureSeed(std::uint64_t worldSeed, int chunkX, int chunkZ, std::uint32_t salt);

/// 3D simplex noise in [-1, 1] with a permutation shuffled by `rng` (Create's SimplexNoise).
class Simplex {
public:
    explicit Simplex(Rng& rng);
    [[nodiscard]] double value(double x, double y, double z) const;

private:
    std::array<int, 512> mPerm{};
};

/// What a world position holds, as far as the rule tests care.
enum class Ground : std::uint8_t {
    Other,     ///< anything else (air, water, ores, ...)
    Stone,     ///< #minecraft:stone_ore_replaceables (stone, granite, diorite, andesite)
    Deepslate, ///< #minecraft:deepslate_ore_replaceables (deepslate, tuff)
    Nether,    ///< #minecraft:base_stone_nether (netherrack, basalt, blackstone)
};

/// Blocks the features place (the adapter maps them to game blocks).
enum class Mat : std::uint8_t {
    ZincOre, DeepslateZincOre, Stone, Deepslate, Tuff, Andesite, Diorite, Calcite, SmoothBasalt, DripstoneBlock,
    Asurine, Crimsite, Limestone, Ochrum, Veridium, Scoria, Scorchia, Blackstone, Basalt, SoulSoil, SoulSand,
    MagmaBlock, Count
};

/// OreConfiguration.TargetBlockState: place `block` where the ground matches `ground`.
struct Target {
    Ground ground;
    Mat    block;
};

/// Create's LayerPattern.Layer: alternatives (each a list of targets), thickness, weight.
struct Layer {
    std::vector<std::vector<Target>> targets;
    int                              minSize = 1;
    int                              maxSize = 1;
    int                              weight  = 1;
};
using LayerPattern = std::vector<Layer>;

/// Create's overworld and nether striated patterns (AllLayerPatterns).
[[nodiscard]] std::vector<LayerPattern> const& overworldPatterns();
[[nodiscard]] std::vector<LayerPattern> const& netherPatterns();
/// Create's LayerPattern.rollNext: weighted pick of a layer other than `previous` (index or -1).
[[nodiscard]] int rollNextLayer(LayerPattern const& pattern, int previous, Rng& rng);

/// World access for a feature. Positions outside the writable area must report `false` from
/// `canWrite`; `surface(x, z)` is the OCEAN_FLOOR_WG height (first y above the top solid block).
struct World {
    std::function<Ground(int x, int y, int z)>         ground;
    std::function<bool(int x, int y, int z)>           canWrite;
    std::function<void(int x, int y, int z, Mat block)> set;
    std::function<int(int x, int z)>                   surface;
    int                                                minY = -64;
    int                                                maxY = 320; ///< exclusive
};

/// Vanilla OreFeature with discard_chance_on_air_exposure 0 (Create's zinc ore, size 12).
int placeOre(World const& world, Rng& rng, int x, int y, int z, int size, std::vector<Target> const& targets);
/// Create's LayeredOreFeature (size 32): stripes of a random pattern across a noisy sphere.
int placeLayered(World const& world, Rng& rng, int x, int y, int z, int size, std::vector<LayerPattern> const& patterns);

/// Create's zinc ore targets (stone -> zinc ore, deepslate -> deepslate zinc ore).
[[nodiscard]] std::vector<Target> const& zincTargets();

/// One feature origin chosen by a placement.
struct Origin {
    int x, y, z;
};
/// Placements of Create's placed features in chunk (cx, cz):
///   zinc_ore: count 8, in square, uniform y -63..70
///   striated_ores_overworld: rarity 1/18, in square, uniform y -30..70
///   striated_ores_nether: rarity 1/18, in square, uniform y 40..90
[[nodiscard]] std::vector<Origin> zincOrigins(Rng& rng, int cx, int cz);
[[nodiscard]] std::vector<Origin> striatedOrigins(Rng& rng, int cx, int cz, bool nether);

/// Everything Create generates in one chunk (zinc + striated layers in the overworld, striated
/// layers in the nether). `dim` 0 overworld, 1 nether, other dimensions get nothing. Returns the
/// number of blocks placed.
int decorateChunk(World const& world, std::uint64_t worldSeed, int dim, int cx, int cz);

} // namespace lv::core::materials
