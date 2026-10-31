// The wrench (U8): Create's WrenchItem + IWrenchable on Bedrock.
//
// The wrench is handled through the shared dispatch (common/Interactions.h; network players and
// SimulatedPlayers alike) at Fallback priority: use-on any block, interact / attack on entities.

#include "feature/goggles/Wrenchable.h"

#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ResourceDrops.h"
#include "mc/world/level/block/ResourceDropsContext.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/deps/nbt/CompoundTag.h"

#include "core/goggles/Rotation.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/tracks/Tracks.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::goggles::wrench {

namespace {

std::unordered_map<std::string, Handler>& registry() {
    static std::unordered_map<std::string, Handler> m;
    return m;
}

std::vector<EntityHandler>& entityHandlers() {
    static std::vector<EntityHandler> v;
    return v;
}

float randomFloat() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(0.f, 1.f)(rng);
}

void playSound(core::GlobalPos const& pos, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(game::blockPos(pos), DimensionType(pos.dim));
}


/// Create's IWrenchable.onSneakWrenched: `Block.getDrops(state, level, pos, be, player, wrench)`,
/// the block's loot table rolled as if mined with the wrench (silk touch / fortune of the wrench
/// apply, the correct-tool rule does not, as in Java).
std::vector<ItemStack> lootOf(BlockSource& region, BlockPos const& pos, Block const& block, ItemStack const& tool) {
    std::vector<ItemStack> out;
    auto                   level = ll::service::getLevel();
    if (!level) return out;
    ResourceDropsContext ctx{ResourceDropsCause::PlayerMining, 0.f, tool, pos, region.getDimensionId(), region};
    auto                 drops = block.getBlockType().getResourceDrops(block, level->getIRandom(), ctx);
    for (auto& s : drops.mItems.get())
        if (!s.isNull() && s.mCount > 0) out.push_back(s);
    return out;
}

/// Create's placeItemBackInInventory: into the inventory, the rest dropped at the player.
void giveBack(Player& player, ItemStack stack) {
    if (stack.isNull() || stack.mCount == 0) return;
    if (!player.add(stack) && stack.mCount > 0) player.drop(stack, false);
    player.sendInventory(false);
}

/// Contraptions whose anchor is the block at `pos` are disassembled before it goes (Create: the
/// anchor's block entity disassembles when it is removed).
void disassembleAnchoredAt(core::GlobalPos const& pos) {
    for (auto id : contraption::all()) {
        auto* c = contraption::find(id);
        if (c && c->dimension() == pos.dim && c->anchor() == pos) contraption::disassemble(id);
    }
}

core::common::Vec localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}


// Create's track (TrackBlock): wrenching does nothing, sneak-wrenching gives the curves' tracks
// to the player (survival) before the default pick-up.
Handler trackHandler() {
    Handler h;
    h.onWrenched      = [](Context&) { return Result::Success; };
    h.onSneakWrenched = [](Context& c) {
        if (!c.player.isCreative() && game::intState(c.block, "levitate:turn").value_or(0) != 0) tracks::dismantleCurves(c.player, c.pos);
        return defaultOnSneakWrenched(c);
    };
    return h;
}

// Create's IWrenchable blocks of the wave-1 units (referenced by id).
LV_REGISTER_WRENCHABLE("levitate:linear_chassis");
LV_REGISTER_WRENCHABLE("levitate:secondary_linear_chassis");
LV_REGISTER_WRENCHABLE("levitate:radial_chassis");
LV_REGISTER_WRENCHABLE("levitate:sticker");
LV_REGISTER_WRENCHABLE("levitate:track", trackHandler());

} // namespace

// ---- registry ------------------------------------------------------------------------------------

void registerBlock(std::string blockId, Handler handler) { registry()[std::move(blockId)] = std::move(handler); }

Handler const* handlerOf(std::string_view blockId) {
    if (auto it = registry().find(std::string(blockId)); it != registry().end()) return &it->second;
    static Handler const kinetic{};
    if (kinetics::specOf(blockId)) return &kinetic; // every Create IRotate block is IWrenchable
    return nullptr;
}

bool isWrenchable(std::string_view blockId) { return handlerOf(blockId) != nullptr; }

void registerEntityHandler(EntityHandler handler) { entityHandlers().push_back(std::move(handler)); }

bool canWrenchPickup(std::string_view id) {
    static std::unordered_set<std::string_view> const exact{
        "minecraft:redstone_wire",
        "minecraft:redstone_torch",
        "minecraft:unlit_redstone_torch",
        "minecraft:unpowered_repeater",
        "minecraft:powered_repeater",
        "minecraft:lever",
        "minecraft:unpowered_comparator",
        "minecraft:powered_comparator",
        "minecraft:observer",
        "minecraft:piston",
        "minecraft:sticky_piston",
        "minecraft:trip_wire",
        "minecraft:tripwire_hook",
        "minecraft:daylight_detector",
        "minecraft:daylight_detector_inverted",
        "minecraft:target",
        "minecraft:hopper",
        "minecraft:rail",
        "minecraft:golden_rail",
        "minecraft:detector_rail",
        "minecraft:activator_rail",
        "minecraft:redstone_lamp",
        "minecraft:lit_redstone_lamp",
        // Create's own: metal bars (MetalBarsGen) and the palette iron blocks (palettesIronBlock)
        "levitate:andesite_bars",
        "levitate:brass_bars",
        "levitate:copper_bars",
        "levitate:industrial_iron_block",
        "levitate:weathered_iron_block",
    };
    if (exact.contains(id)) return true;
    // #minecraft:buttons, #minecraft:pressure_plates
    if (id.starts_with("minecraft:") && (id.ends_with("_button") || id.ends_with("_pressure_plate"))) return true;
    return false;
}

// ---- defaults ------------------------------------------------------------------------------------

Block const& rotatedBlock(Block const& block, int face) {
    core::goggles::States states;
    for (auto const* name : core::goggles::kRotationStates) {
        if (auto v = game::intState(block, name)) states[name] = *v;
    }
    if (states.empty()) return block;
    auto         rotated = core::goggles::rotatedStates(states, face);
    Block const* out     = &block;
    for (auto const& [name, value] : rotated) {
        if (states[name] == value) continue;
        auto const* def = out->getBlockType().getBlockState(HashedString(name));
        if (!def) continue;
        if (auto b = out->setState(*def, value)) out = b.as_ptr();
    }
    return *out;
}

void playRotateSound(core::GlobalPos const& pos) { playSound(pos, "levitate.wrench_rotate", 1.f, randomFloat() + .5f); }

void playRemoveSound(core::GlobalPos const& pos) {
    float pitch = randomFloat() * .5f + .5f;
    playSound(pos, "levitate.wrench_remove", 1.f, pitch);
    playSound(pos, "levitate.wrench_remove_hit", 1.f, pitch);
}

Result defaultOnWrenched(Context& ctx) {
    auto const*  handler = handlerOf(ctx.block.getTypeName());
    Block const* rotated = handler && handler->rotate ? handler->rotate(ctx.block, ctx.face) : &rotatedBlock(ctx.block, ctx.face);
    if (!rotated || rotated == &ctx.block) return Result::Success;
    game::setBlock(ctx.region, game::blockPos(ctx.pos), *rotated);
    if (&ctx.region.getBlock(game::blockPos(ctx.pos)) != &ctx.block) {
        playRotateSound(ctx.pos);
        log().info("wrench: {} rotated {} at {} (face {})", ctx.player.getRealName(), ctx.block.getTypeName(), ctx.pos.key(), ctx.face);
    }
    return Result::Success;
}

Result defaultOnSneakWrenched(Context& ctx) {
    auto bp = game::blockPos(ctx.pos);
    auto id = ctx.block.getTypeName();
    // the drops are rolled before the block goes
    auto drops = ctx.player.isCreative() ? std::vector<ItemStack>{} : lootOf(ctx.region, bp, ctx.block, ctx.player.getSelectedItem());
    disassembleAnchoredAt(ctx.pos);
    bool removed = false;
    if (auto level = ll::service::getLevel()) removed = level->destroyBlock(ctx.region, bp, false, BlockChangeContext{});
    if (!removed) removed = game::clearBlock(ctx.region, bp);
    if (!removed || &ctx.region.getBlock(bp) == &ctx.block) return Result::Success; // protected: nothing given
    for (auto& d : drops) giveBack(ctx.player, d);
    playRemoveSound(ctx.pos);
    log().info("wrench: {} dismantled {} at {}", ctx.player.getRealName(), id, ctx.pos.key());
    return Result::Success;
}

// ---- hooks ---------------------------------------------------------------------------------------

namespace {

/// Bedrock repeats use-on packets while the button is held: one action per click (Java's item
/// use delay of 4 ticks).
bool debounced(Player& player, core::GlobalPos const& pos) {
    static std::unordered_map<std::int64_t, std::pair<core::GlobalPos, std::uint64_t>> last;
    auto  now  = player.getLevel().getCurrentServerTick().tickID;
    auto& slot = last[player.getOrCreateUniqueID().rawID];
    bool  skip = slot.first == pos && now >= slot.second && now - slot.second < 4;
    slot       = {pos, now};
    return skip;
}

/// Handles a wrench click on a block; nullopt: let the game handle it.
std::optional<Result> onWrenchUse(Player& player, BlockPos const& at, int face, Vec3 const& click) {
    if (!player.canUseAbility(AbilitiesIndex::Build)) return std::nullopt; // Create: player.mayBuild()
    auto&        region   = player.getDimensionBlockSource();
    Block const& block    = region.getBlock(at);
    auto         id       = block.getTypeName();
    bool         sneaking = player.isSneaking();
    auto         hit      = localHit(at, click);
    // value boxes and filter slots take plain clicks (U3)
    if (!sneaking
        && (common::valuebox::boxAt(block, face, hit) || common::filterslot::slotAt(block, face, hit))) {
        return std::nullopt;
    }
    auto const* handler = handlerOf(id);
    auto        pos     = game::globalPos(region, at);
    if (!handler) {
        if (!sneaking || !canWrenchPickup(id)) return std::nullopt;
        if (debounced(player, pos)) return Result::Success;
        Context ctx{player, region, pos, block, face, hit[0], hit[1], hit[2]};
        return defaultOnSneakWrenched(ctx); // Create's WrenchItem.onItemUseOnOther
    }
    if (debounced(player, pos)) return Result::Success;
    Context ctx{player, region, pos, block, face, hit[0], hit[1], hit[2]};
    if (sneaking) return handler->onSneakWrenched ? handler->onSneakWrenched(ctx) : defaultOnSneakWrenched(ctx);
    return handler->onWrenched ? handler->onWrenched(ctx) : defaultOnWrenched(ctx);
}

} // namespace

namespace ia = common::interactions;

// Fallback priority: the units' own block interactions (which may use the wrench, e.g. a
// deployer's mode) and value boxes run first and fall through to the generic wrench.
class WrenchModule {
public:
    void install() {
        mIds.push_back(ia::onWrench(
            "",
            [](ia::UseOnContext const& c) {
                Vec3 hit{float(c.hit[0]), float(c.hit[1]), float(c.hit[2])}; // block-local (localHit keeps it)
                auto r = onWrenchUse(c.player, game::blockPos(c.pos), c.face, hit);
                return !r || *r == Result::Pass ? ia::Result::Pass : ia::Result::Consume;
            },
            ia::priority::Fallback
        ));
        mIds.push_back(ia::onInteract(
            {.item = ia::kWrenchId},
            [](ia::EntityContext const& c) {
                for (auto const& h : entityHandlers()) {
                    if (h(c.player, c.entity, c.sneaking)) return ia::Result::Consume;
                }
                return ia::Result::Pass;
            },
            ia::priority::Fallback
        ));
        // Create's WrenchItem.wrenchInstaKillsMinecarts: survival players break minecarts in one hit.
        mIds.push_back(ia::onAttack(
            {.item = ia::kWrenchId},
            [](ia::EntityContext const& c) {
                auto& player = c.player;
                auto& entity = c.entity;
                if (!player.isCreative() && c.entityId.ends_with("minecart") && entity.isAlive()) {
                    entity.hurtByCause(100, SharedTypes::Legacy::ActorDamageCause::EntityAttack, player);
                    log().info("wrench: {} broke a {}", player.getRealName(), c.entityId);
                }
                return ia::Result::Pass;
            },
            ia::priority::Fallback
        ));
    }
    void uninstall() {
        for (auto id : mIds) ia::remove(id);
        mIds.clear();
    }

private:
    std::vector<ia::HandlerId> mIds;
};

} // namespace lv::feature::goggles::wrench

LV_REGISTER_MODULE(lv::feature::goggles::wrench::WrenchModule);
