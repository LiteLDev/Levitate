// Item drain (Create's ItemDrainBlockEntity): items cross the drain (from belts, funnels, chutes,
// thrown on top or inserted through the item capability); at the centre an item that holds a
// fluid is emptied into the 1500 mB tank (20 ticks), then the item moves on and leaves on the far
// side (into the next Levitate block, or as an item entity when that side is open). Players
// exchange fluid by right-clicking with containers and take the held item with an empty hand.
// Pipes extract from the tank on every side; nothing is inserted from outside.

#include <algorithm>
#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/Items.h"
#include "feature/fluids/FluidsInternal.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace items = common::items;
namespace cfl   = common::fluids;
namespace bd    = common::blockdata;

namespace {

struct Held {
    ItemStack     stack;
    cf::DrainItem move;
};

std::optional<Held> heldOf(Json const* d) {
    if (!d) return std::nullopt;
    auto it = d->find("held");
    if (it == d->end() || !it->is_object()) return std::nullopt;
    Held h;
    h.stack = items::fromJson(it->value("item", Json()));
    if (items::isEmpty(h.stack)) return std::nullopt;
    h.move.beltPosition     = it->value("pos", 0.f);
    h.move.prevBeltPosition = it->value("prev", h.move.beltPosition);
    h.move.travel           = it->value("travel", 0);
    h.move.checked          = it->value("checked", false);
    return h;
}

void setHeld(Json& d, std::optional<Held> const& h) {
    if (!h || items::isEmpty(h->stack)) {
        d.erase("held");
        d.erase("processing");
        return;
    }
    d["held"] = {
        {"item",    items::toJson(h->stack)  },
        {"pos",     h->move.beltPosition     },
        {"prev",    h->move.prevBeltPosition },
        {"travel",  h->move.travel           },
        {"checked", h->move.checked          },
    };
}

/// Create's ItemDrainItemHandler / tryInsertingFromSide: one item at a time when it can be
/// emptied, otherwise the whole stack; only while the drain holds nothing.
ItemStack insertInto(GlobalPos const& p, ItemStack const& stack, int fromFace, bool simulate) {
    if (items::isEmpty(stack)) return stack;
    if (heldOf(dataAt(p))) return stack;
    ItemStack inserted = stack, rest = items::empty();
    if (items::countOf(stack) > 1 && canBeEmptied(stack)) {
        rest     = items::withCount(stack, items::countOf(stack) - 1);
        inserted = items::withCount(stack, 1);
    }
    if (!simulate) {
        Held h{inserted, cf::insertDrainItem(fromFace)};
        setHeld(editData(p), h);
    }
    return rest;
}

/// The face an item entity moving by (mx, mz) travels towards (Create's Direction.getNearest of the
/// normalised horizontal motion; north for no horizontal motion).
int fallenTravel(double mx, double mz) {
    double len = std::sqrt(mx * mx + mz * mz);
    if (len < 1e-4) return 2;
    mx /= len;
    mz /= len;
    // Direction order: down, up, north, south, west, east; the first strictly best one wins
    static constexpr int    faces[]{2, 3, 4, 5};
    static constexpr double fx[]{0, 0, -1, 1}, fz[]{-1, 1, 0, 0};
    int    best  = 2;
    double score = 1.4e-45; // Float.MIN_VALUE: a zero product never wins
    for (int i = 0; i < 4; ++i) {
        double d = fx[i] * mx + fz[i] * mz;
        if (d > score) {
            score = d;
            best  = faces[i];
        }
    }
    return best;
}

/// Accepts item entities that land on the drain (Create's updateEntityAfterFallOn).
void collectFallen(BlockSource& region, GlobalPos const& p) {
    AABB box{
        Vec3{float(p.x), float(p.y) + 0.5f,  float(p.z)},
        Vec3{float(p.x + 1), float(p.y) + 1.25f, float(p.z + 1)}
    };
    for (auto* a : region.getEntities(box, 0.0f)) {
        if (!a || !a->isAlive() || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto& ia    = static_cast<ItemActor&>(*a);
        auto  stack = ia.item();
        if (items::isEmpty(stack)) continue;
        // Create: inserted from the horizontal direction nearest the item's motion (Direction.getNearest
        // of the normalised horizontal motion; north when it lies still), so it crosses the drain
        auto rest = insertInto(p, stack, cf::oppositeFace(fallenTravel(a->getPosDelta().x, a->getPosDelta().z)), false);
        if (items::countOf(rest) == items::countOf(stack)) return;
        if (items::isEmpty(rest)) a->remove();
        else ia.item() = rest;
        return;
    }
}

/// Create's ItemDrainRenderer draws the held item crossing the drain, 15/16 up, from the side it came
/// in towards the side it leaves (`travel`). Companions get the client mod's display item, vanilla
/// players a client-only item entity (the chute's display, U12).
void showHeld(GlobalPos const& p, std::optional<Held> const& held) {
    if (!held || items::isEmpty(held->stack)) {
        funnels::detail::hideItem(p);
        return;
    }
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dz[] = {0, 0, -1, 1, 0, 0};
    int    side   = std::clamp(held->move.travel, 0, 5);
    double along  = static_cast<double>(held->move.beltPosition) - .5;
    double x      = p.x + .5 + dx[side] * along, z = p.z + .5 + dz[side] * along;
    funnels::detail::showItem(p, held->stack, Vec3{static_cast<float>(x), static_cast<float>(p.y + 15 / 16.), static_cast<float>(z)});
}

void output(BlockSource& region, GlobalPos const& p, Held& held) {
    int       side = held.move.travel;
    GlobalPos next = offsetPos(p, side);
    auto      id   = blockIdAt(next);
    common::Access acc{region, toBlockPos(next), cf::oppositeFace(side)};
    bool direct = belts::hasDirectInput(next);
    if (direct || (id.starts_with("levitate:") && items::find(acc))) {
        // Create's DirectBeltInputBehaviour of belts, depots, chutes, funnels, drains, basins, ...
        ItemStack rest;
        if (direct) {
            belts::TransportedItem t;
            t.stack        = held.stack;
            t.insertedFrom = side;
            rest           = belts::insertDirect(next, t, side, false);
        } else {
            rest = items::insert(region, toBlockPos(next), cf::oppositeFace(side), held.stack, false);
        }
        if (items::countOf(rest) == items::countOf(held.stack)) return;
        log().info("item drain at {} {} {} passed {} on", p.x, p.y, p.z, items::idOf(held.stack));
        if (items::isEmpty(rest) && id == "levitate:item_drain") advancements::awardOwner(p, "chained_drain");
        // the insertion may have touched other BlockData documents: look ours up again
        if (items::isEmpty(rest)) setHeld(editData(p), std::nullopt);
        else {
            held.stack = rest;
            setHeld(editData(p), held);
        }
        return;
    }
    if (solidAt(next)) return; // wait
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    double   speed = cf::kDrainItemSpeed;
    double   ox = p.x + .5 + dx[side] * .75, oy = p.y + .5 + dy[side] * .75, oz = p.z + .5 + dz[side] * .75;
    double   mx = dx[side] * speed, my = dy[side] * speed + 1 / 8., mz = dz[side] * speed;
    spawnItem(p.dim, ox, oy + 6 / 16., oz, held.stack, mx, my, mz);
    log().info("item drain at {} {} {} ejected {}", p.x, p.y, p.z, items::idOf(held.stack));
    setHeld(editData(p), std::nullopt);
}

} // namespace

namespace {

/// One tick of the held item: emptying at the centre, moving, leaving at the far side.
void moveHeld(BlockSource* region, GlobalPos const& p, std::optional<Held>& held) {
    auto& d          = editData(p);
    int   processing = d.value("processing", 0);
    if (processing > 0) {
        --processing;
        d["processing"] = processing;
        // Create's continueProcessing
        if (processing < 5) return;
        auto e = emptyItem(held->stack);
        if (!e) {
            d["processing"] = 0;
            return;
        }
        FluidStack tank = tankOf(d);
        if (processing > 5) {
            if (tankFill(tank, cf::kDrainCapacity, e->fluid, true) != e->fluid.amount) d["processing"] = cf::kDrainTime;
            return;
        }
        if (tankFill(tank, cf::kDrainCapacity, e->fluid, true) != e->fluid.amount) {
            d["processing"] = cf::kDrainTime;
            return;
        }
        tankFill(tank, cf::kDrainCapacity, e->fluid, false);
        setTank(d, tank);
        log().info("item drain at {} {} {} emptied {} ({} mB {})", p.x, p.y, p.z, items::idOf(held->stack), e->fluid.amount, e->fluid.fluid);
        held->stack = e->container;
        setHeld(d, held);
        playSound(p, "bucket.empty_water", 0.5f, 1.f);
        advancements::awardOwner(p, "drain");
        return;
    }
    auto step = cf::stepDrainItem(held->move);
    if (step == cf::DrainStep::ReachedCentre && canBeEmptied(held->stack)) {
        d["processing"] = cf::kDrainTime;
        setHeld(d, held);
        return;
    }
    if (step == cf::DrainStep::AtOutput) {
        setHeld(d, held);
        output(*region, p, *held);
        return;
    }
    setHeld(d, held);
}

} // namespace

void tickDrain(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return;
    auto held = heldOf(dataAt(p));
    if (!held) {
        funnels::detail::hideItem(p);
        collectFallen(*region, p);
        if (auto now = heldOf(dataAt(p))) showHeld(p, now);
        return;
    }
    moveHeld(region, p, held);
    // shown after this tick's move (the display follows the item, interpolated by the clients)
    showHeld(p, heldOf(dataAt(p)));
}

bool useOnDrain(Player& player, GlobalPos const& p, ItemStack const& handItem) {
    auto* region = regionOf(p.dim);
    if (!region) return false;
    // block items without a fluid are placed as usual (Create: PASS)
    if (!items::isEmpty(handItem) && handItem.mBlock && !canBeEmptied(handItem)) return false;
    auto& d = editData(p);
    if (!items::isEmpty(handItem)) {
        FluidStack tank = tankOf(d);
        if (exchangeWithTank(player, tank, cf::kDrainCapacity, handItem, true)) {
            setTank(d, tank);
            log().info("item drain at {} {} {} exchanged fluid with {}", p.x, p.y, p.z, player.getRealName());
            playSound(p, "bucket.fill_water", 0.6f, 1.f);
            return true;
        }
    }
    if (auto held = heldOf(&d)) {
        giveOrDrop(player, held->stack);
        setHeld(d, std::nullopt);
        log().info("item drain at {} {} {} gave {} back to {}", p.x, p.y, p.z, items::idOf(held->stack), player.getRealName());
    }
    return true;
}

void registerDrain() {
    // items: one slot, the held item
    items::Handler ih;
    ih.slots   = [](common::Access const&) { return 1; };
    ih.getSlot = [](common::Access const& at, int slot) -> ItemStack {
        auto h = slot == 0 ? heldOf(dataAt(game::globalPos(at.region, at.pos))) : std::nullopt;
        return h ? h->stack : items::empty();
    };
    // Create: the item handler exists on the horizontal sides only
    ih.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) {
        if (at.face == 0 || at.face == 1) return stack;
        return insertInto(game::globalPos(at.region, at.pos), stack, at.face, simulate);
    };
    ih.extract = [](common::Access const& at, items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto p = game::globalPos(at.region, at.pos);
        auto h = heldOf(dataAt(p));
        if (at.face == 0 || at.face == 1) return items::empty(); // horizontal sides only
        if (!h || amount <= 0 || (filter && !filter(h->stack))) return items::empty();
        int  n   = std::min(amount, items::countOf(h->stack));
        auto out = items::withCount(h->stack, n);
        if (!simulate) {
            auto& d = editData(p);
            if (n >= items::countOf(h->stack)) setHeld(d, std::nullopt);
            else {
                h->stack = items::withCount(h->stack, items::countOf(h->stack) - n);
                setHeld(d, h);
            }
        }
        return out;
    };
    items::registerBlock(kItemDrain, ih);
    // Create's ItemDrainBlockEntity has a DirectBeltInputBehaviour: belt ends, saws, chutes, ...
    // hand their items straight in (an item travelling `side` enters through the opposite face)
    belts::registerDirectInput(kItemDrain, belts::DirectInput{
        .canInsertFromSide = [](GlobalPos const&, int side) { return side != 1; },
        .insert = [](GlobalPos const& p, belts::TransportedItem const& item, int side, bool simulate) {
            return insertInto(p, item.stack, side >= 0 && side <= 5 ? side ^ 1 : 1, simulate);
        },
    });

    // fluids: the tank, extraction only
    cfl::Handler fh;
    fh.tanks = [](common::Access const& at) {
        auto const* d = dataAt(game::globalPos(at.region, at.pos));
        return std::vector<cfl::Tank>{{d ? tankOf(*d) : FluidStack{}, cf::kDrainCapacity}};
    };
    fh.fill  = [](common::Access const&, FluidStack const&, bool) { return 0; };
    fh.drain = [](common::Access const& at, FluidStack const& request, bool simulate) {
        if (at.face == 1) return FluidStack{}; // Create: no fluid handler on the top
        auto p    = game::globalPos(at.region, at.pos);
        auto const* d = dataAt(p);
        if (!d) return FluidStack{};
        auto tank = tankOf(*d);
        auto out  = tankDrain(tank, request, simulate);
        if (!simulate && !out.empty()) setTank(editData(p), tank);
        return out;
    };
    cfl::registerBlock(kItemDrain, fh);

    // the held item drops when the drain is broken (Create's onRemove)
    bd::onErase([](GlobalPos const& p, Json const& doc, std::string const& prev) {
        if (prev != kItemDrain) return;
        funnels::detail::hideItem(p);
        auto it = doc.find(kDataKey);
        if (it == doc.end()) return;
        if (auto h = heldOf(&*it)) spawnItem(p.dim, p.x + .5, p.y + .5, p.z + .5, h->stack);
    });
}

} // namespace lv::feature::fluids
