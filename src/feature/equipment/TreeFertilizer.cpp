// Tree fertilizer (Create's TreeFertilizerItem): the tree grows at once, even without room.
//
// Create grows the tree in a "dream world" (TreesDreamWorld: everything is air except the soil
// below the sapling) and copies the blocks the tree placed into the real world: a solid block
// (Java: isRedstoneConductor, a log) replaces anything but unbreakable blocks, any other block
// (leaves, vines, ...) only replaces blocks without collision; replaced blocks drop as broken.
//
// Here the dream world is an IBlockWorldGenAPI that records what Bedrock's own tree feature places
// (the feature named by the sapling block: single / 2x2, mangrove, azalea), read from the sapling
// block and resolved through the level's FeatureRegistry. When the feature can't be resolved the
// old path runs: Bedrock's sapling growth (onFertilized) repeated until it grows (needs room).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "ll/api/service/Bedrock.h"

#include "mc/world/Pos.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/FertilizerType.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/IBlockWorldGenAPI.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/WorldGenContext.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/SaplingBlock.h"
#include "mc/world/level/levelgen/feature/gamerefs_feature/GameRefsFeature.h"
#include "mc/world/level/levelgen/feature/IFeature.h"
#include "mc/world/level/levelgen/feature/registry/FeatureRegistry.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/util/Random.h"
#include "mc/world/phys/AABB.h"

#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Log.h"

// The base class's chunk access is not exported (MCNAPI); the dream world overrides both, but the
// base vtable (used while constructing) still names them.
bool        IBlockWorldGenAPI::canGetChunk() const { return false; }
LevelChunk* IBlockWorldGenAPI::getChunk(ChunkPos const&) { return nullptr; }

namespace lv::feature::equipment {

namespace {

using Key = std::tuple<int, int, int>;

Key keyOf(BlockPos const& p) { return {p.x, p.y, p.z}; }

/// Create's TreesDreamWorld: air everywhere, the soil below `soilTop`; records what is placed.
class DreamWorld final : public IBlockWorldGenAPI {
public:
    DreamWorld(BlockSource& real, Block const& air, Block const& soil, int soilTop)
    : mReal(real), mAir(air), mSoil(soil), mSoilTop(soilTop) {
        std::memset(mContext, 0, sizeof(mContext));
    }

    std::map<Key, Block const*> placed;

    bool          canGetChunk() const override { return false; }
    LevelChunk*   getChunk(ChunkPos const&) override { return nullptr; }
    Block const*  tryGetLiquidBlock(BlockPos const&) const override { return nullptr; }
    Block const&  getBlock(BlockPos const& pos) const override { return at(pos); }
    Block const&  getBlockNoBoundsCheck(BlockPos const& pos) const override { return at(pos); }
    Block const&  getExtraBlock(BlockPos const&) const override { return mAir; }
    gsl::span<BlockDataFetchResult<Block> const> fetchBlocksInBox(BoundingBox const&, std::function<bool(Block const&)>) override {
        return {};
    }
    bool hasBiomeTag(uint64, BlockPos const&) const override { return false; }
    bool hasAnyBiomeTags(gsl::span<uint64 const>, BlockPos const&) const override { return false; }
    bool hasAllBiomeTags(gsl::span<uint64 const>, BlockPos const&) const override { return false; }
    bool setBlock(BlockPos const& pos, Block const& block, int) override { return put(pos, block); }
    bool setBlockSimple(BlockPos const& pos, Block const& block) override { return put(pos, block); }
    bool apply() const override { return true; }
    bool placeStructure(BlockPos const&, StructureTemplate&, StructureSettings&) override { return false; }
    bool mayPlace(BlockPos const& pos, Block const&) const override { return inHeight(pos.y); }
    bool mayPlacePlacementFilterComponent(BlockPos const& pos, Block const&) const override { return inHeight(pos.y); }
    bool canSurvive(BlockPos const& pos, Block const&) const override { return inHeight(pos.y); }
    bool canBeBuiltOver(BlockPos const& pos, Block const&) const override { return at(pos).isAir(); }
    short getMaxHeight() const override { return mReal.getMaxHeight(); }
    short getMinHeight() const override { return mReal.getMinHeight(); }
    bool  shimPlaceForOldFeatures(Feature const&, BlockPos const&, Random&) const override { return false; }
    short getHeightmap(int, int) override { return static_cast<short>(mSoilTop); }
    bool  isLegacyLevel() override { return false; }
    Biome const* getBiome(BlockPos const& pos) const override {
        BlockPos p = mReal.hasChunksAt(pos, 0, false) ? pos : BlockPos{pos.x, mSoilTop, pos.z};
        if (!mReal.hasChunksAt(p, 0, false)) return nullptr;
        return &mReal.getBiome(p);
    }
    bool  isInBounds(Pos const& pos) const override { return inHeight(pos.y); }
    short getLocalWaterLevel(BlockPos const&) const override { return 0; }
    LevelData const& getLevelData() const override { return ll::service::getLevel()->getLevelData(); }
    WorldGenContext const& getContext() override { return *reinterpret_cast<WorldGenContext const*>(mContext); }
    void disableBlockSimple() override {}

private:
    [[nodiscard]] bool inHeight(int y) const { return y >= mReal.getMinHeight() && y < mReal.getMaxHeight(); }

    [[nodiscard]] Block const& at(BlockPos const& pos) const {
        if (auto it = placed.find(keyOf(pos)); it != placed.end()) return *it->second;
        return pos.y < mSoilTop ? mSoil : mAir;
    }

    bool put(BlockPos const& pos, Block const& block) {
        if (!inHeight(pos.y)) return false;
        if (block.getTypeName() == "minecraft:podzol") return true; // Create's dream world ignores podzol
        placed[keyOf(pos)] = &block;
        return true;
    }

    BlockSource&    mReal;
    Block const&    mAir;
    Block const&    mSoil;
    int             mSoilTop;
    alignas(8) std::byte mContext[sizeof(WorldGenContext)];
};

/// IFeature::PlacementContext (the game's layout: target, pos, random, render params, logger).
struct PlacementContextLayout {
    IBlockWorldGenAPI* target;
    BlockPos           pos;
    Random*            random;
    RenderParams*      renderParams;
    void*              logger;
};
static_assert(sizeof(PlacementContextLayout) == sizeof(IFeature::PlacementContext));
static_assert(offsetof(PlacementContextLayout, pos) == 8 && offsetof(PlacementContextLayout, random) == 24);
static_assert(offsetof(PlacementContextLayout, renderParams) == 32 && offsetof(PlacementContextLayout, logger) == 40);

/// A feature of the level's registry by name (nullptr: unknown).
IFeature const* featureNamed(std::string const& name) {
    auto level = ll::service::getLevel();
    if (!level || name.empty()) return nullptr;
    auto&             registry = level->getFeatureRegistry();
    WeakRef<IFeature> ref      = registry.lookupByName(name);
    if (!ref.mRegistry->has_value()) return nullptr;
    auto const& features = *registry.mFeatureRegistry;
    auto        index    = static_cast<std::size_t>(ref.mIndex);
    if (index >= features.size()) return nullptr;
    return features[index].get();
}

/// The feature name in an optional<string> member of a sapling block, checked for sanity.
std::string featureName(std::optional<std::string> const& o) {
    auto const* raw     = reinterpret_cast<unsigned char const*>(&o);
    unsigned    engaged = raw[sizeof(std::string)];
    if (engaged > 1 || !engaged) return {};
    auto const& s = *o;
    if (s.size() > 128 || s.capacity() < s.size()) return {};
    return s.starts_with("minecraft:") ? s : std::string();
}

bool isDirtLike(std::string_view id) {
    for (auto d : {"minecraft:dirt", "minecraft:grass_block", "minecraft:podzol", "minecraft:mycelium", "minecraft:coarse_dirt",
                   "minecraft:rooted_dirt", "minecraft:dirt_with_roots", "minecraft:mud", "minecraft:muddy_mangrove_roots",
                   "minecraft:moss_block", "minecraft:pale_moss_block", "minecraft:grass"}) {
        if (id == d) return true;
    }
    return false;
}

constexpr std::array<std::string_view, 8> kSaplings = {
    "minecraft:oak_sapling",    "minecraft:spruce_sapling",   "minecraft:birch_sapling",  "minecraft:jungle_sapling",
    "minecraft:acacia_sapling", "minecraft:dark_oak_sapling", "minecraft:cherry_sapling", "minecraft:pale_oak_sapling",
};

/// What the sapling grows: the feature, where it is placed and the sapling cells it replaces.
struct Plan {
    std::string           feature;
    BlockPos              origin;
    std::vector<BlockPos> cells;
    bool                  resolved = false; ///< false: the sapling's features are unknown (fallback)
};

Plan planFor(BlockSource& region, Block const& block, BlockPos const& pos) {
    Plan        plan;
    std::string id = block.getTypeName();
    plan.origin    = pos;
    plan.cells     = {pos};
    if (id == "minecraft:mangrove_propagule") {
        // Java's TreeGrower.MANGROVE: the tall mangrove with a chance of 0.85
        plan.feature  = random01() < 0.85 ? "minecraft:tall_mangrove_tree_feature" : "minecraft:mangrove_tree_feature";
        plan.resolved = true;
        return plan;
    }
    if (id == "minecraft:azalea" || id == "minecraft:flowering_azalea") {
        plan.feature  = "minecraft:azalea_tree_feature";
        plan.resolved = true;
        return plan;
    }
    if (std::find(kSaplings.begin(), kSaplings.end(), id) == kSaplings.end()) return plan;
    // every listed sapling is a SaplingBlock: they share the oak sapling's vtable
    auto const* oak = game::blockWith("minecraft:oak_sapling");
    if (!oak) return plan;
    if (*reinterpret_cast<void* const*>(&block.getBlockType()) != *reinterpret_cast<void* const*>(&oak->getBlockType())) return plan;
    auto const& sapling = static_cast<SaplingBlock const&>(block.getBlockType());
    auto        single  = featureName(*sapling.mSingleSaplingTree);
    auto        quad    = featureName(*sapling.mQuadrupleSaplingTree);
    plan.resolved       = true;
    if (!quad.empty()) {
        if (auto offset = sapling._getQuadrupleSaplingOffset(region, pos)) {
            plan.feature = quad;
            plan.origin  = BlockPos{pos.x + offset->x, pos.y + offset->y, pos.z + offset->z};
            plan.cells   = {};
            for (int dx = 0; dx < 2; ++dx)
                for (int dz = 0; dz < 2; ++dz) plan.cells.push_back(BlockPos{plan.origin.x + dx, plan.origin.y, plan.origin.z + dz});
            return plan;
        }
    }
    plan.feature = single; // empty: needs a 2x2 (dark oak, pale oak): nothing grows
    return plan;
}

bool hasCollision(BlockSource& region, BlockPos const& pos, Block const& block) {
    if (block.isAir()) return false;
    AABB box = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
    return box.max.x - box.min.x > 1e-4f && box.max.y - box.min.y > 1e-4f && box.max.z - box.min.z > 1e-4f;
}

/// Grows the plan's feature in a dream world and copies it (Create's rules). Returns the number of
/// copied blocks, or nullopt when the feature could not be placed there.
std::optional<int> growInDream(BlockSource& region, Plan const& plan) {
    auto const* feature = featureNamed(plan.feature);
    if (!feature) return std::nullopt;
    auto const* air  = game::blockWith("minecraft:air");
    auto const* dirt = game::blockWith("minecraft:dirt");
    if (!air || !dirt) return std::nullopt;
    BlockPos below{plan.origin.x, plan.origin.y - 1, plan.origin.z};
    auto const& under = region.getBlock(below);
    // Create: dirt-like soil becomes dirt ("tree features don't seem to succeed with mud")
    Block const& soil = isDirtLike(under.getTypeName()) || under.isAir() ? *dirt : under;

    DreamWorld world(region, *air, soil, plan.origin.y);
    // the render parameters Molang conditions of composite features read (placement target, position)
    std::vector<std::byte> rpBytes(sizeof(RenderParams) + 64, std::byte{0});
    auto*                  rp = reinterpret_cast<RenderParams*>(rpBytes.data());
    rp->mBlockPlacementTarget = &world;
    rp->mBlockSource          = &region;
    rp->mLevel                = ll::service::getLevel().as_ptr();
    rp->mBlock                = &region.getBlock(plan.origin);
    std::construct_at(&rp->mBlockPos.get(), plan.origin);
    std::construct_at(&rp->mRandomFunction.get(), [] { return static_cast<float>(random01()); });
    // an empty variable map (two empty vectors) for the Molang the features evaluate
    alignas(8) std::byte varBytes[sizeof(MolangVariableMap)]{};
    auto*                vars = reinterpret_cast<MolangVariableMap*>(varBytes);
    rp->mVariables            = vars;
    struct Guard {
        RenderParams*      rp;
        MolangVariableMap* vars;
        ~Guard() {
            std::destroy_at(&rp->mRandomFunction.get());
            vars->~MolangVariableMap();
        }
    } guard{rp, vars};
    PlacementContextLayout ctx{&world, plan.origin, &Random::getThreadLocal(), rp, nullptr};
    auto result = feature->place(*reinterpret_cast<IFeature::PlacementContext const*>(&ctx));
    if (!result || world.placed.empty()) return std::nullopt;

    auto level = ll::service::getLevel();
    auto cell  = [&](BlockPos const& p) {
        return std::any_of(plan.cells.begin(), plan.cells.end(), [&](BlockPos const& c) { return c.x == p.x && c.y == p.y && c.z == p.z; });
    };
    int copied = 0;
    for (auto const& [k, block] : world.placed) {
        BlockPos p{std::get<0>(k), std::get<1>(k), std::get<2>(k)};
        if (p.y < region.getMinHeight() || p.y >= region.getMaxHeight() || !region.hasChunksAt(p, 0, false)) continue;
        auto const& target = region.getBlock(p);
        if (&target == block) continue;
        if (!target.isAir() && target.getBlockType().getDestroySpeed() < 0) continue; // don't replace bedrock
        // don't replace solid blocks with leaves
        if (!block->_isSolid() && hasCollision(region, p, target)) continue;
        // the grown saplings turn into the tree (no drop); anything else breaks as mined
        if (!target.isAir() && !cell(p) && level) level->destroyBlock(region, p, true, BlockChangeContext{});
        if (game::setBlock(region, p, *block)) ++copied;
    }
    return copied;
}

} // namespace

bool useTreeFertilizer(Player& player, ItemStack& held, GlobalPos const& pos) {
    std::string id = idAt(pos);
    bool sapling = id.ends_with("_sapling") || id == "minecraft:mangrove_propagule" || id == "minecraft:azalea" || id == "minecraft:flowering_azalea"
                || id == "minecraft:sapling";
    if (!sapling || id == "minecraft:bamboo_sapling") return false;
    if (id == "minecraft:mangrove_propagule" && stateAt(pos, "hanging").value_or(0)) return true; // Create: nothing on a hanging propagule
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    BlockPos     bp{pos.x, pos.y, pos.z};
    Block const& block = region->getBlock(bp);
    Plan         plan  = planFor(*region, block, bp);

    std::string how;
    if (plan.resolved && plan.feature.empty()) {
        how = "no tree: needs a 2x2";
    } else if (plan.resolved) {
        std::optional<int> copied;
        try {
            copied = growInDream(*region, plan);
        } catch (std::exception const& e) {
            log().error("equipment: tree fertilizer dream world failed: {}", e.what());
        }
        if (copied) {
            std::string const saplingId = id;
            for (auto const& c : plan.cells) // saplings the tree did not overwrite are used up as well
                if (game::blockIdAt(*region, c) == saplingId) game::clearBlock(*region, c);
            how = "grew: dream world " + plan.feature + ", " + std::to_string(*copied) + " blocks";
        }
    }
    if (how.empty()) {
        // fallback: Bedrock's own growth, repeated until the sapling turns into a tree (needs room)
        for (int i = 0; i < 64 && idAt(pos) == id; ++i) {
            auto const& b = region->getBlock(bp);
            b.getBlockType().onFertilized(*region, bp, &player, FertilizerType::Rapid);
        }
        how = std::string(idAt(pos) != id ? "grew" : "no room") + ": fallback, Bedrock's growth";
    }
    if (!player.isCreative()) {
        held.remove(1);
        player.sendInventory(false);
    }
    for (int i = 0; i < 12; ++i)
        particle(pos.dim, pos.x + random01(), pos.y + random01(), pos.z + random01(), "minecraft:crop_growth_emitter");
    log().info("equipment: {} used tree fertilizer on {} at {} {} {} ({})", player.getRealName(), id, pos.x, pos.y, pos.z, how);
    return true;
}

} // namespace lv::feature::equipment
