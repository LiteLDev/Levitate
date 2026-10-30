// The basin (Create's BasinBlock / BasinBlockEntity / BasinInventory): 9 input slots (one stack
// per item kind), 9 output slots, 2 input and 2 output tanks of 1000 mB, the spout output to the
// side, item entities falling in, player use, the item / fluid capabilities.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/basin/BasinInternal.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::basin::basinops {

namespace items  = common::items;
namespace fluids = common::fluids;
namespace bd     = common::blockdata;

namespace {

std::map<GlobalPos, BasinState>& cache() {
    static std::map<GlobalPos, BasinState> c;
    return c;
}

Json fluidJson(cb::Fluid const& f) { return f.empty() ? Json(nullptr) : Json::array({f.id, f.amount}); }
cb::Fluid fluidFrom(Json const& j) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_string() || !j[1].is_number_integer()) return {};
    return {j[0].get<std::string>(), j[1].get<int>()};
}

BasinState load(Json const* doc) {
    BasinState b;
    if (!doc) return b;
    auto it = doc->find("basin");
    if (it == doc->end() || !it->is_object()) return b;
    auto const& j = *it;
    auto readSlots = [&](char const* key, std::array<ItemStack, cb::kSlotCount>& out) {
        if (auto s = j.find(key); s != j.end() && s->is_array())
            for (std::size_t i = 0; i < out.size() && i < s->size(); ++i) out[i] = items::fromJson((*s)[i]);
    };
    auto readTanks = [&](char const* key, std::vector<cb::Fluid>& out) {
        if (auto s = j.find(key); s != j.end() && s->is_array())
            for (std::size_t i = 0; i < out.size() && i < s->size(); ++i) out[i] = fluidFrom((*s)[i]);
    };
    readSlots("in", b.in);
    readSlots("out", b.out);
    readTanks("tin", b.tankIn);
    readTanks("tout", b.tankOut);
    b.preferred = j.value("pref", -1);
    if (auto d = j.find("disabled"); d != j.end() && d->is_array())
        for (auto const& f : *d)
            if (f.is_number_integer()) b.disabled.push_back(f.get<int>());
    if (auto s = j.find("spout"); s != j.end() && s->is_array())
        for (auto const& e : *s)
            if (auto st = items::fromJson(e); !items::isEmpty(st)) b.spout.push_back(st);
    if (auto s = j.find("spoutFluid"); s != j.end() && s->is_array())
        for (auto const& e : *s)
            if (auto f = fluidFrom(e); !f.empty()) b.spoutFluid.push_back(f);
    return b;
}

int slotLimit(ItemStack const& stack) { return std::min(64, maxStackOf(stack)); }

/// Create's BasinInventory + ItemHandlerHelper.insertItemStacked: the slot holding the same item,
/// else the first free slot; returns the remainder.
ItemStack insertInto(std::array<ItemStack, cb::kSlotCount>& slots, ItemStack const& stack, bool simulate) {
    if (items::isEmpty(stack)) return items::empty();
    int target = -1;
    for (int i = 0; i < cb::kSlotCount; ++i) {
        if (!items::isEmpty(slots[i]) && items::sameItemSameData(slots[i], stack)) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        for (int i = 0; i < cb::kSlotCount; ++i) {
            if (items::isEmpty(slots[i])) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) return stack;
    int have  = items::countOf(slots[target]);
    int room  = slotLimit(stack) - have;
    int moved = std::clamp(items::countOf(stack), 0, std::max(room, 0));
    if (moved <= 0) return stack;
    if (!simulate) slots[target] = items::withCount(stack, have + moved);
    return items::withCount(stack, items::countOf(stack) - moved);
}

/// The fluid's sound suffix ("water" / "lava").
std::string fluidSound(std::string const& fluid, bool fill) {
    std::string kind = fluid == "minecraft:lava" ? "lava" : "water";
    return std::string(fill ? "bucket.fill_" : "bucket.empty_") + kind;
}

/// The container filled with `fluid` (Create's GenericItemFilling for buckets and bottles):
/// {filled item id, aux, required amount}; empty id when the container can't take it.
struct Filling {
    std::string id;
    int         aux    = 0;
    int         amount = 0;
};
Filling fillingFor(std::string const& container, std::string const& fluid) {
    if (container == "minecraft:bucket") {
        static std::map<std::string, std::string> const buckets{
            {"minecraft:water",       "minecraft:water_bucket"      },
            {"minecraft:lava",        "minecraft:lava_bucket"       },
            {"minecraft:milk",        "minecraft:milk_bucket"       },
            {"minecraft:powder_snow", "minecraft:powder_snow_bucket"},
            {"levitate:honey",        "levitate:honey_bucket"       },
            {"levitate:chocolate",    "levitate:chocolate_bucket"   },
        };
        if (auto it = buckets.find(fluid); it != buckets.end() && items::make(it->second)) return {it->second, 0, 1000};
        return {};
    }
    if (container == "minecraft:glass_bottle" && fluid == "minecraft:water") return {"minecraft:potion", 0, 250};
    return {};
}

/// The empty container of a filled one (water bucket -> bucket, water bottle -> glass bottle).
std::string emptyContainerOf(std::string const& id) {
    if (id == "minecraft:potion" || id.ends_with("_bottle")) return "minecraft:glass_bottle";
    if (id.ends_with("_bucket")) return "minecraft:bucket";
    return {};
}

void changed(GlobalPos const& pos, BasinState& b) {
    notifyChangeOfContents(b);
    save(pos, b);
    sync::dirty(pos);
}

std::vector<int> horizontalOrder() { return {kHorizontals.begin(), kHorizontals.end()}; }

bool directInputBlock(std::string const& id) {
    // blocks with Create's DirectBeltInputBehaviour (by id, owned by other units)
    static std::set<std::string> const ids{
        "levitate:belt",
        "levitate:depot",
        "levitate:weighted_ejector",
        "levitate:basin",
        "levitate:item_drain",
        "levitate:crushing_wheel_controller",
        "levitate:andesite_tunnel",
        "levitate:brass_tunnel",
    };
    return ids.contains(id);
}

bool isFunnel(std::string const& id) { return id == "levitate:andesite_funnel" || id == "levitate:brass_funnel"; }

} // namespace

// ---- state ------------------------------------------------------------------------------------------------

BasinState* get(BlockSource& region, GlobalPos const& pos) {
    auto& c = cache();
    if (auto it = c.find(pos); it != c.end()) return &it->second;
    if (idAt(region, game::blockPos(pos)) != kBasin) return nullptr;
    return &c.emplace(pos, load(bd::get(pos))).first->second;
}

void save(GlobalPos const& pos, BasinState const& b) {
    Json j;
    Json in = Json::array(), out = Json::array(), tin = Json::array(), tout = Json::array();
    for (auto const& s : b.in) in.push_back(items::toJson(s));
    for (auto const& s : b.out) out.push_back(items::toJson(s));
    for (auto const& t : b.tankIn) tin.push_back(fluidJson(t));
    for (auto const& t : b.tankOut) tout.push_back(fluidJson(t));
    j["in"]   = std::move(in);
    j["out"]  = std::move(out);
    j["tin"]  = std::move(tin);
    j["tout"] = std::move(tout);
    if (b.preferred >= 0) j["pref"] = b.preferred;
    if (!b.disabled.empty()) j["disabled"] = b.disabled;
    if (!b.spout.empty()) {
        Json s = Json::array();
        for (auto const& st : b.spout) s.push_back(items::toJson(st));
        j["spout"] = std::move(s);
    }
    if (!b.spoutFluid.empty()) {
        Json s = Json::array();
        for (auto const& f : b.spoutFluid) s.push_back(fluidJson(f));
        j["spoutFluid"] = std::move(s);
    }
    bd::edit(pos)["basin"] = std::move(j);
}

void forget(GlobalPos const& pos) { cache().erase(pos); }
void forgetAll() { cache().clear(); }

bool isEmpty(BasinState const& b) {
    auto none = [](auto const& arr) { return std::all_of(arr.begin(), arr.end(), [](ItemStack const& s) { return items::isEmpty(s); }); };
    auto dry  = [](auto const& t) { return std::all_of(t.begin(), t.end(), [](cb::Fluid const& f) { return f.empty(); }); };
    return none(b.in) && none(b.out) && dry(b.tankIn) && dry(b.tankOut);
}

bool canContinueProcessing(BasinState const& b) { return b.spout.empty() && b.spoutFluid.empty(); }

void notifyChangeOfContents(BasinState& b) { b.contentsChanged = true; }

ItemStack insertInput(BasinState& b, ItemStack const& stack, bool simulate) {
    auto rest = insertInto(b.in, stack, simulate);
    if (!simulate && items::countOf(rest) != items::countOf(stack)) b.contentsChanged = true;
    return rest;
}

std::vector<cb::Fluid*> combinedTanks(BasinState& b) {
    // Create's CombinedTankWrapper(outputTank, inputTank)
    return {&b.tankOut[0], &b.tankOut[1], &b.tankIn[0], &b.tankIn[1]};
}

ItemStack& slot(BasinState& b, int i) { return i < cb::kSlotCount ? b.in[i] : b.out[i - cb::kSlotCount]; }

cb::HeatLevel heatUnder(BlockSource& region, GlobalPos const& pos) {
    auto below = game::blockPos(pos);
    --below.y;
    return common::heat::level(region, below);
}

// ---- outputs ------------------------------------------------------------------------------------------------

namespace {
bool acceptFluidsInto(std::vector<cb::Fluid>& tanks, std::vector<cb::Fluid> const& fluidsOut, bool simulate) {
    for (auto const& f : fluidsOut) {
        // Create's forceFill: output tanks take fluid although they forbid insertion from outside
        if (cb::fillTanks(tanks, f, cb::kTankCapacity, simulate) != f.amount) return false;
    }
    return true;
}
} // namespace

bool acceptOutputs(BlockSource& region, GlobalPos const& pos, BasinState& b, std::vector<ItemStack> const& itemsOut,
                   std::vector<cb::Fluid> const& fluidsOut, bool simulate) {
    auto bp     = game::blockPos(pos);
    int  facing = stateAt(region, bp, kFacingState, kDown);
    if (facing != kDown) {
        auto target   = offsetOf(offsetOf(bp, kDown), facing);
        int  side     = oppositeFace(facing);
        bool inv      = loaded(region, target) && items::has(region, target, side);
        bool tank     = loaded(region, target) && fluids::find({region, target, side}).has_value();
        if (!itemsOut.empty() && !inv) return false;
        if (!fluidsOut.empty() && !tank) {
            // fluid outputs but the target only takes items: they stay in the basin
            if (!acceptFluidsInto(b.tankOut, fluidsOut, simulate)) return false;
        }
        if (simulate) return true;
        for (auto const& s : itemsOut)
            if (!items::isEmpty(s)) b.spout.push_back(s);
        if (tank)
            for (auto const& f : fluidsOut) b.spoutFluid.push_back(f);
        return true;
    }
    for (auto const& s : itemsOut) {
        if (!items::isEmpty(insertInto(b.out, s, simulate))) return false;
    }
    if (fluidsOut.empty()) return true;
    return acceptFluidsInto(b.tankOut, fluidsOut, simulate);
}

bool canOutputTo(BlockSource& region, BlockPos const& basin, int face) {
    auto neighbour = offsetOf(basin, face);
    auto output    = offsetOf(neighbour, kDown);
    if (!loaded(region, neighbour) || !loaded(region, output)) return false;
    auto nid = idAt(region, neighbour);
    if (isFunnel(nid)) {
        // a funnel whose opening faces away from the basin doesn't take its outputs
        if (stateAt(region, neighbour, kFacingState, -1) == face) return false;
    } else if (hasCollision(region, neighbour)) {
        return false;
    } else if (auto belt = belts::beltAt(game::globalPos(region, output))) {
        // a belt below moving towards the basin doesn't take its outputs
        if (belt->speed != 0 && belt->movementFace == oppositeFace(face)) return false;
    }
    // Create: a DirectBeltInputBehaviour below the neighbour (belts, depots, basins, ...)
    return directInputBlock(idAt(region, output)) && items::has(region, output, oppositeFace(face));
}

void updateSpoutput(BlockSource& region, GlobalPos const& pos, BasinState& b) {
    auto bp      = game::blockPos(pos);
    int  current = stateAt(region, bp, kFacingState, kDown);
    int  next    = kDown;
    for (int face : horizontalOrder()) {
        if (canOutputTo(region, bp, face) && std::find(b.disabled.begin(), b.disabled.end(), face) == b.disabled.end()) next = face;
    }
    if (b.preferred >= 2 && canOutputTo(region, bp, b.preferred)) next = b.preferred;
    if (next == current) return;
    setState(region, bp, kFacingState, next);
    log().info("basin at {} now outputs {}", pos.key(), next == kDown ? std::string("into itself") : "to face " + std::to_string(next));
    sync::dirty(pos);
    if (next == kDown) return;
    // the output buffers move to the spout
    for (auto& s : b.out) {
        if (items::isEmpty(s)) continue;
        if (acceptOutputs(region, pos, b, {s}, {}, true)) {
            auto moved = s;
            s          = items::empty();
            acceptOutputs(region, pos, b, {moved}, {}, false);
        }
    }
    for (auto& t : b.tankOut) {
        if (t.empty()) continue;
        if (acceptOutputs(region, pos, b, {}, {t}, true)) {
            auto moved = t;
            t          = {};
            acceptOutputs(region, pos, b, {}, {moved}, false);
        }
    }
    changed(pos, b);
}

void onWrenched(BlockSource& region, GlobalPos const& pos, BasinState& b, int face) {
    int current = stateAt(region, game::blockPos(pos), kFacingState, kDown);
    std::erase(b.disabled, face);
    if (current == face) {
        if (b.preferred == face) b.preferred = -1;
        b.disabled.push_back(face);
    } else {
        b.preferred = face;
    }
    updateSpoutput(region, pos, b);
    save(pos, b);
    playSound(pos.dim, game::blockPos(pos), "random.click", 0.3f, 1.2f);
    log().info("basin at {} wrenched on face {}", pos.key(), face);
}

namespace {
void tryClearingSpoutputOverflow(BlockSource& region, GlobalPos const& pos, BasinState& b) {
    auto bp     = game::blockPos(pos);
    int  facing = stateAt(region, bp, kFacingState, kDown);
    auto target = offsetOf(offsetOf(bp, kDown), facing);
    int  side   = oppositeFace(facing);
    bool inv    = facing != kDown && loaded(region, target) && items::has(region, target, side);
    bool tank   = facing != kDown && loaded(region, target) && fluids::find({region, target, side}).has_value();
    bool update = false;
    for (auto it = b.spout.begin(); it != b.spout.end();) {
        if (facing == kDown) {
            popItem(region, bp.x + .5f, bp.y + .5f, bp.z + .5f, *it);
            it     = b.spout.erase(it);
            update = true;
            continue;
        }
        if (!inv) break;
        auto rest = items::insert(region, target, side, *it, true);
        if (items::countOf(rest) == items::countOf(*it)) {
            ++it;
            continue;
        }
        // the target's filter (not a recipe filter such as another basin's)
        auto targetId = idAt(region, target);
        if (targetId != kBasin && common::filterslot::hasSlots(targetId)
            && !common::filterslot::matches(game::globalPos(pos.dim, target), *it)) {
            ++it;
            continue;
        }
        if (b.visualOut.size() < 3) b.visualOut.emplace_back(*it, 10);
        update = true;
        rest   = items::insert(region, target, side, *it, false);
        if (items::isEmpty(rest)) it = b.spout.erase(it);
        else {
            *it = rest;
            ++it;
        }
    }
    for (auto it = b.spoutFluid.begin(); it != b.spoutFluid.end();) {
        if (facing == kDown) {
            it     = b.spoutFluid.erase(it);
            update = true;
            continue;
        }
        if (!tank) break;
        fluids::FluidStack f{it->id, it->amount};
        if (fluids::fill(region, target, side, f, true) != it->amount) break;
        (void)fluids::fill(region, target, side, f, false);
        it     = b.spoutFluid.erase(it);
        update = true;
    }
    if (update) changed(pos, b);
}

void collectItemEntities(BlockSource& region, GlobalPos const& pos, BasinState& b) {
    auto bp = game::blockPos(pos);
    AABB box{
        Vec3{float(bp.x), float(bp.y), float(bp.z)},
        Vec3{float(bp.x + 1), float(bp.y + 1.5f), float(bp.z + 1)}
    };
    bool any = false;
    // Create's updateEntityAfterFallOn takes the items that fell in; Bedrock pushes an item entity
    // out of the basin's cell as soon as it gets in, so items are taken while they fall into it
    // (up to half a block above the rim)
    for (auto* actor : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!actor || !actor->isAlive()) continue;
        auto& itemActor = static_cast<ItemActor&>(*actor);
        auto& stack     = itemActor.item();
        if (items::isEmpty(stack)) continue;
        auto rest = insertInto(b.in, stack, false);
        if (items::countOf(rest) == items::countOf(stack)) continue;
        any = true;
        log().info("basin at {} took {} x{}", pos.key(), items::idOf(stack), items::countOf(stack) - items::countOf(rest));
        if (items::isEmpty(rest)) actor->remove();
        else {
            // the rest as a fresh entity so clients see the new count
            Vec3 at = actor->getPosition();
            actor->remove();
            popItem(region, at.x, at.y, at.z, rest);
        }
    }
    if (any) changed(pos, b);
}
} // namespace

void wakeOperator(GlobalPos const& basinPos) {
    GlobalPos above{basinPos.dim, basinPos.x, basinPos.y + 2, basinPos.z};
    auto      it = tracked().find(above);
    if (it == tracked().end()) return;
    if (it->second != Kind::Press && it->second != Kind::Mixer) return;
    if (auto* s = operators::get(above, it->second == Kind::Press)) s->basinCheck = true;
}

void tick(BlockSource& region, GlobalPos const& pos, BasinState& b) {
    auto const& cfg = config::get<BasinConfig>();
    // lazyTick (every 10 ticks): spout direction, a backup recipe check every 200 ticks
    if (--b.lazy <= 0) {
        b.lazy = 10;
        updateSpoutput(region, pos, b);
        if (b.recipeBackupCheck-- <= 0) {
            b.recipeBackupCheck = 20;
            if (!isEmpty(b)) notifyChangeOfContents(b);
        }
    }
    if (!b.spout.empty() || !b.spoutFluid.empty()) tryClearingSpoutputOverflow(region, pos, b);
    if (--b.intakeCooldown <= 0) {
        b.intakeCooldown = std::max(1, cfg.basinIntakeInterval);
        collectItemEntities(region, pos, b);
    }
    for (auto& v : b.visualOut) --v.second;
    std::erase_if(b.visualOut, [](auto const& v) { return v.second <= 0; });
    if (!b.contentsChanged) return;
    b.contentsChanged = false;
    wakeOperator(pos);
    // basins that spout into this one look again too
    auto bp = game::blockPos(pos);
    for (int face : kHorizontals) {
        auto up = offsetOf(offsetOf(bp, kUp), face);
        if (idAt(region, up) != kBasin || stateAt(region, up, kFacingState, kDown) != oppositeFace(face)) continue;
        if (auto* other = get(region, game::globalPos(pos.dim, up))) other->contentsChanged = true;
    }
}

// ---- player use ---------------------------------------------------------------------------------------------

bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held, int /*face*/) {
    auto* b = get(region, pos);
    if (!b) return false;
    auto bp       = game::blockPos(pos);
    bool creative = player.isCreative();
    if (!items::isEmpty(held)) {
        auto id = items::idOf(held);
        // FluidHelper.tryEmptyItemIntoBE
        if (auto f = fluids::fluidInItem(held); !f.empty()) {
            cb::Fluid fluid{f.fluid, f.amount};
            if (cb::fillTanks(b->tankIn, fluid, cb::kTankCapacity, true) != fluid.amount) return true;
            cb::fillTanks(b->tankIn, fluid, cb::kTankCapacity, false);
            if (!creative) {
                auto emptyId = emptyContainerOf(id);
                auto rest    = items::withCount(held, items::countOf(held) - 1);
                auto emptied = emptyId.empty() ? std::nullopt : items::make(emptyId);
                if (items::isEmpty(rest)) player.setSelectedItem(emptied ? *emptied : items::empty());
                else {
                    player.setSelectedItem(rest);
                    if (emptied) giveOrDrop(player, *emptied);
                }
            }
            playSound(pos.dim, bp, fluidSound(fluid.id, false), 1.f, 1.f);
            log().info("basin at {}: {} poured {} mB of {}", pos.key(), player.getRealName(), fluid.amount, fluid.id);
            changed(pos, *b);
            return true;
        }
        // FluidHelper.tryFillItemFromBE
        if (id == "minecraft:bucket" || id == "minecraft:glass_bottle") {
            for (auto* t : combinedTanks(*b)) {
                if (t->empty()) continue;
                auto fill = fillingFor(id, t->id);
                if (fill.id.empty() || fill.amount > t->amount) continue;
                auto filled = items::make(fill.id, 1, fill.aux);
                if (!filled) continue;
                std::string fluid = t->id;
                t->amount -= fill.amount;
                if (t->amount <= 0) *t = {};
                if (!creative) {
                    auto rest = items::withCount(held, items::countOf(held) - 1);
                    if (items::isEmpty(rest)) player.setSelectedItem(*filled);
                    else {
                        player.setSelectedItem(rest);
                        giveOrDrop(player, *filled);
                    }
                }
                playSound(pos.dim, bp, fluidSound(fluid, true), 1.f, 1.f);
                log().info("basin at {}: {} took {} mB of {}", pos.key(), player.getRealName(), fill.amount, fluid);
                changed(pos, *b);
                return true;
            }
            return true; // Create: a container that could be filled never places anything
        }
        if (id == "minecraft:sponge") {
            std::string fluid;
            for (auto* t : combinedTanks(*b))
                if (!t->empty()) {
                    fluid = t->id;
                    break;
                }
            if (fluid.empty()) return false;
            for (auto* t : combinedTanks(*b))
                if (t->id == fluid) *t = {};
            log().info("basin at {}: {} soaked up the {}", pos.key(), player.getRealName(), fluid);
            changed(pos, *b);
            return true;
        }
        return false;
    }
    // empty hand: take every item
    bool success = false;
    for (int i = 0; i < 2 * cb::kSlotCount; ++i) {
        auto& s = slot(*b, i);
        if (items::isEmpty(s)) continue;
        giveOrDrop(player, s);
        s       = items::empty();
        success = true;
    }
    if (success) {
        playSound(pos.dim, bp, "random.pop", .2f, 1.f + random01());
        log().info("basin at {} emptied by {}", pos.key(), player.getRealName());
    }
    // Create's onEmptied: the operator above stops
    GlobalPos above{pos.dim, pos.x, pos.y + 2, pos.z};
    if (auto it = tracked().find(above); it != tracked().end() && (it->second == Kind::Press || it->second == Kind::Mixer)) {
        if (auto* s = operators::get(above, it->second == Kind::Press)) s->basinRemoved = true;
    }
    changed(pos, *b);
    return true;
}

void dropContents(BlockSource& region, GlobalPos const& pos, Json const& doc) {
    auto b  = load(&doc);
    auto bp = game::blockPos(pos);
    int  n  = 0;
    for (auto const& s : b.in) {
        if (items::isEmpty(s)) continue;
        popItem(region, bp.x + .5f, bp.y + .5f, bp.z + .5f, s);
        ++n;
    }
    for (auto const& s : b.out) {
        if (items::isEmpty(s)) continue;
        popItem(region, bp.x + .5f, bp.y + .5f, bp.z + .5f, s);
        ++n;
    }
    for (auto const& s : b.spout) {
        popItem(region, bp.x + .5f, bp.y + .5f, bp.z + .5f, s);
        ++n;
    }
    if (n) log().info("basin at {} broke: dropped {} stacks", pos.key(), n);
}

Json renderJson(BasinState const& b) {
    Json itemsJ = Json::array();
    for (auto const* arr : {&b.in, &b.out})
        for (auto const& s : *arr)
            if (!items::isEmpty(s)) itemsJ.push_back({items::idOf(s), items::countOf(s), static_cast<int>(s.getAuxValue())});
    Json fluidsJ = Json::array();
    for (auto const* tanks : {&b.tankIn, &b.tankOut})
        for (auto const& t : *tanks)
            if (!t.empty()) fluidsJ.push_back({t.id, t.amount});
    Json out = Json::array();
    for (auto const& [s, t] : b.visualOut) out.push_back(items::idOf(s));
    Json stacks = Json::array(); // full stacks for the client mod's item renderer (U33)
    for (auto const* arr : {&b.in, &b.out})
        for (auto const& s : *arr)
            if (!items::isEmpty(s)) stacks.push_back(items::toJson(s));
    return {{"items", itemsJ}, {"fluids", fluidsJ}, {"spouting", out}, {"stacks", stacks}};
}

// ---- capabilities ---------------------------------------------------------------------------------------------

void registerCapabilities() {
    items::Handler h;
    h.slots   = [](common::Access const&) { return 2 * cb::kSlotCount; };
    h.getSlot = [](common::Access const& a, int i) -> ItemStack {
        auto* b = get(a.region, game::globalPos(a.region, a.pos));
        if (!b || i < 0 || i >= 2 * cb::kSlotCount) return items::empty();
        return slot(*b, i);
    };
    h.insert = [](common::Access const& a, ItemStack const& stack, bool simulate) -> ItemStack {
        auto gp = game::globalPos(a.region, a.pos);
        auto* b = get(a.region, gp);
        if (!b) return stack;
        auto rest = insertInput(*b, stack, simulate);
        if (!simulate && items::countOf(rest) != items::countOf(stack)) changed(gp, *b);
        return rest;
    };
    h.extract = [](common::Access const& a, items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto gp = game::globalPos(a.region, a.pos);
        auto* b = get(a.region, gp);
        if (!b || amount <= 0) return items::empty();
        ItemStack kind;
        int       got = 0;
        for (int i = 0; i < 2 * cb::kSlotCount && got < amount; ++i) {
            auto& s = slot(*b, i);
            if (items::isEmpty(s)) continue;
            if (items::isEmpty(kind)) {
                if (filter && !filter(s)) continue;
                kind = s;
            } else if (!items::sameItemSameData(kind, s)) {
                continue;
            }
            int take = std::min(amount - got, items::countOf(s));
            got += take;
            if (!simulate) s = items::withCount(s, items::countOf(s) - take);
        }
        if (got == 0) return items::empty();
        if (!simulate) changed(gp, *b);
        return items::withCount(kind, got);
    };
    items::registerBlock(kBasin, std::move(h));

    fluids::Handler f;
    f.tanks = [](common::Access const& a) {
        std::vector<fluids::Tank> out;
        auto* b = get(a.region, game::globalPos(a.region, a.pos));
        if (!b) return out;
        for (auto* t : combinedTanks(*b)) out.push_back({{t->id, t->amount}, cb::kTankCapacity});
        return out;
    };
    f.fill = [](common::Access const& a, fluids::FluidStack const& stack, bool simulate) {
        auto gp = game::globalPos(a.region, a.pos);
        auto* b = get(a.region, gp);
        if (!b || stack.empty()) return 0;
        int n = cb::fillTanks(b->tankIn, {stack.fluid, stack.amount}, cb::kTankCapacity, simulate);
        if (!simulate && n > 0) changed(gp, *b);
        return n;
    };
    f.drain = [](common::Access const& a, fluids::FluidStack const& request, bool simulate) -> fluids::FluidStack {
        auto gp = game::globalPos(a.region, a.pos);
        auto* b = get(a.region, gp);
        if (!b || request.amount <= 0) return {};
        std::string fluid = request.fluid;
        if (fluid.empty()) {
            for (auto* t : combinedTanks(*b))
                if (!t->empty()) {
                    fluid = t->id;
                    break;
                }
        }
        if (fluid.empty()) return {};
        int got = 0;
        for (auto* t : combinedTanks(*b)) {
            if (t->empty() || t->id != fluid || got >= request.amount) continue;
            int n = std::min(request.amount - got, t->amount);
            got += n;
            if (!simulate) {
                t->amount -= n;
                if (t->amount <= 0) *t = {};
            }
        }
        if (got == 0) return {};
        if (!simulate) changed(gp, *b);
        return {fluid, got};
    };
    fluids::registerBlock(kBasin, std::move(f));
}

} // namespace lv::feature::basin::basinops
