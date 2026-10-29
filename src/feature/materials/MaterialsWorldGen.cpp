// Materials unit (U9): world generation. Create adds its zinc ore veins and striated stone layers
// as data-driven features (vanilla ore / Create's layered_ore); Bedrock's feature JSON can't
// express the layered feature, so both run natively when a chunk is decorated, with Create's
// placements (src/core/materials/WorldGen.*).

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/levelgen/v1/NetherGenerator.h"
#include "mc/world/level/levelgen/v1/OverworldGeneratorMultinoise.h"

#include "core/materials/WorldGen.h"
#include "feature/materials/Materials.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::materials {

namespace cm = core::materials;

namespace {

/// Game blocks of the features, resolved once per world (thread safe, read-only afterwards).
struct Palette {
    std::array<Block const*, static_cast<size_t>(cm::Mat::Count)> mats{};
    std::array<BlockType const*, 4>                                 stone{};
    std::array<BlockType const*, 2>                                 deepslate{};
    std::array<BlockType const*, 3>                                 nether{};
    bool                                                            complete = false;
};

std::mutex                     gMutex;
std::shared_ptr<Palette const> gPalette;
std::atomic<long long>         gDecorated{0};
std::atomic<bool>              gAnnounced{false};

BlockType const* typeOf(char const* id) {
    auto t = BlockType::tryGetFromRegistry(HashedString(id));
    return t ? t.as_ptr() : nullptr;
}
Block const* blockOf(char const* id) {
    auto const* t = typeOf(id);
    return t ? t->mDefaultState : nullptr;
}

std::shared_ptr<Palette const> palette() {
    std::lock_guard lock(gMutex);
    if (gPalette) return gPalette;
    auto p   = std::make_shared<Palette>();
    using M  = cm::Mat;
    auto set = [&](M m, char const* id) { p->mats[static_cast<size_t>(m)] = blockOf(id); };
    set(M::ZincOre, "levitate:zinc_ore");
    set(M::DeepslateZincOre, "levitate:deepslate_zinc_ore");
    set(M::Stone, "minecraft:stone");
    set(M::Deepslate, "minecraft:deepslate");
    set(M::Tuff, "minecraft:tuff");
    set(M::Andesite, "minecraft:andesite");
    set(M::Diorite, "minecraft:diorite");
    set(M::Calcite, "minecraft:calcite");
    set(M::SmoothBasalt, "minecraft:smooth_basalt");
    set(M::DripstoneBlock, "minecraft:dripstone_block");
    set(M::Asurine, "levitate:asurine");
    set(M::Crimsite, "levitate:crimsite");
    set(M::Limestone, "levitate:limestone");
    set(M::Ochrum, "levitate:ochrum");
    set(M::Veridium, "levitate:veridium");
    set(M::Scoria, "levitate:scoria");
    set(M::Scorchia, "levitate:scorchia");
    set(M::Blackstone, "minecraft:blackstone");
    set(M::Basalt, "minecraft:basalt");
    set(M::SoulSoil, "minecraft:soul_soil");
    set(M::SoulSand, "minecraft:soul_sand");
    set(M::MagmaBlock, "minecraft:magma");
    p->stone     = {typeOf("minecraft:stone"), typeOf("minecraft:granite"), typeOf("minecraft:diorite"), typeOf("minecraft:andesite")};
    p->deepslate = {typeOf("minecraft:deepslate"), typeOf("minecraft:tuff")};
    p->nether    = {typeOf("minecraft:netherrack"), typeOf("minecraft:basalt"), typeOf("minecraft:blackstone")};
    p->complete  = true;
    for (auto const* b : p->mats) p->complete = p->complete && b;
    if (!p->complete) return nullptr; // the behavior pack isn't loaded (yet): try again next chunk
    gPalette = p;
    return gPalette;
}

template <size_t N>
bool contains(std::array<BlockType const*, N> const& a, BlockType const* t) {
    for (auto const* x : a)
        if (x == t) return true;
    return false;
}

} // namespace

void resetWorldGenCache() {
    std::lock_guard lock(gMutex);
    gPalette.reset();
    gDecorated = 0;
    gAnnounced = false;
}

long long decoratedChunks() { return gDecorated.load(); }

int decorateChunk(BlockSource& region, int dim, int chunkX, int chunkZ) {
    if (dim != 0 && dim != 1) return 0;
    if (config::get<MaterialsConfig>().disableWorldGen) return 0;
    auto pal = palette();
    if (!pal) return 0;

    // Features of a chunk may write into its 8 neighbours (Java's WorldGenRegion), when present.
    std::array<bool, 9> present{};
    for (int i = 0; i < 9; ++i) {
        int cx     = chunkX + i % 3 - 1, cz = chunkZ + i / 3 - 1;
        present[i] = region.hasChunksAt(BlockPos{cx * 16 + 8, 0, cz * 16 + 8}, 0, false);
    }
    BlockChangeContext ctx{};
    cm::World          w;
    w.minY     = region.getMinHeight();
    w.maxY     = region.getMaxHeight();
    w.canWrite = [&](int x, int, int z) {
        int dx = (x >> 4) - chunkX, dz = (z >> 4) - chunkZ;
        if (dx < -1 || dx > 1 || dz < -1 || dz > 1) return false;
        return present[(dz + 1) * 3 + dx + 1];
    };
    w.ground = [&](int x, int y, int z) {
        auto const* t = &region.getBlock(BlockPos{x, y, z}).getBlockType();
        if (contains(pal->stone, t)) return cm::Ground::Stone;
        if (contains(pal->deepslate, t)) return cm::Ground::Deepslate;
        if (contains(pal->nether, t)) return cm::Ground::Nether;
        return cm::Ground::Other;
    };
    w.set = [&](int x, int y, int z, cm::Mat m) {
        region.setBlock(BlockPos{x, y, z}, *pal->mats[static_cast<size_t>(m)], 2, nullptr, ctx);
    };
    w.surface = [&](int x, int z) {
        if (!w.canWrite(x, 0, z)) return w.maxY; // unknown column: treat as underground
        return static_cast<int>(region.getAboveTopSolidBlock(x, z, false, false));
    };
    auto seed   = static_cast<std::uint64_t>(region.getLevel().getSeed());
    int  placed = cm::decorateChunk(w, seed, dim, chunkX, chunkZ);
    ++gDecorated;
    if (!gAnnounced.exchange(true)) {
        lv::log().info("materials: world generation active (dimension {}, chunk {} {}, {} blocks)", dim, chunkX, chunkZ, placed);
    }
    return placed;
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsOverworldDecorateHook,
    ll::memory::HookPriority::Normal,
    OverworldGeneratorMultinoise,
    &OverworldGeneratorMultinoise::$decorateWorldGenPostProcess,
    void,
    Biome const&  biome,
    LevelChunk&   lc,
    BlockSource&  source,
    Random&       random
) {
    origin(biome, lc, source, random);
    ChunkPos const& cp = lc.mPosition;
    decorateChunk(source, 0, cp.x, cp.z);
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsNetherDecorateHook,
    ll::memory::HookPriority::Normal,
    NetherGenerator,
    &NetherGenerator::$decorateWorldGenPostProcess,
    void,
    Biome const&  biome,
    LevelChunk&   lc,
    BlockSource&  source,
    Random&       random
) {
    origin(biome, lc, source, random);
    ChunkPos const& cp = lc.mPosition;
    decorateChunk(source, 1, cp.x, cp.z);
}

void installWorldGenHooks() {
    MaterialsOverworldDecorateHook::hook();
    MaterialsNetherDecorateHook::hook();
}

void uninstallWorldGenHooks() {
    MaterialsNetherDecorateHook::unhook();
    MaterialsOverworldDecorateHook::unhook();
}

} // namespace lv::feature::materials
