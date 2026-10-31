// Movement behaviours of vanilla blocks on contraptions (Create's AllMovementBehaviours):
// droppers and dispensers drop or shoot from their own carried inventory at each new position
// (DropperMovementBehaviour, DispenserMovementBehaviour with the mounted dispense behaviours of
// TNT, fire charges and projectiles), bells ring when the contraption starts, turns back or stops
// (BellMovementBehaviour) and lit campfires smoke (CampfireMovementBehaviour).

#include <algorithm>
#include <cmath>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"

#include "feature/common/Capabilities.h"
#include "feature/contraption/Internal.h"
#include "mod/Log.h"
#include <string>
#include "ll/api/form/SimpleForm.h"
#include "mc/world/actor/player/Player.h"
#include "feature/common/Items.h"
#include "feature/contraption/Registries.h"
#include "mod/I18n.h"

namespace lv::feature::contraption {

namespace {

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
double rand01() { return std::uniform_real_distribution<double>(0, 1)(rng()); }
double gaussian() { return std::normal_distribution<double>(0, 1)(rng()); }

std::int64_t intOf(cc::States const& s, char const* key, std::int64_t fallback) {
    auto it = s.find(key);
    if (it == s.end()) return fallback;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    return v ? *v : fallback;
}

/// MountedDispenseBehavior#getDispenserNormal: the facing turned with the contraption.
cc::Vec3d dispenserNormal(MovementContext const& ctx) {
    auto facing = static_cast<cc::Dir>(std::clamp<std::int64_t>(intOf(ctx.states, "facing_direction", 0), 0, 5));
    return ctx.contraption.transform().rotateVector(cc::Vec3d::of(cc::offset(facing)));
}

cc::Dir closestFacing(cc::Vec3d const& v) {
    cc::Dir best  = cc::Dir::Up;
    double  bestD = -1e9;
    for (auto d : cc::kAllDirs) {
        auto   o   = cc::offset(d);
        double dot = v.x * o.x + v.y * o.y + v.z * o.z;
        if (dot > bestD) bestD = dot, best = d;
    }
    return best;
}

/// DefaultMountedDispenseBehavior#spawnItem.
void spawnItem(MovementContext const& ctx, ItemStack const& stack, int speed, cc::Vec3d const& facing, core::GlobalPos const& pos) {
    auto  level  = ll::service::getLevel();
    auto* region = detail::regionOf(pos.dim);
    if (!level || !region) return;
    double x = pos.x + facing.x + .5, y = pos.y + facing.y + .5, z = pos.z + facing.z + .5;
    y -= cc::axisOf(closestFacing(facing)) == cc::Axis::Y ? 0.125 : 0.15625;
    auto* item = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(x), float(y), float(z)}, 0);
    if (!item) return;
    double d3 = rand01() * 0.1 + 0.2;
    item->lerpMotion(Vec3{
        float(gaussian() * 0.0075 * speed + facing.x * d3 + ctx.motion.x),
        float(gaussian() * 0.0075 * speed + facing.y * d3 + ctx.motion.y),
        float(gaussian() * 0.0075 * speed + facing.z * d3 + ctx.motion.z)
    });
}

/// The entity a dispenser shoots for an item (Create's mounted dispense behaviours and the
/// projectile items), "" for the default (drop the item).
std::string shotOf(std::string const& id) {
    if (id == "minecraft:tnt") return "minecraft:tnt";
    if (id == "minecraft:fire_charge") return "minecraft:small_fireball";
    if (id == "minecraft:arrow") return "minecraft:arrow";
    if (id == "minecraft:snowball") return "minecraft:snowball";
    if (id == "minecraft:egg") return "minecraft:egg";
    return {};
}

/// DropperMovementBehaviour#visitNewPosition (and the dispenser's behaviours).
void dispense(MovementContext& ctx, core::GlobalPos const& pos, bool dispenser) {
    auto* region = detail::regionOf(pos.dim);
    if (!region) return;
    auto slots = detail::blockSlots(ctx.contraption, ctx.localPos);
    // getSlot: empty slots are skipped, single items of stackables topped off from the storage
    std::vector<ItemStack*> filled;
    for (auto* s : slots) {
        if (s->isNull() || s->mCount == 0) continue;
        if (s->mCount == 1 && s->getMaxStackSize() != 1) {
            auto same = [&](ItemStack const& o) { return o.isStackable(*s); };
            // (the slot itself is part of the storage: take it out first so it isn't counted)
            ItemStack self = *s;
            *s             = ItemStack{};
            auto more      = ctx.contraption.storage().extractItem(same, self.getMaxStackSize() - 1, false);
            *s             = self;
            if (more.isNull() || more.mCount == 0) continue;
            s->set(s->mCount + more.mCount);
        }
        filled.push_back(s);
    }
    cc::Vec3d center = cc::Vec3d::center({pos.x, pos.y, pos.z});
    if (filled.empty()) {
        detail::playSound(pos.dim, center, "random.click", 1.f, 1.2f); // SOUND_DISPENSER_FAIL
        return;
    }
    auto*     slot   = filled[std::uniform_int_distribution<std::size_t>(0, filled.size() - 1)(rng())];
    cc::Vec3d normal = dispenserNormal(ctx);
    cc::Dir   face   = closestFacing(normal);
    auto      shot   = dispenser ? shotOf(slot->getTypeName()) : std::string();
    if (!shot.empty()) {
        // the projectile dispense behaviours: shot along the turned facing
        auto level = ll::service::getLevel();
        if (!level) return;
        cc::Vec3d at = center + normal * 0.7;
        if (shot == "minecraft:tnt") at = center + normal;
        auto* a = level->getSpawner().spawnProjectile(
            *region,
            ActorDefinitionIdentifier(shot),
            nullptr,
            Vec3{float(at.x), float(at.y), float(at.z)},
            Vec3{float(normal.x), float(normal.y), float(normal.z)}
        );
        if (!a) return;
        slot->set(slot->mCount - 1);
        if (slot->mCount <= 0) *slot = ItemStack{};
        detail::playSound(pos.dim, center, shot == "minecraft:tnt" ? "random.fuse" : "random.bow", 1.f, 1.f);
        return;
    }
    cc::Pos front{pos.x + cc::offset(face).x, pos.y + cc::offset(face).y, pos.z + cc::offset(face).z};
    if (common::items::has(*region, detail::toBlockPos(front), static_cast<int>(cc::opposite(face)))) {
        // a container in front takes one item (HopperBlockEntity.addItem)
        ItemStack one = *slot;
        one.set(1);
        auto rest = common::items::insert(*region, detail::toBlockPos(front), static_cast<int>(cc::opposite(face)), one, false);
        if (rest.isNull() || rest.mCount == 0) {
            slot->set(slot->mCount - 1);
            if (slot->mCount <= 0) *slot = ItemStack{};
        }
        return;
    }
    ItemStack one = *slot;
    one.set(1);
    slot->set(slot->mCount - 1);
    if (slot->mCount <= 0) *slot = ItemStack{};
    spawnItem(ctx, one, 6, normal, pos);
    detail::playSound(pos.dim, center, "random.click", 1.f, 1.f); // SOUND_DISPENSER_DISPENSE
    detail::particle(pos.dim, center + normal * 0.6, "minecraft:basic_smoke_particle");
}

/// BellMovementBehaviour#playSound (vanilla bell).
void ring(MovementContext const& ctx) { detail::playSound(ctx.contraption.dimension(), ctx.position, "block.bell.hit", 2.f, 1.f); }

struct VanillaActors {
    VanillaActors() {
        for (bool dispenser : {false, true}) {
            MovementBehaviour b;
            b.visitNewPosition = [dispenser](MovementContext& ctx, core::GlobalPos const& pos) { dispense(ctx, pos, dispenser); };
            registerActor(dispenser ? "minecraft:dispenser" : "minecraft:dropper", std::move(b));
        }

        MovementBehaviour bell;
        // Create's isActive: not on trains
        bell.isActive    = [](MovementContext const& ctx) { return ctx.contraption.kind() != "carriage"; };
        bell.startMoving = [](MovementContext& ctx) {
            if (ctx.contraption.kind() != "elevator") ring(ctx); // firstMovement
            ctx.data["BellMotion"] = nlohmann::json::array({ctx.motion.x, ctx.motion.y, ctx.motion.z});
        };
        bell.tick = [](MovementContext& ctx) {
            if (ctx.contraption.kind() == "elevator") {
                // rings when the cabin arrives (ElevatorContraption#arrived)
                auto const& e       = ctx.contraption.anchorData().value("elevator", nlohmann::json::object());
                bool        arrived = e.contains("target") && e.value("target", 0) == e.value("current", 1);
                if (!arrived) ctx.data["BellMoved"] = true;
                else if (ctx.data.value("BellMoved", false)) {
                    ring(ctx);
                    ctx.data.erase("BellMoved");
                }
                return;
            }
            // onSpeedChanged: the motion turned back
            auto const& last = ctx.data.value("BellMotion", nlohmann::json::array());
            if (last.is_array() && last.size() == 3) {
                double dot = last[0].get<double>() * ctx.motion.x + last[1].get<double>() * ctx.motion.y + last[2].get<double>() * ctx.motion.z;
                if (dot < 0 && ctx.motion.lengthSqr() > 1e-10) ring(ctx);
            }
            if (ctx.motion.lengthSqr() > 1e-10) ctx.data["BellMotion"] = nlohmann::json::array({ctx.motion.x, ctx.motion.y, ctx.motion.z});
        };
        bell.stopMoving = [](MovementContext& ctx) {
            if (ctx.contraption.kind() != "carriage" && ctx.contraption.kind() != "elevator") ring(ctx);
            ctx.data.erase("BellMotion");
        };
        bell.tickWhileStill = true; // an elevator cabin stands still when it arrives
        registerActor("minecraft:bell", std::move(bell));

        for (char const* id : {"minecraft:campfire", "minecraft:soul_campfire"}) {
            MovementBehaviour fire;
            fire.tick = [](MovementContext& ctx) {
                if (intOf(ctx.states, "extinguished", 0) != 0) return;
                if (rand01() >= 0.11) return;
                int n = 2 + static_cast<int>(rand01() * 2);
                for (int i = 0; i < n; ++i) {
                    cc::Vec3d at{
                        ctx.position.x + rand01() / (rand01() < .5 ? 3.0 : -3.0),
                        ctx.position.y + rand01() + rand01(),
                        ctx.position.z + rand01() / (rand01() < .5 ? 3.0 : -3.0)
                    };
                    detail::particle(ctx.contraption.dimension(), at, "minecraft:campfire_smoke_particle");
                }
            };
            fire.tickWhileStill = true; // Create ticks every actor each tick
            registerActor(id, std::move(fire));
        }
    }
};
VanillaActors const gVanillaActors;

} // namespace

} // namespace lv::feature::contraption

// Right-clicks on blocks of a contraption (Create's AllInteractionBehaviours and
// MountedItemStorage#handleInteraction): levers toggle (LeverMovingInteraction), wooden doors,
// trapdoors and fence gates open and close (DoorMovingInteraction, TrapdoorMovingInteraction), and
// mounted inventories open. Create opens the container's own menu; a Bedrock client can only
// show a container screen for a real block, so the inventory is a form (take a stack out, put the
// held stack in). U22's ray cast over contraption blocks (actors) calls `interactionFor`.
namespace lv::feature::contraption {
namespace moving_interactions {


namespace {

namespace items = common::items;

std::int64_t intOf(cc::States const& s, char const* key, std::int64_t fallback) {
    auto it = s.find(key);
    if (it == s.end()) return fallback;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    return v ? *v : fallback;
}

CarriedBlock const* blockAt(Contraption const& c, cc::Pos const& local) {
    for (auto const& b : c.blocks())
        if (b.local == local) return &b;
    return nullptr;
}

/// Toggles `open_bit` of the carried block at `local`; returns the new value (nullopt: none).
std::optional<bool> toggleOpen(Contraption& c, cc::Pos const& local) {
    auto const* b = blockAt(c, local);
    if (!b || !b->states.contains("open_bit")) return std::nullopt;
    bool       open   = intOf(b->states, "open_bit", 0) == 0;
    cc::States states = b->states;
    states["open_bit"] = std::int64_t{open ? 1 : 0};
    if (!setBlockStates(c.id(), local, states)) return std::nullopt;
    return open;
}

bool woodenDoorLike(std::string_view id, std::string_view suffix) {
    if (!id.ends_with(suffix)) return false;
    return id.find("iron") == std::string_view::npos && id.find("copper") == std::string_view::npos;
}

void logClick(Player& player, Contraption const& c, cc::Pos const& local, std::string const& what) {
    log().info("{} {} on contraption #{} at {} {} {}", player.getRealName(), what, c.id(), local.x, local.y, local.z);
}

// ---- mounted storage form -----------------------------------------------------------------------

void openStorage(Player& player, Id id, cc::Pos local);

/// The stacks of the inventory at `local` (vanilla slots or an adapted inventory).
std::vector<ItemStack> stacksAt(Contraption& c, cc::Pos const& local) {
    std::vector<ItemStack> out;
    for (auto* s : detail::blockSlots(c, local)) out.push_back(*s);
    if (!out.empty()) return out;
    auto const* b = blockAt(c, local);
    if (!b) return out;
    if (auto const* adapter = detail::itemAdapter(b->id); adapter && adapter->slots) out = adapter->slots(b->data);
    return out;
}

/// Takes slot `index`'s stack (or what is left of it) into the player's inventory.
void take(Player& player, Contraption& c, cc::Pos const& local, int index) {
    auto slots = detail::blockSlots(c, local);
    ItemStack got;
    if (!slots.empty()) {
        if (index < 0 || index >= static_cast<int>(slots.size())) return;
        got           = *slots[static_cast<std::size_t>(index)];
        *slots[static_cast<std::size_t>(index)] = ItemStack{};
    } else {
        auto const* b       = blockAt(c, local);
        auto const* adapter = b ? detail::itemAdapter(b->id) : nullptr;
        auto*       data    = c.blockData(local);
        if (!adapter || !adapter->slots || !adapter->extract || !data) return;
        auto view = adapter->slots(*data);
        if (index < 0 || index >= static_cast<int>(view.size()) || items::isEmpty(view[static_cast<std::size_t>(index)])) return;
        auto want = view[static_cast<std::size_t>(index)];
        got       = adapter->extract(*data, [&](ItemStack const& s) { return s.isStackable(want); }, items::countOf(want), false);
    }
    if (items::isEmpty(got)) return;
    auto copy = got;
    if (!player.add(copy)) player.drop(copy, false);
    player.refreshInventory();
    logClick(player, c, local, "took " + items::idOf(got) + " x" + std::to_string(items::countOf(got)) + " from a mounted inventory");
}

/// Puts the held stack into the inventory at `local`.
void deposit(Player& player, Contraption& c, cc::Pos const& local) {
    ItemStack held = player.getSelectedItem();
    if (items::isEmpty(held)) return;
    ItemStack rest = held;
    auto      slots = detail::blockSlots(c, local);
    if (!slots.empty()) {
        for (int pass = 0; pass < 2 && items::countOf(rest) > 0; ++pass) {
            for (auto* s : slots) {
                if (items::countOf(rest) == 0) break;
                bool empty = s->isNull() || s->mCount == 0;
                if (pass == 0 ? (empty || !s->isStackable(rest)) : !empty) continue;
                int max   = std::max<int>(1, rest.getMaxStackSize());
                int have  = empty ? 0 : s->mCount;
                int moved = std::min<int>(max - have, rest.mCount);
                if (moved <= 0) continue;
                if (empty) {
                    *s = rest;
                    s->set(moved);
                } else {
                    s->set(have + moved);
                }
                rest.set(rest.mCount - moved);
            }
        }
    } else {
        auto const* b       = blockAt(c, local);
        auto const* adapter = b ? detail::itemAdapter(b->id) : nullptr;
        auto*       data    = c.blockData(local);
        if (!adapter || !adapter->insert || !data) return;
        rest = adapter->insert(*data, held, false);
    }
    if (items::countOf(rest) == items::countOf(held)) return;
    player.setSelectedItem(items::countOf(rest) == 0 ? ItemStack{} : rest);
    player.refreshInventory();
    logClick(player, c, local, "put " + items::idOf(held) + " into a mounted inventory");
}

void openStorage(Player& player, Id id, cc::Pos local) {
    auto* c = find(id);
    if (!c) return;
    auto stacks = stacksAt(*c, local);
    ll::form::SimpleForm form(
        trFor(player, "lv.contraption.mounted_storage", "Mounted Storage"),
        trFor(player, "lv.contraption.mounted_storage.content", "Take a stack out, or put the held stack in.")
    );
    form.appendButton(trFor(player, "lv.contraption.mounted_storage.deposit", "Put the held stack in"), [id, local](Player& p) {
        if (auto* cc = find(id)) {
            deposit(p, *cc, local);
            openStorage(p, id, local);
        }
    });
    for (int i = 0; i < static_cast<int>(stacks.size()); ++i) {
        auto const& s = stacks[static_cast<std::size_t>(i)];
        if (items::isEmpty(s)) continue;
        form.appendButton(items::displayName(s) + " x" + std::to_string(items::countOf(s)), [id, local, i](Player& p) {
            if (auto* cc = find(id)) {
                take(p, *cc, local, i);
                openStorage(p, id, local);
            }
        });
    }
    form.sendTo(player);
}

struct Interactions {
    Interactions() {
        // LeverMovingInteraction
        registerInteraction("minecraft:lever", [](Player& player, Contraption& c, cc::Pos const& local) {
            auto open = toggleOpen(c, local);
            if (!open) return false;
            detail::playSound(c.dimension(), c.toWorld(cc::Vec3d::center(local)), "random.click", 0.3f, *open ? 0.6f : 0.5f);
            logClick(player, c, local, *open ? "switched a lever on" : "switched a lever off");
            return true;
        });
        // DoorMovingInteraction: both halves of a wooden door
        registerInteractionProvider(
            [](std::string_view id) { return woodenDoorLike(id, "_door") && id.find("trapdoor") == std::string_view::npos; },
            [](Player& player, Contraption& c, cc::Pos const& local) {
                auto const* b = blockAt(c, local);
                if (!b) return false;
                bool    upper = intOf(b->states, "upper_block_bit", 0) != 0;
                cc::Pos other = local + cc::offset(upper ? cc::Dir::Down : cc::Dir::Up);
                auto    open  = toggleOpen(c, local);
                if (!open) return false;
                if (auto const* o = blockAt(c, other); o && o->states.contains("open_bit")) {
                    cc::States s = o->states;
                    s["open_bit"] = std::int64_t{*open ? 1 : 0};
                    setBlockStates(c.id(), other, s);
                }
                detail::playSound(c.dimension(), c.toWorld(cc::Vec3d::center(local)), *open ? "random.door_open" : "random.door_close", 1.f, 1.f);
                logClick(player, c, local, *open ? "opened a door" : "closed a door");
                return true;
            }
        );
        // TrapdoorMovingInteraction: wooden trapdoors and fence gates
        auto trapdoor = [](Player& player, Contraption& c, cc::Pos const& local) {
            auto open = toggleOpen(c, local);
            if (!open) return false;
            detail::playSound(c.dimension(), c.toWorld(cc::Vec3d::center(local)), *open ? "random.door_open" : "random.door_close", 1.f, 1.f);
            logClick(player, c, local, *open ? "opened a trapdoor" : "closed a trapdoor");
            return true;
        };
        registerInteractionProvider([](std::string_view id) { return woodenDoorLike(id, "trapdoor"); }, trapdoor);
        registerInteractionProvider([](std::string_view id) { return id.ends_with("fence_gate"); }, trapdoor);
        // MountedItemStorage#handleInteraction: vanilla containers and adapted inventories
        registerInteractionProvider(
            [](std::string_view id) {
                return id == "minecraft:chest" || id == "minecraft:trapped_chest" || id == "minecraft:barrel" || id.ends_with("shulker_box")
                    || id == "minecraft:dispenser" || id == "minecraft:dropper" || id == "minecraft:hopper"
                    || detail::itemAdapter(std::string(id)) != nullptr;
            },
            [](Player& player, Contraption& c, cc::Pos const& local) {
                detail::playSound(c.dimension(), c.toWorld(cc::Vec3d::center(local)), "random.chestopen", 0.5f, 1.f);
                openStorage(player, c.id(), local);
                logClick(player, c, local, "opened a mounted inventory");
                return true;
            }
        );
    }
};
Interactions const gInteractions;

} // namespace

} // namespace moving_interactions
} // namespace lv::feature::contraption
