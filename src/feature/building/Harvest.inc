// Building unit (U59): which tool a building block needs to drop (Create's requiresCorrectToolForDrops,
// inherited from the block it copies its properties from) and the copycats' material rules: a
// copycat is harvested and mined like its material (CopycatBlock#canHarvestBlock /
// #getDestroyProgress).
//
// requiresCorrectToolForDrops in Create:
// - casings (SharedProperties.stone, axe or pickaxe), the railway casing too;
// - bars (iron bars), metal table cloths, sliding doors, trapdoors, girders, the placard (pickaxe);
// - copper shingles / tiles and their stairs and slabs (copper block; pickaxe, #needs_stone_tool);
// - copycats: the material's rule, an empty one is copycat_base (soft metal; axe or pickaxe).
// Glass, windows, ladders, scaffolding, brackets and dyed table cloths drop with any tool.

#include <cstddef>
#include <cstring>
#include <map>

#include "ll/api/memory/Hook.h"

#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/provider/player_destroy/DestroyProgressContext.h"
#include "mc/world/actor/provider/player_destroy/PlayerDestroy.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/VanillaItemTags.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ResourceDropsContext.h"

#include "feature/building/Building.h"
#include "feature/common/Interactions.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::building {

namespace {
namespace ia = common::interactions;

bool isCopycat(Info const& i) { return i.kind == Kind::CopycatPanel || i.kind == Kind::CopycatStep; }

bool isPickaxe(ItemStack const& t) { return !t.isNull() && t.hasTag(VanillaItemTags::Pickaxe()); }
bool isAxe(ItemStack const& t) { return !t.isNull() && t.hasTag(VanillaItemTags::Hatchet()); }

/// The copycat materials of the blocks players are breaking this tick (the material is gone from
/// BlockData once the drops are spawned).
std::map<GlobalPos, Block const*>& breakingMaterials() {
    static std::map<GlobalPos, Block const*> m;
    return m;
}

/// Java's Block#canHarvestBlock for a vanilla material: no correct tool needed, or the tool can
/// destroy it specially (Bedrock's tier check).
bool materialHarvests(Block const& material, ItemStack const& tool) {
    if (!material.getBlockType().mRequiresCorrectToolForDrops) return true;
    return !tool.isNull() && tool.mItem && tool.mItem->canDestroySpecial(material);
}

} // namespace

HarvestNeed harvestNeedOf(Info const& i) {
    switch (i.kind) {
    case Kind::Casing:
    case Kind::CopycatPanel:
    case Kind::CopycatStep: return HarvestNeed::AxeOrPickaxe;
    case Kind::Bars:
    case Kind::Door:
    case Kind::Trapdoor:
    case Kind::Girder:
    case Kind::GirderShaft:
    case Kind::Placard: return HarvestNeed::Pickaxe;
    case Kind::CopperBlock:
    case Kind::CopperStairs:
    case Kind::CopperSlab: return HarvestNeed::StonePickaxe;
    case Kind::TableCloth: {
        bool metal = i.id == "levitate:andesite_table_cloth" || i.id == "levitate:brass_table_cloth" || i.id == "levitate:copper_table_cloth";
        return metal ? HarvestNeed::Pickaxe : HarvestNeed::None;
    }
    default: return HarvestNeed::None;
    }
}

bool toolHarvests(HarvestNeed need, ItemStack const& tool) {
    switch (need) {
    case HarvestNeed::None: return true;
    case HarvestNeed::Pickaxe: return isPickaxe(tool);
    case HarvestNeed::AxeOrPickaxe: return isPickaxe(tool) || isAxe(tool);
    case HarvestNeed::StonePickaxe:
        return isPickaxe(tool)
            && (tool.hasTag(VanillaItemTags::StoneTier()) || tool.hasTag(VanillaItemTags::IronTier()) || tool.hasTag(VanillaItemTags::DiamondTier())
                || tool.hasTag(VanillaItemTags::NetheriteTier()) || tool.hasTag(VanillaItemTags::CopperTier()));
    }
    return true;
}

std::function<Block const*(BlockPos const&)>& clientCopycatMaterial() {
    static std::function<Block const*(BlockPos const&)> fn;
    return fn;
}

bool canHarvest(Info const& info, Block const* copycatMaterial, ItemStack const& tool) {
    if (isCopycat(info) && copycatMaterial) return materialHarvests(*copycatMaterial, tool);
    return toolHarvests(harvestNeedOf(info), tool);
}

} // namespace lv::feature::building

namespace bld = lv::feature::building;

LL_TYPE_INSTANCE_HOOK(
    BuildingHarvestHook,
    ll::memory::HookPriority::Normal,
    BlockType,
    &BlockType::spawnResources,
    void,
    BlockSource&                region,
    BlockPos const&             pos,
    Block const&                block,
    IRandom&                    random,
    ResourceDropsContext const& ctx,
    Actor const*                actor
) {
    auto const* info = bld::infoOf(block);
    if (!info || ctx.mCause != ResourceDropsCause::PlayerMining) return origin(region, pos, block, random, ctx, actor);
    auto        at       = lv::game::globalPos(region, pos);
    Block const* material = nullptr;
    if (auto it = bld::breakingMaterials().find(at); it != bld::breakingMaterials().end()) material = it->second;
    if (!bld::canHarvest(*info, material, ctx.mUsedItem)) return; // Create: requiresCorrectToolForDrops
    origin(region, pos, block, random, ctx, actor);
}

namespace {

/// Create's CopycatBlock#getDestroyProgress: a copycat is mined as fast as its material. The
/// progress of a copycat the player is mining (GameMode's destroy position) is the material's.
/// Only for players with the client mod (it mines at the same speed): a vanilla client predicts
/// the copycat's own speed.
Block const* copycatMaterialMined(PlayerDestroy::DestroyProgressContext const& ctx) {
    auto const& block = static_cast<Block const&>(ctx.block);
    auto const* info  = bld::infoOf(block);
    if (!info || (info->kind != bld::Kind::CopycatPanel && info->kind != bld::Kind::CopycatStep)) return nullptr;
    auto* actor = Actor::tryGetFromEntity(const_cast<EntityContext&>(static_cast<EntityContext const&>(ctx.provider)), false);
    if (!actor || !actor->isPlayer()) return nullptr;
    auto& player = static_cast<Player&>(*actor);
    auto* gameMode = player.mGameMode.get();
    if (!gameMode) return nullptr;
    BlockPos p = gameMode->mDestroyBlockPos;
    if (player.isClientSide()) {
        // the client mod's own player: the material the server sent for drawing
        auto const& lookup = bld::clientCopycatMaterial();
        if (!lookup || &player.getDimensionBlockSource().getBlock(p) != &block) return nullptr;
        return lookup(p);
    }
    if (!lv::feature::net::isCompanion(player)) return nullptr;
    auto at = lv::game::globalPos(static_cast<int>(player.getDimensionId()), p);
    auto const* here = bld::blockAt(at);
    if (here != &block) return nullptr;
    return bld::copycatMaterialBlock(at);
}

float withMaterial(PlayerDestroy::DestroyProgressContext& ctx, Block const& material, float (*fn)(PlayerDestroy::DestroyProgressContext&)) {
    // the context holds the block by reference: the same context with the material in its place
    alignas(PlayerDestroy::DestroyProgressContext) std::byte copy[sizeof(PlayerDestroy::DestroyProgressContext)];
    std::memcpy(copy, &ctx, sizeof(copy));
    auto offset = reinterpret_cast<std::byte const*>(&ctx.block) - reinterpret_cast<std::byte const*>(&ctx);
    Block const* m = &material;
    std::memcpy(copy + offset, &m, sizeof(m));
    return fn(*reinterpret_cast<PlayerDestroy::DestroyProgressContext*>(copy));
}

} // namespace

LL_STATIC_HOOK(BuildingCopycatSpeedHook, ll::memory::HookPriority::Normal, &PlayerDestroy::getDestroySpeed, float, PlayerDestroy::DestroyProgressContext& ctx) {
    if (auto const* m = copycatMaterialMined(ctx)) return withMaterial(ctx, *m, [](PlayerDestroy::DestroyProgressContext& c) { return origin(c); });
    return origin(ctx);
}

LL_STATIC_HOOK(BuildingCopycatProgressHook, ll::memory::HookPriority::Normal, &PlayerDestroy::getDestroyProgress, float, PlayerDestroy::DestroyProgressContext& ctx) {
    if (auto const* m = copycatMaterialMined(ctx)) return withMaterial(ctx, *m, [](PlayerDestroy::DestroyProgressContext& c) { return origin(c); });
    return origin(ctx);
}

namespace lv::feature::building {
namespace {

class HarvestModule {
public:
    void install() {
        BuildingHarvestHook::hook();
        BuildingCopycatSpeedHook::hook();
        BuildingCopycatProgressHook::hook();
        for (auto const* id : {"levitate:copycat_panel", "levitate:copycat_step"}) {
            mIds.push_back(ia::onDestroy({std::string(id)}, [](ia::DestroyContext const& c) {
                breakingMaterials()[c.pos] = copycatMaterialBlock(c.pos);
                if (breakingMaterials().size() > 64) breakingMaterials().clear();
                return ia::Result::Pass;
            }));
            mIds.push_back(ia::onDestroyed({std::string(id)}, [](ia::DestroyContext const& c, bool) { breakingMaterials().erase(c.pos); }));
        }
    }
    void uninstall() {
        for (auto id : mIds) ia::remove(id);
        mIds.clear();
        BuildingCopycatProgressHook::unhook();
        BuildingCopycatSpeedHook::unhook();
        BuildingHarvestHook::unhook();
    }

private:
    std::vector<ia::HandlerId> mIds;
};

} // namespace
} // namespace lv::feature::building

LV_REGISTER_MODULE(lv::feature::building::HarvestModule);
