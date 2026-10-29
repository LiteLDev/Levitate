// Materials unit (U9): block table, config, drops, slabs, experience nuggets, Builder's Tea and
// the game hooks. World generation lives in MaterialsWorldGen.cpp, the legacy item entities
// (chromatic compound, shadow steel, refined radiance) in MaterialsItems.cpp.

#include "feature/materials/Materials.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/world/actor/item/ExperienceOrb.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/VanillaItemTags.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ResourceDropsContext.h"

#include "feature/common/Items.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("materials", lv::feature::materials::MaterialsConfig);

namespace lv::feature::materials {

// ---- block table ---------------------------------------------------------------------------------

std::vector<BlockInfo> const& allBlocks() {
    static std::vector<BlockInfo> const table = [] {
        std::vector<BlockInfo> v;
#define LV_MATERIALS_BLOCK(id, tool, tier, shape, drop) v.push_back({id, tool, tier, shape, drop});
#include "feature/materials/MaterialsBlocks.inc"
#undef LV_MATERIALS_BLOCK
        return v;
    }();
    return table;
}

BlockInfo const* blockInfo(std::string_view id) {
    static std::unordered_map<std::string_view, BlockInfo const*> const byId = [] {
        std::unordered_map<std::string_view, BlockInfo const*> m;
        for (auto const& b : allBlocks()) m[b.id] = &b;
        return m;
    }();
    auto it = byId.find(id);
    return it == byId.end() ? nullptr : it->second;
}

namespace {

/// Block types of the unit, resolved when the module installs (the hooks run on every block).
std::unordered_map<BlockType const*, BlockInfo const*>& typeTable() {
    static std::unordered_map<BlockType const*, BlockInfo const*> table;
    return table;
}

BlockInfo const* infoOf(BlockType const& type) {
    auto const& t  = typeTable();
    auto        it = t.find(&type);
    return it == t.end() ? nullptr : it->second;
}

/// String / bool states of a block (from its serialisation NBT).
std::string strState(Block const& block, std::string const& key) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(key)) return {};
    auto const& v = nbt["states"][key];
    if (v.hold(Tag::Type::String)) return std::string(v.get<StringTag>());
    if (v.hold(Tag::Type::Byte)) return std::to_string(int(v.get<ByteTag>().data));
    if (v.hold(Tag::Type::Int)) return std::to_string(v.get<IntTag>().data);
    return {};
}

Block const* withByteState(Block const& block, std::string const& key, uchar value) {
    CompoundTag nbt = *block.mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(key)) return nullptr;
    nbt["states"][key] = value;
    auto result        = Block::tryGetFromRegistry(nbt);
    return result ? result.as_ptr() : nullptr;
}

} // namespace

std::string itemIdOf(ItemStack const& stack) { return stack.isNull() ? std::string{} : stack.getTypeName(); }

ItemStack makeStack(std::string const& id, int count) {
    auto s = common::items::make(id, count);
    return s ? *s : ItemStack{};
}

Block const* defaultBlock(std::string const& id) { return game::blockWith(id); }

// ---- drops ---------------------------------------------------------------------------------------

bool toolCanHarvest(BlockInfo const& info, ItemStack const& tool) {
    if (info.tool == Tool::Axe) return true; // cardboard: axeOnly, but no requiresCorrectToolForDrops
    if (tool.isNull() || !tool.hasTag(VanillaItemTags::Pickaxe())) return false;
    if (info.tier == Tier::Iron) {
        return tool.hasTag(VanillaItemTags::IronTier()) || tool.hasTag(VanillaItemTags::DiamondTier())
            || tool.hasTag(VanillaItemTags::NetheriteTier());
    }
    return true;
}

std::vector<std::pair<std::string, int>>
dropsFor(BlockInfo const& info, Block const& block, bool silkTouch, int fortune, std::function<int(int)> const& nextInt) {
    std::vector<std::pair<std::string, int>> out;
    switch (info.drop) {
    case Drop::ZincOre:
        if (silkTouch) {
            out.emplace_back(std::string(info.id), 1);
        } else {
            // ApplyBonusCount.addOreBonusCount: count * (max(0, rand(fortune + 2) - 1) + 1)
            int bonus = fortune > 0 ? std::max(0, nextInt(fortune + 2) - 1) : 0;
            out.emplace_back("levitate:raw_zinc", bonus + 1);
        }
        break;
    case Drop::BoundCardboard:
        if (silkTouch) {
            out.emplace_back(std::string(info.id), 1);
        } else {
            out.emplace_back("minecraft:string", 1);
            out.emplace_back("levitate:cardboard_block", 1);
        }
        break;
    case Drop::Self: {
        int n = 1;
        if (info.shape == Shape::Slab && strState(block, "levitate:double") == "1") n = 2;
        out.emplace_back(std::string(info.id), n);
        break;
    }
    }
    return out;
}

// ---- slabs ---------------------------------------------------------------------------------------

std::optional<bool> singleSlabHalf(Block const& block) {
    auto const* info = infoOf(block.getBlockType());
    if (!info || info->shape != Shape::Slab) return std::nullopt;
    if (strState(block, "levitate:double") == "1") return std::nullopt;
    return strState(block, "minecraft:vertical_half") == "top";
}

bool mergeSlab(BlockSource& region, BlockPos const& pos) {
    auto const& block = region.getBlock(pos);
    if (!singleSlabHalf(block)) return false;
    auto const* merged = withByteState(block, "levitate:double", 1);
    return merged && game::setBlock(region, pos, *merged);
}

// ---- items ---------------------------------------------------------------------------------------

std::vector<int> experienceOrbValues(int amount) {
    std::vector<int> out;
    if (amount <= 0) return out;
    int total    = static_cast<int>(std::ceil(3.0f * amount));
    int maxOrbs  = amount == 1 ? 1 : 5;
    int valuePer = std::max(1, 1 + total / maxOrbs);
    for (int i = 0; i < maxOrbs; ++i) {
        int value = std::min(valuePer, total - i * valuePer);
        if (value <= 0) continue;
        out.push_back(value);
    }
    return out;
}

int useExperienceNuggets(Player& player, ItemStack& stack, bool sneaking) {
    int count = stack.isNull() ? 0 : static_cast<int>(stack.mCount);
    if (count <= 0) return 0;
    int  used  = sneaking ? 1 : count;
    auto& region = player.getDimensionBlockSource();
    Vec3 look   = player.getViewVector();
    Vec3 pos    = player.getPosition() + look * 0.5f;
    int  total  = 0;
    for (int v : experienceOrbValues(used)) {
        ExperienceOrb::spawnOrbs(region, pos, v, ExperienceOrb::DropType::FromPlayer, &player);
        total += v;
    }
    stack.set(count - used);
    if (count - used <= 0) stack = ItemStack::EMPTY_ITEM();
    lv::log().info("materials: {} used {} experience nugget(s) for {} xp", player.getRealName(), used, total);
    return total;
}

void applyBuildersTea(Player& player) {
    EffectDuration duration{};
    duration.mValue = 3 * 60 * 20;
    MobEffectInstance effect(3 /* haste */, duration);
    effect.mAmplifier       = 0;
    effect.mAmbient         = false;
    effect.mEffectVisible   = false;
    effect.mDisplayOnScreenTextureAnimation = false;
    player.addEffect(effect);
}

// ---- hooks ---------------------------------------------------------------------------------------

namespace {

std::string const kExperienceNugget = "levitate:experience_nugget";
std::string const kBuildersTea      = "levitate:builders_tea";

double unit(std::mt19937& rng) { return std::uniform_real_distribution<double>(0, 1)(rng); }

} // namespace

LL_TYPE_INSTANCE_HOOK(
    MaterialsSpawnResourcesHook,
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
    auto const* info = infoOf(*this);
    if (!info || ctx.mCause != ResourceDropsCause::PlayerMining) return origin(region, pos, block, random, ctx, actor);
    ItemStack const& tool = ctx.mUsedItem;
    if (!toolCanHarvest(*info, tool)) return; // Create: requiresCorrectToolForDrops
    if (info->drop == Drop::Self) return origin(region, pos, block, random, ctx, actor);
    static thread_local std::mt19937 rng{std::random_device{}()};
    bool silk    = ctx.isUsingSilkTouch();
    int  fortune = tool.isNull() ? 0 : EnchantUtils::getEnchantLevel(Enchant::Type::Fortune, tool);
    auto drops   = dropsFor(*info, block, silk, fortune, [](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); });
    Vec3 at{pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f};
    for (auto const& [id, count] : drops) {
        auto stack = makeStack(id, count);
        if (stack.isNull()) continue;
        Vec3 jitter{static_cast<float>(unit(rng) * 0.5 - 0.25), static_cast<float>(unit(rng) * 0.5 - 0.25), static_cast<float>(unit(rng) * 0.5 - 0.25)};
        region.getLevel().getSpawner().spawnItem(region, stack, nullptr, at + jitter, 10);
    }
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsUseItemOnHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItemOn,
    InteractionResult,
    ItemStack&      item,
    BlockPos const& at,
    uchar           face,
    Vec3 const&     hit,
    HandSlot        handSlot,
    Block const*    targetBlock,
    bool            isFirstEvent
) {
    Player& player = mPlayer;
    if (player.isClientSide() || item.isNull()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto const* itemBlock = game::blockWith(item.getTypeName());
    auto const* info      = itemBlock ? infoOf(itemBlock->getBlockType()) : nullptr;
    if (!info || info->shape != Shape::Slab) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);

    // Create's SlabBlock.canBeReplaced: a single slab takes a second one of its kind into its empty half.
    auto&        region  = player.getDimensionBlockSource();
    Block const& clicked = targetBlock ? *targetBlock : region.getBlock(at);
    std::optional<BlockPos> mergeAt;
    if (&clicked.getBlockType() == &itemBlock->getBlockType()) {
        if (auto top = singleSlabHalf(clicked); top && ((face == 1 && !*top) || (face == 0 && *top))) mergeAt = at;
    }
    if (!mergeAt) {
        static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
        if (face < 6) {
            BlockPos     n{at.x + dx[face], at.y + dy[face], at.z + dz[face]};
            Block const& there = region.getBlock(n);
            if (&there.getBlockType() == &itemBlock->getBlockType()) {
                if (auto top = singleSlabHalf(there)) {
                    float frac = hit.y - std::floor(hit.y);
                    if (face == 1) frac = 0.0f; // clicked the top of the block below: lower half
                    if (face == 0) frac = 1.0f;
                    if ((*top && frac < 0.5f) || (!*top && frac >= 0.5f)) mergeAt = n;
                }
            }
        }
    }
    if (!mergeAt || !mergeSlab(region, *mergeAt)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    if (!player.isCreative()) {
        item.set(item.mCount - 1);
        if (item.mCount <= 0) item = ItemStack::EMPTY_ITEM();
    }
    player.sendInventory(false);
    lv::log().info("materials: {} merged {} into a double slab", player.getRealName(), info->id);
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsUseItemHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItem,
    bool,
    ItemStack& item,
    HandSlot   handSlot
) {
    Player& player = mPlayer;
    if (!player.isClientSide() && !item.isNull() && item.getTypeName() == kExperienceNugget) {
        useExperienceNuggets(player, item, player.isSneaking());
        player.sendInventory(false);
        return true;
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(MaterialsCompleteUsingItemHook, ll::memory::HookPriority::Normal, Player, &Player::completeUsingItem, void) {
    bool tea = !isClientSide() && getSelectedItem().getTypeName() == kBuildersTea;
    origin();
    if (tea) {
        applyBuildersTea(*this);
        lv::log().info("materials: {} drank builder's tea", getRealName());
    }
}

void installMagicalItemHook();
void uninstallMagicalItemHook();
void installWorldGenHooks();
void uninstallWorldGenHooks();

void resetWorldGenCache();
void resetItemCache();

/// Resolves the unit's block types (the client registers pack blocks per world).
void rebuildTypeTable() {
    auto& t = typeTable();
    t.clear();
    int missing = 0;
    for (auto const& b : allBlocks()) {
        auto type = BlockType::tryGetFromRegistry(HashedString(std::string(b.id)));
        if (type) t[type.as_ptr()] = &b;
        else ++missing;
    }
    resetWorldGenCache();
    if (t.empty()) return; // no world yet (client at game start)
    if (missing) lv::log().warn("materials: {} block(s) of the behavior pack are not loaded", missing);
    lv::log().info("materials: {} blocks ready", t.size());
}

struct MaterialsModule {
    void install() {
        rebuildTypeTable();
        game::db::onWorldReady([] { rebuildTypeTable(); });
        game::db::onWorldClosed([] {
            typeTable().clear();
            resetWorldGenCache();
            resetItemCache();
        });
        MaterialsSpawnResourcesHook::hook();
        MaterialsUseItemOnHook::hook();
        MaterialsUseItemHook::hook();
        MaterialsCompleteUsingItemHook::hook();
        installMagicalItemHook();
        installWorldGenHooks();
    }
    void uninstall() {
        uninstallWorldGenHooks();
        uninstallMagicalItemHook();
        MaterialsCompleteUsingItemHook::unhook();
        MaterialsUseItemHook::unhook();
        MaterialsUseItemOnHook::unhook();
        MaterialsSpawnResourcesHook::unhook();
    }
};

} // namespace lv::feature::materials

LV_REGISTER_MODULE(lv::feature::materials::MaterialsModule);
