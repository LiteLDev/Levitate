// Held tools: extendo grip (reach, air), sand paper (polishing, scraping) and tree fertilizer (the
// potato cannon is in Projectile.cpp).

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/FertilizerType.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include "core/equipment/Gear.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

// ---- extendo grip -------------------------------------------------------------------------------------

float extendoReach(Player const& player) {
    bool main = items::idOf(player.getSelectedItem()) == kExtendoGrip;
    bool off  = items::idOf(player.getOffhandSlot()) == kExtendoGrip;
    if (main && off) return 5.f;
    if (main || off) return 3.f;
    return 0.f;
}

void extendoAction(Player& player) {
    bool main = items::idOf(player.getSelectedItem()) == kExtendoGrip;
    bool off  = items::idOf(player.getOffhandSlot()) == kExtendoGrip;
    if (!main && !off) return;
    // Create: the main hand grip is checked first
    useAirOrDurability(player, main, config().maxExtendoGripActions);
}

// ---- sand paper ---------------------------------------------------------------------------------------------

namespace {
Actor* actorById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level || !uid) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->fetchEntity(id, false);
}
} // namespace

namespace {
struct Sanding {
    std::uint64_t start    = 0;
    bool          mainHand = true;
    bool          fromHand = true; ///< polishing the item in the other hand (else an item entity)
    std::int64_t  entity   = 0;
};
std::map<std::int64_t, Sanding>& sandings() {
    static std::map<std::int64_t, Sanding> m;
    return m;
}
constexpr int kSandingTicks = 32;

bool polishable(ItemStack const& s) {
    return !items::isEmpty(s) && common::recipes::findFirst("sandpaper_polishing", s) != nullptr;
}
} // namespace

bool startSanding(Player& player, bool mainHand) {
    ItemStack other = mainHand ? player.getOffhandSlot() : player.getSelectedItem();
    Sanding   s;
    s.start    = currentTick();
    s.mainHand = mainHand;
    if (polishable(other)) {
        s.fromHand = true;
    } else {
        // an item entity near the looked-at point, within 3 blocks
        auto hit = player.traceRay(3.0f, false, true);
        if (hit.mType != HitResultType::Tile) return false;
        auto* region = regionOf(static_cast<int>(player.getDimensionId()));
        if (!region) return false;
        AABB box{Vec3{hit.mPos.x - 1, hit.mPos.y - 1, hit.mPos.z - 1}, Vec3{hit.mPos.x + 1, hit.mPos.y + 1, hit.mPos.z + 1}};
        for (auto* e : region->getEntities(box, 0.0f)) {
            if (!e || !e->isType(ActorType::ItemEntity)) continue;
            if (!polishable(static_cast<ItemActor*>(e)->item())) continue;
            s.fromHand = false;
            s.entity   = uidOf(*e);
            break;
        }
        if (!s.entity) return false;
    }
    sandings()[uidOf(player)] = s;
    return true;
}

void cancelSanding(Player& player) { sandings().erase(uidOf(player)); }

void tickSanding() {
    for (auto it = sandings().begin(); it != sandings().end();) {
        auto*  player = playerById(it->first);
        auto&  s      = it->second;
        bool   drop   = !player;
        if (player) {
            ItemStack paper = s.mainHand ? player->getSelectedItem() : player->getOffhandSlot();
            if (!isPaper(items::idOf(paper))) drop = true;
            else {
                auto age = currentTick() - s.start;
                auto pos = player->getPosition();
                int  dim = static_cast<int>(player->getDimensionId());
                if (age >= 6 && (age - 6) % 7 == 0) sound(dim, pos.x, pos.y, pos.z, "step.sand", .9f, 1.f + float(random01()) * .2f);
                if (age >= kSandingTicks) {
                    ItemStack source;
                    ItemActor* entity = nullptr;
                    if (s.fromHand) source = s.mainHand ? player->getOffhandSlot() : player->getSelectedItem();
                    else if (auto* a = actorById(s.entity); a && a->isType(ActorType::ItemEntity)) {
                        entity = static_cast<ItemActor*>(a);
                        source = entity->item();
                    }
                    auto const* r = polishable(source) ? common::recipes::findFirst("sandpaper_polishing", source) : nullptr;
                    if (r) {
                        auto rest = items::withCount(source, items::countOf(source) - 1);
                        if (entity) {
                            if (items::isEmpty(rest)) entity->remove();
                            else entity->item() = rest;
                        } else if (s.mainHand) player->setOffhandSlot(rest);
                        else player->setSelectedItem(rest);
                        for (auto const& out : common::recipes::rollResults(*r)) giveOrDrop(*player, out);
                        if (!player->isCreative()) {
                            paper.hurtAndBreak(1, player);
                            if (s.mainHand) player->setSelectedItem(paper);
                            else player->setOffhandSlot(paper);
                        }
                        player->sendInventory(false);
                        sound(dim, pos.x, pos.y, pos.z, "step.sand", 1.f, 1.2f);
                        for (int i = 0; i < 5; ++i) particle(dim, pos.x, pos.y + 1.2, pos.z, "minecraft:basic_crit_particle");
                        log().info("equipment: {} polished {}", player->getRealName(), items::idOf(source));
                    }
                    drop = true;
                }
            }
        }
        if (drop) it = sandings().erase(it);
        else ++it;
    }
}

bool sandBlock(Player& player, ItemStack& held, GlobalPos const& pos) {
    // Create: sand paper scrapes oxidation and removes wax like an axe
    std::string id = idAt(pos);
    if (!id.starts_with("minecraft:") || id.find("copper") == std::string::npos) return false;
    for (auto bad : {"stairs", "slab", "door", "bulb", "lantern", "chain", "torch", "ore", "raw", "grate"})
        if (id.find(bad) != std::string::npos) return false;
    std::string next;
    if (id.starts_with("minecraft:waxed_")) next = "minecraft:" + id.substr(16);
    else if (auto p = id.find("oxidized_"); p != std::string::npos) next = id.substr(0, p) + "weathered_" + id.substr(p + 9);
    else if (auto p2 = id.find("weathered_"); p2 != std::string::npos) next = id.substr(0, p2) + "exposed_" + id.substr(p2 + 10);
    else if (auto p3 = id.find("exposed_"); p3 != std::string::npos) {
        next = id.substr(0, p3) + id.substr(p3 + 8);
        if (next == "minecraft:copper") next = "minecraft:copper_block";
    }
    if (next.empty() || !game::blockWith(next)) return false;
    place(pos, next);
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "step.sand", 1.f, 1.f);
    if (!player.isCreative()) {
        held.hurtAndBreak(1, &player);
        player.sendInventory(false);
    }
    log().info("equipment: {} sanded {} into {}", player.getRealName(), id, next);
    return true;
}

// ---- tree fertilizer --------------------------------------------------------------------------------------

bool useTreeFertilizer(Player& player, ItemStack& held, GlobalPos const& pos) {
    std::string id = idAt(pos);
    bool sapling = id.ends_with("_sapling") || id == "minecraft:mangrove_propagule" || id == "minecraft:azalea" || id == "minecraft:flowering_azalea"
                || id == "minecraft:sapling";
    if (!sapling) return false;
    if (id == "minecraft:mangrove_propagule" && stateAt(pos, "hanging").value_or(0)) return true; // Create: nothing on a hanging propagule
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    BlockPos bp{pos.x, pos.y, pos.z};
    // grow at once: fertilize until the sapling turns into a tree (Bedrock's own growth, incl. 2x2)
    for (int i = 0; i < 64 && idAt(pos) == id; ++i) {
        auto const& b = region->getBlock(bp);
        b.getBlockType().onFertilized(*region, bp, &player, FertilizerType::Rapid);
    }
    bool grew = idAt(pos) != id;
    if (!player.isCreative()) {
        held.remove(1);
        player.sendInventory(false);
    }
    for (int i = 0; i < 12; ++i)
        particle(pos.dim, pos.x + random01(), pos.y + random01(), pos.z + random01(), "minecraft:crop_growth_emitter");
    log().info("equipment: {} used tree fertilizer on {} at {} {} {} ({})", player.getRealName(), id, pos.x, pos.y, pos.z, grew ? "grew" : "no room");
    return true;
}

} // namespace lv::feature::equipment
