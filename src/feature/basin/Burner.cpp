// Blaze burners (Create's BlazeBurnerBlock / BlazeBurnerBlockEntity / LitBlazeBurnerBlock /
// BlazeBurnerBlockItem): capturing blazes, fuel, blaze cakes, the creative cake, lighting and
// extinguishing the empty burner, heat for basins and boilers (U3's heat capability).

#include <algorithm>
#include <map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BaseMobSpawner.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/SpawnData.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"
#include "mc/world/level/block/actor/MobSpawnerBlockActor.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::basin::burners {

namespace items = common::items;
namespace bd    = common::blockdata;
using cb::HeatLevel;

namespace {

std::map<GlobalPos, BurnerData>& cache() {
    static std::map<GlobalPos, BurnerData> c;
    return c;
}

HeatLevel heatFromBlock(BlockSource& region, BlockPos const& pos) {
    if (!loaded(region, pos)) return HeatLevel::None;
    return heatOfBlock(region.getBlock(pos));
}

/// Create's setBlockHeat: the `blaze` state shows the heat.
void setBlockHeat(BlockSource& region, BlockPos const& pos, HeatLevel heat) {
    if (heat == HeatLevel::None) heat = HeatLevel::Smouldering; // a burner with a blaze never shows "none"
    setState(region, pos, kBlazeState, static_cast<int>(heat));
}

void playBlazeShoot(int dim, BlockPos const& pos) {
    // Create: BLAZE_SHOOT, volume .125-.25, pitch .5-.75
    playSound(dim, pos, "mob.blaze.shoot", .125f + random01() * .125f, .75f - random01() * .25f);
}

bool isShovel(std::string const& id) { return id.ends_with("_shovel"); }
bool soulFireBase(std::string const& id) { return id == "minecraft:soul_sand" || id == "minecraft:soul_soil"; }

void damageHeld(Player& player, int amount) {
    if (player.isCreative()) return;
    auto held = player.getSelectedItem();
    if (held.isNull() || !held.isDamageableItem()) return;
    if (held.hurtAndBreak(amount, &player)) held = items::empty();
    player.setSelectedItem(held);
}

/// Swaps the burner block, keeping its facing.
void replaceBurner(BlockSource& region, BlockPos const& pos, char const* id, std::vector<std::pair<std::string_view, int>> extra = {}) {
    int facing = stateAt(region, pos, kFacingState, kSouth);
    extra.emplace_back(kFacingState, facing);
    if (auto const* b = game::blockWith(id, extra)) game::setBlock(region, pos, *b);
}

void giveBurnerItemTo(Player& player) {
    auto filled = items::make(kBurner);
    if (!filled) return;
    auto held = player.getSelectedItem();
    if (!player.isCreative()) held = items::withCount(held, items::countOf(held) - 1);
    if (items::isEmpty(held)) player.setSelectedItem(*filled);
    else {
        player.setSelectedItem(held);
        giveOrDrop(player, *filled);
    }
}

void captureEffects(int dim, Vec3 const& at) {
    BlockPos p{static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y)), static_cast<int>(std::floor(at.z))};
    playSound(dim, p, "mob.blaze.hit", .25f, .75f);
    playSound(dim, p, "random.fizz", .5f, .75f);
    for (int i = 0; i < 8; ++i) {
        particle(dim, at.x + (random01() - .5f) * .5f, at.y + random01() * .5f, at.z + (random01() - .5f) * .5f, "minecraft:basic_flame_particle");
    }
    particle(dim, at.x, at.y, at.z, "minecraft:large_explosion");
}

} // namespace

HeatLevel heatOfBlock(Block const& block) {
    auto id = block.getTypeName();
    if (id == kBurner) return static_cast<HeatLevel>(std::clamp(game::intState(block, kBlazeState).value_or(1), 1, 4));
    if (id == kLitBurner) return HeatLevel::Smouldering; // #create:passive_boiler_heaters
    return HeatLevel::None;
}

void registerHeat() {
    common::heat::registerBlock(kBurner, [](BlockSource&, BlockPos const&, Block const& b) { return heatOfBlock(b); });
    common::heat::registerBlock(kLitBurner, [](BlockSource&, BlockPos const&, Block const& b) { return heatOfBlock(b); });
    common::heat::registerBlock(kEmptyBurner, [](BlockSource&, BlockPos const&, Block const&) { return HeatLevel::None; });
    // Arms, deployers and funnels feed burners through the item capability (Create's tryInsert
    // without overflow: automation only refuels below the insertion threshold)
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return 1; };
    h.getSlot = [](common::Access const&, int) { return items::empty(); };
    h.insert  = [](common::Access const& a, ItemStack const& stack, bool simulate) -> ItemStack {
        if (items::isEmpty(stack)) return stack;
        auto gp      = game::globalPos(a.region, a.pos);
        auto [ok, c] = tryInsert(a.region, gp, items::withCount(stack, 1), false, false, simulate);
        if (!ok) return stack;
        if (items::idOf(stack) == kCreativeCake) return stack; // applied, never used up
        if (!simulate && !items::isEmpty(c)) popItem(a.region, a.pos.x + .5f, a.pos.y + 1.f, a.pos.z + .5f, c);
        return items::withCount(stack, items::countOf(stack) - 1);
    };
    h.extract = [](common::Access const&, items::ItemPredicate const&, int, bool) { return items::empty(); };
    common::items::registerBlock(kBurner, std::move(h));
}

BurnerData* get(GlobalPos const& pos) {
    auto& c = cache();
    if (auto it = c.find(pos); it != c.end()) return &it->second;
    BurnerData d;
    if (auto const* doc = bd::get(pos)) {
        if (auto it = doc->find("burner"); it != doc->end() && it->is_object()) {
            d.state.fuel     = static_cast<cb::Fuel>(std::clamp(it->value("fuel", 0), 0, 2));
            d.state.burnTime = it->value("burn", 0);
            d.state.creative = it->value("creative", false);
            d.goggles        = it->value("goggles", false);
        }
    }
    return &c.emplace(pos, d).first->second;
}

void save(GlobalPos const& pos, BurnerData const& d) {
    bd::edit(pos)["burner"] = Json{
        {"fuel",     static_cast<int>(d.state.fuel)},
        {"burn",     d.state.burnTime              },
        {"creative", d.state.creative              },
        {"goggles",  d.goggles                     },
    };
}

void forget(GlobalPos const& pos) { cache().erase(pos); }
void forgetAll() { cache().clear(); }

void tick(BlockSource& region, GlobalPos const& pos, BurnerData& d) {
    auto bp = game::blockPos(pos);
    particles(region, pos, Kind::Burner);
    if (d.state.creative) return;
    auto before = d.state;
    cb::tickBurner(d.state);
    setBlockHeat(region, bp, cb::burnerHeat(d.state));
    if (before.fuel != d.state.fuel || d.state.burnTime % 20 == 0) {
        if (before.fuel != d.state.fuel || before.burnTime != d.state.burnTime) save(pos, d);
    }
    if (before.fuel != d.state.fuel && d.state.fuel == cb::Fuel::None) log().info("blaze burner at {} ran out of fuel", pos.key());
}

cb::FuelOffer fuelOf(ItemStack const& stack) {
    if (items::isEmpty(stack)) return {};
    auto id = items::idOf(stack);
    if (id == kBlazeCake) return {cb::Fuel::Special, cb::kSpecialBurnTime};
    // the furnace burn time of the item (Create: getBurnTime, vanilla fuel values)
    int burn = static_cast<int>(FurnaceBlockActor::getBurnDuration(stack, 200.f));
    if (burn > 0) return {cb::Fuel::Normal, burn};
    return {};
}

std::pair<bool, ItemStack> tryInsert(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, bool doNotConsume,
                                     bool forceOverflow, bool simulate) {
    auto bp = game::blockPos(pos);
    if (idAt(region, bp) != kBurner) return {false, items::empty()};
    auto* d = get(pos);
    if (!d) return {false, items::empty()};
    auto id = items::idOf(stack);
    if (id == kCreativeCake) {
        if (!simulate) {
            // Create's applyCreativeFuel: the heat cycles, fading is skipped
            d->state = {cb::Fuel::None, 0, true};
            auto next = cb::creativeNext(heatFromBlock(region, bp));
            playBlazeShoot(pos.dim, bp);
            setBlockHeat(region, bp, next);
            save(pos, *d);
            sync::dirty(pos);
            log().info("blaze burner at {} set to {} by a creative blaze cake", pos.key(), core::common::heatLevelName(next));
        }
        return {true, items::empty()};
    }
    auto prev = heatFromBlock(region, bp);
    if (!cb::tryUpdateFuel(d->state, fuelOf(stack), forceOverflow, simulate)) return {false, items::empty()};
    if (!simulate) {
        playBlazeShoot(pos.dim, bp);
        setBlockHeat(region, bp, cb::burnerHeat(d->state));
        if (prev != heatFromBlock(region, bp)) {
            playSound(pos.dim, bp, "mob.blaze.breathe", .125f + random01() * .125f, 1.15f - random01() * .25f);
        }
        // Create's spawnParticleBurst
        for (int i = 0; i < 6; ++i) {
            particle(pos.dim, bp.x + .5f + (random01() - .5f) * .6f, bp.y + .6f + random01() * .4f, bp.z + .5f + (random01() - .5f) * .6f,
                     d->state.fuel == cb::Fuel::Special ? "minecraft:blue_flame_particle" : "minecraft:basic_flame_particle");
        }
        save(pos, *d);
        sync::dirty(pos);
        log().info("blaze burner at {} fed {}: {} for {} ticks", pos.key(), id, core::common::heatLevelName(cb::burnerHeat(d->state)), d->state.burnTime);
    }
    ItemStack container = doNotConsume ? items::empty() : recipes::remainderOf(stack);
    return {true, container};
}

bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held) {
    auto bp   = game::blockPos(pos);
    auto kind = kindOf(idAt(region, bp));
    if (!kind) return false;
    auto heldId = items::idOf(held);
    if (*kind == Kind::Burner) {
        auto* d = get(pos);
        if (!d) return false;
        if (heldId == kGoggles) {
            if (d->goggles) return false;
            d->goggles = true;
            save(pos, *d);
            sync::dirty(pos);
            log().info("blaze burner at {} put on goggles", pos.key());
            return true;
        }
        if (heldId.empty()) {
            if (!d->goggles) return false;
            d->goggles = false;
            save(pos, *d);
            sync::dirty(pos);
            log().info("blaze burner at {} took off its goggles", pos.key());
            return true;
        }
        // Create: the creative blaze cake is never used up
        bool doNotConsume = player.isCreative() || heldId == kCreativeCake;
        auto [ok, container] = tryInsert(region, pos, held, doNotConsume, true, false);
        if (!ok) return false;
        if (!doNotConsume) {
            auto rest = items::withCount(held, items::countOf(held) - 1);
            if (items::isEmpty(rest)) player.setSelectedItem(container);
            else {
                player.setSelectedItem(rest);
                giveOrDrop(player, container);
            }
        }
        return true;
    }
    if (*kind == Kind::EmptyBurner) {
        if (heldId != "minecraft:flint_and_steel") return false;
        playSound(pos.dim, bp, "fire.ignite", 1.f, random01() * .4f + .8f);
        damageHeld(player, 1);
        replaceBurner(region, bp, kLitBurner, {{kFlameState, 0}});
        log().info("empty blaze burner at {} lit by {}", pos.key(), player.getRealName());
        return true;
    }
    if (*kind == Kind::LitBurner) {
        if (isShovel(heldId)) {
            playSound(pos.dim, bp, "random.fizz", .5f, 2.f);
            damageHeld(player, 1);
            replaceBurner(region, bp, kEmptyBurner);
            log().info("lit blaze burner at {} put out by {}", pos.key(), player.getRealName());
            return true;
        }
        if (stateAt(region, bp, kFlameState, 0) == 0 && soulFireBase(heldId)) {
            playSound(pos.dim, bp, "dig.soul_sand", 1.f, random01() * .4f + .8f);
            setState(region, bp, kFlameState, 1);
            log().info("lit blaze burner at {} turned to soul fire by {}", pos.key(), player.getRealName());
            return true;
        }
    }
    return false;
}

bool captureFromEntity(Player& player, Actor& target) {
    // Create's #create:blaze_burner_capturable: blazes
    if (target.getTypeName() != "minecraft:blaze" || !target.isAlive()) return false;
    int  dim = static_cast<int>(player.getDimensionId());
    Vec3 at  = target.getPosition();
    captureEffects(dim, at);
    giveBurnerItemTo(player);
    target.despawn();
    log().info("{} captured a blaze with an empty blaze burner", player.getRealName());
    return true;
}

bool captureFromSpawner(Player& player, BlockSource& region, BlockPos const& pos) {
    auto* actor = region.getBlockEntity(pos);
    if (!actor || idAt(region, pos) != "minecraft:mob_spawner") return false;
    auto& spawnerActor = static_cast<MobSpawnerBlockActor&>(*actor);
    BaseMobSpawner* spawner = spawnerActor.mSpawner.get();
    if (!spawner) return false;
    auto isBlaze = [](ActorDefinitionIdentifier const& id) {
        return *id.mFullName == "minecraft:blaze" || *id.mIdentifier == "blaze";
    };
    bool blaze = isBlaze(*spawner->mActorDefId);
    for (auto const& data : *spawner->mSpawnPotentials) blaze = blaze || isBlaze(*data.mActorId);
    if (!blaze) return false;
    int dim = static_cast<int>(player.getDimensionId());
    captureEffects(dim, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f});
    giveBurnerItemTo(player);
    log().info("{} captured a blaze from the spawner at {} {} {}", player.getRealName(), pos.x, pos.y, pos.z);
    return true;
}

void particles(BlockSource& region, GlobalPos const& pos, Kind kind) {
    if (!config::get<BasinConfig>().burnerParticles) return;
    auto bp  = game::blockPos(pos);
    int  dim = pos.dim;
    float cx = bp.x + .5f, cy = bp.y + .5f, cz = bp.z + .5f;
    if (kind == Kind::LitBurner) {
        // Create's LitBlazeBurnerBlock.animateTick
        if (random01() < .25f) particle(dim, cx + (random01() - .5f) * .6f, bp.y + random01() + random01(), cz + (random01() - .5f) * .6f, "minecraft:basic_smoke_particle");
        if (random01() < .1f) playSound(dim, bp, "block.campfire.crackle", .25f + random01() * .25f, random01() * .7f + .6f);
        bool soul = stateAt(region, bp, kFlameState, 0) == 1;
        if (soul && random01() < 1.f / 8.f) particle(dim, cx, bp.y + .3f + random01() / 2, cz, "minecraft:soul_particle");
        if (!soul && random01() < .2f) particle(dim, cx, cy, cz, "minecraft:lava_particle");
        return;
    }
    // Create's BlazeBurnerBlockEntity.spawnParticles (1 in 4 ticks) and animateTick crackle (1 in 10)
    auto heat = heatFromBlock(region, bp);
    if (random01() < .1f) playSound(dim, bp, "block.campfire.crackle", .5f + random01(), random01() * .7f + .6f);
    if (random01() >= .25f) return;
    bool empty = !hasCollision(region, offsetOf(bp, kUp));
    if (empty || random01() < .125f) particle(dim, cx + (random01() - .5f) * .25f, cy, cz + (random01() - .5f) * .25f, "minecraft:basic_smoke_particle");
    float r  = (empty ? .25f : .5f) + random01() * .125f;
    float a  = random01() * 6.2831853f;
    float px = cx + std::cos(a) * r, pz = cz + std::sin(a) * r, py = cy + .5f;
    if (heat == HeatLevel::Seething) particle(dim, px, py, pz, "minecraft:blue_flame_particle");
    else if (heat == HeatLevel::Fading || heat == HeatLevel::Kindled) particle(dim, px, py, pz, "minecraft:basic_flame_particle");
}

} // namespace lv::feature::basin::burners
