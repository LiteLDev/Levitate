// Materials unit (U9): block table, config, drops, slabs, experience nuggets, Builder's Tea and
// the game hooks. World generation lives in MaterialsWorldGen.cpp, the legacy item entities
// (chromatic compound, shadow steel, refined radiance) in MaterialsItems.cpp.

#include "feature/materials/Materials.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/item/ExperienceOrb.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/VanillaItemTags.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/GameType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ResourceDropsContext.h"

#include "feature/common/Interactions.h"
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

BlockInfo const* blockInfo(Block const& block) { return infoOf(block.getBlockType()); }

std::string stringState(Block const& block, std::string const& key) { return strState(block, key); }

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
    (void)block;
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
    case Drop::Self: // the pack's loot tables (double slabs: their permutation's table drops two)
        out.emplace_back(std::string(info.id), 1);
        break;
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
    // Create: the amethyst break sound for the user (volume 0.5)
    if (auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound)) {
        auto& snd   = static_cast<PlaySoundPacket&>(*packet);
        auto  p     = player.getPosition();
        snd.mName   = "break.amethyst_block";
        snd.mPos    = BlockPos{static_cast<int>(p.x * 8), static_cast<int>(p.y * 8), static_cast<int>(p.z * 8)};
        snd.mVolume = 0.5f;
        snd.mPitch  = 1.0f;
        snd.sendTo(player);
    }
    // Create: each orb starts at eye + look / 2 + look x (the player's left, 0.75) and is thrown
    // along the look (look * 0.2 + (0, 0.2, 0) + a random unit offset * 0.1)
    Vec3   look = player.getViewVector();
    double yaw  = player.getRotation().y * 3.14159265358979 / 180.0;
    Vec3   side{static_cast<float>(-0.75 * std::cos(yaw)), 0.f, static_cast<float>(-0.75 * std::sin(yaw))};
    Vec3   cross{look.y * side.z - look.z * side.y, look.z * side.x - look.x * side.z, look.x * side.y - look.y * side.x};
    Vec3   pos = player.getEyePos() + look * 0.5f + cross;
    static thread_local std::mt19937 orbRng{std::random_device{}()};
    auto   orbsNear = [&] {
        std::vector<Actor*> out;
        AABB box{pos - Vec3{3, 3, 3}, pos + Vec3{3, 3, 3}};
        for (auto* e : region.getEntities(box, 0.0f))
            if (e && e->isType(ActorType::Experience)) out.push_back(e);
        return out;
    };
    auto before = orbsNear();
    int  total  = 0;
    for (int v : experienceOrbValues(used)) {
        ExperienceOrb::spawnOrbs(region, pos, v, ExperienceOrb::DropType::FromPlayer, &player);
        total += v;
    }
    for (auto* orb : orbsNear()) {
        if (std::find(before.begin(), before.end(), orb) != before.end()) continue;
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        Vec3  off{u(orbRng), u(orbRng), u(orbRng)};
        float len = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
        if (len > 1e-4f) off = off * (1.f / len);
        orb->mBuiltInComponents->mStateVectorComponent->mPosDelta = look * 0.2f + Vec3{0, 0.2f, 0} + off * 0.1f;
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

namespace ia = common::interactions;

namespace {

/// Create's SlabBlock.canBeReplaced: a single slab takes a second one of its kind into its empty half.
ia::Result slabUseOn(ia::UseOnContext const& c) {
    if (c.itemId.empty()) return ia::Result::Pass;
    auto const* itemBlock = game::blockWith(c.itemId);
    auto const* info      = itemBlock ? infoOf(itemBlock->getBlockType()) : nullptr;
    if (!info || info->shape != Shape::Slab) return ia::Result::Pass;

    auto&                   player  = c.player;
    auto&                   region  = player.getDimensionBlockSource();
    Block const&            clicked = c.block;
    BlockPos                at      = game::blockPos(c.pos);
    int                     face    = c.face;
    std::optional<BlockPos> mergeAt;
    if (&clicked.getBlockType() == &itemBlock->getBlockType()) {
        // the clicked slab: its top face (bottom slab), bottom face (top slab), or a side clicked in
        // the empty half
        if (auto top = singleSlabHalf(clicked)) {
            bool upper = c.hit[1] - std::floor(c.hit[1]) > 0.5;
            bool side  = face >= 2 && face < 6;
            if ((!*top && (face == 1 || (upper && side))) || (*top && (face == 0 || (!upper && side)))) mergeAt = at;
        }
    }
    if (!mergeAt && !game::replaceable(region, at)) {
        // placing into the block beside the clicked face: a single slab there always takes it
        static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
        if (face >= 0 && face < 6) {
            BlockPos     n{at.x + dx[face], at.y + dy[face], at.z + dz[face]};
            Block const& there = region.getBlock(n);
            if (&there.getBlockType() == &itemBlock->getBlockType() && singleSlabHalf(there)) mergeAt = n;
        }
    }
    if (mergeAt) {
        // vanilla placement rules: no adventure mode, nobody standing in the block
        AABB box{Vec3{float(mergeAt->x), float(mergeAt->y), float(mergeAt->z)}, Vec3{mergeAt->x + 1.0f, mergeAt->y + 1.0f, mergeAt->z + 1.0f}};
        if (player.getPlayerGameType() == GameType::Adventure || !region.isUnobstructedByEntities(box, nullptr)) mergeAt.reset();
    }
    if (!mergeAt || !mergeSlab(region, *mergeAt)) return ia::Result::Pass;
    if (!player.isCreative()) {
        auto& item = ia::mutableItem(c.item);
        item.set(item.mCount - 1);
        if (item.mCount <= 0) item = ItemStack::EMPTY_ITEM();
    }
    player.sendInventory(false);
    lv::log().info("materials: {} merged {} into a double slab", player.getRealName(), info->id);
    return ia::Result::Consume;
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

void installInteractions() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, slabUseOn));
    ids.push_back(ia::onUse(kExperienceNugget, [](ia::UseContext const& c) {
        useExperienceNuggets(c.player, ia::mutableItem(c.item), c.sneaking);
        c.player.sendInventory(false);
        return ia::Result::Consume;
    }));
}

void uninstallInteractions() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace

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
void installFurnaceXp();
void uninstallFurnaceXp();
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
        installInteractions();
        MaterialsCompleteUsingItemHook::hook();
        installMagicalItemHook();
        installWorldGenHooks();
        installConnections();
        installFurnaceXp();
    }
    void uninstall() {
        uninstallFurnaceXp();
        uninstallConnections();
        uninstallWorldGenHooks();
        uninstallMagicalItemHook();
        MaterialsCompleteUsingItemHook::unhook();
        uninstallInteractions();
        MaterialsSpawnResourcesHook::unhook();
    }
};

} // namespace lv::feature::materials

LV_REGISTER_MODULE(lv::feature::materials::MaterialsModule);
