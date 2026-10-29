// Andesite and brass tunnels (Create's BeltTunnelBlock / BrassTunnelBlockEntity, SPEC §3.6 U12).
//
// A tunnel sits on a belt (the segment below it, beltlink in FunnelsExt.h). Belt items crossing
// into its segment are reported by the belt (beltlink::itemEntering): an andesite tunnel with
// side outputs splits one item off to each side; a brass tunnel with more than one exit takes
// the stack and distributes it among the exits of its group (brass tunnels on parallel belts)
// with the selected distribution mode. Side filters sort items; items can also be inserted into
// a brass tunnel directly (item capability).

#include <algorithm>
#include <random>
#include <unordered_map>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::funnels::detail {

namespace items    = common::items;
namespace beltlink = common::beltlink;

namespace {

struct Runtime {
    bool      loaded = false;
    ItemStack stack;
    int       enteredFrom   = -1;
    int       progress      = -1;
    int       previousIndex = 0;
    bool      newItem       = false;
    bool      synced        = false;
};
std::unordered_map<std::string, Runtime>& runtimes() {
    static std::unordered_map<std::string, Runtime> m;
    return m;
}

/// A new world (the client loads several in one process) starts without cached state.
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldClosed([] { runtimes().clear(); });
    }
} const gLifecycle;

Runtime& rt(GlobalPos const& pos) {
    auto& r = runtimes()[pos.key()];
    if (!r.loaded) {
        r.loaded = true;
        r.stack  = items::empty();
        if (auto const* d = docOf(pos)) {
            if (d->contains("stack")) r.stack = items::fromJson((*d)["stack"]);
            r.enteredFrom   = d->value("from", -1);
            r.previousIndex = d->value("index", 0);
            r.synced        = d->value("synced", false);
        }
    }
    return r;
}

void saveRt(GlobalPos const& pos, Runtime const& r) {
    auto& d = editDoc(pos);
    if (items::isEmpty(r.stack)) d.erase("stack");
    else d["stack"] = items::toJson(r.stack);
    d["from"]   = r.enteredFrom;
    d["index"]  = r.previousIndex;
    d["synced"] = r.synced;
}

std::mt19937& rng() {
    static std::mt19937 g{std::random_device{}()};
    return g;
}

Block const* tunnelBlock(BlockSource& region, BlockPos const& p) {
    auto const* b = loadedBlock(region, p);
    return b && isTunnel(kindOf(b->getTypeName())) ? b : nullptr;
}
bool isBrass(Block const& b) { return b.getTypeName() == kBrassTunnel; }

int  axisOf(Block const& b) { return state(b, kAxis, 0) == 2 ? 2 : 0; }
auto shapeOf(Block const& b) { return static_cast<cf::TunnelShape>(std::clamp(state(b, kShape, 0), 0, 5)); }

std::vector<int> sidesOf(Block const& b) { return cf::tunnelSides(axisOf(b), shapeOf(b)); }

bool contains(std::vector<int> const& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

/// Create's flaps: open sides not covered by another tunnel.
std::vector<int> flapsOf(BlockSource& region, BlockPos const& p, Block const& b) {
    std::vector<int> out;
    for (int side : sidesOf(b))
        if (!tunnelBlock(region, offset(p, side))) out.push_back(side);
    return out;
}

/// Create's getAdjacent: the brass tunnel next to this one (left = counter-clockwise of the
/// positive axis direction) on the same axis.
std::optional<BlockPos> adjacent(BlockSource& region, BlockPos const& p, Block const& b, bool left) {
    if (!isBrass(b)) return std::nullopt;
    int      base = cf::positiveFace(axisOf(b));
    BlockPos n    = offset(p, left ? cf::counterClockWise(base) : cf::clockWise(base));
    auto const* nb = tunnelBlock(region, n);
    if (!nb || !isBrass(*nb) || axisOf(*nb) != axisOf(b)) return std::nullopt;
    return n;
}

bool connected(BlockSource& region, BlockPos const& p, Block const& b, bool left) {
    if (flapsOf(region, p, b).empty()) return false;
    auto n = adjacent(region, p, b, left);
    if (!n) return false;
    return !flapsOf(region, *n, *tunnelBlock(region, *n)).empty();
}

/// Create's hasDistributionBehaviour.
bool distributes(BlockSource& region, BlockPos const& p, Block const& b) {
    auto flaps = flapsOf(region, p, b);
    if (flaps.empty()) return false;
    if (connected(region, p, b, true) || connected(region, p, b, false)) return true;
    if (!isBrass(b)) return false;
    for (int f : flaps)
        if (cf::axisOf(f) != axisOf(b)) return true;
    return false;
}

/// The whole group: this tunnel, then every tunnel to the left, then to the right (Create's order).
std::vector<BlockPos> groupOf(BlockSource& region, BlockPos const& p) {
    std::vector<BlockPos> out{p};
    for (bool left : {true, false}) {
        BlockPos cur = p;
        for (int i = 0; i < 64; ++i) {
            auto const* cb = tunnelBlock(region, cur);
            if (!cb) break;
            auto n = adjacent(region, cur, *cb, left);
            if (!n) break;
            out.push_back(*n);
            cur = *n;
        }
    }
    return out;
}

std::string sideSlot(int side) { return "side_" + std::to_string(side); }

/// Create's testFlapFilter: the side's filter, else a filter of the block on that side.
bool testFlapFilter(BlockSource& region, GlobalPos const& pos, int side, ItemStack const& stack) {
    auto own = common::filterslot::get(pos, sideSlot(side));
    if (!items::isEmpty(own)) return common::filterslot::matches(pos, stack, sideSlot(side));
    auto n  = offset(pos, side);
    auto id = idAt(region, toBlockPos(n));
    if (!id.empty() && common::filterslot::hasSlots(id)) return common::filterslot::matches(n, stack, "filter");
    return true;
}

bool flapFilterEmpty(BlockSource& region, GlobalPos const& pos, int side) {
    if (!items::isEmpty(common::filterslot::get(pos, sideSlot(side)))) return false;
    auto n  = offset(pos, side);
    auto id = idAt(region, toBlockPos(n));
    if (!id.empty() && common::filterslot::hasSlots(id)) return items::isEmpty(common::filterslot::get(n, "filter"));
    return true;
}

/// Something that takes items from the side like Create's DirectBeltInputBehaviour: a belt, or
/// one of our own blocks / another Levitate machine with an item capability.
bool directInput(BlockSource& region, BlockPos const& p, int fromSide) {
    if (beltlink::segmentAt(region, p)) return true;
    auto id = idAt(region, p);
    return id.starts_with("levitate:") && common::items::has(region, p, cf::oppositeFace(fromSide));
}

/// Create's insertIntoTunnel: nullopt when `side` of `tunnel` is no output for `stack`.
std::optional<ItemStack> insertIntoTunnel(BlockSource& region, GlobalPos const& tunnel, int side, ItemStack const& stack, bool simulate) {
    if (items::isEmpty(stack)) return stack;
    if (!testFlapFilter(region, tunnel, side, stack)) return std::nullopt;
    BlockPos below   = offset(toBlockPos(tunnel), cf::kDown);
    auto     segment = beltlink::segmentAt(region, below);
    if (!segment) return std::nullopt;
    BlockPos out = offset(below, side);
    if (directInput(region, out, side)) {
        auto rest = common::items::insert(region, out, cf::oppositeFace(side), stack, simulate);
        return rest;
    }
    if (side == segment->movementFacing && !solidFace(region, out, cf::oppositeFace(side))) {
        if (!simulate) {
            auto  n     = cf::faceOffset(side);
            float speed = std::max(std::abs(segment->speed) / 480.f, 1 / 8.f);
            Vec3  at{below.x + .5f + n[0] * .5f, below.y + 1.f + 6 / 16.f, below.z + .5f + n[2] * .5f};
            spawnItem(region, stack, at, Vec3{n[0] * speed, 1 / 8.f, n[2] * speed});
        }
        return items::empty();
    }
    return std::nullopt;
}

/// Create's addValidOutputsOf / gatherValidOutputs.
std::vector<std::pair<BlockPos, int>> validOutputs(BlockSource& region, BlockPos const& self, std::vector<BlockPos> const& group) {
    std::vector<std::pair<BlockPos, int>> out;
    for (auto const& t : group) {
        auto const* tb = tunnelBlock(region, t);
        if (!tb || !isBrass(*tb)) continue;
        auto segment = beltlink::segmentAt(region, offset(t, cf::kDown));
        if (!segment) continue;
        int  movement   = segment->movementFacing;
        auto sides      = sidesOf(*tb);
        bool prioritize = t == self;
        for (bool sidePass : {true, false}) {
            if (!prioritize && sidePass) continue;
            for (int dir : cf::kHorizontals) {
                if (dir == movement && segment->speed == 0) continue;
                if (prioritize && sidePass == (cf::axisOf(dir) == cf::axisOf(movement))) continue;
                if (dir == cf::oppositeFace(movement)) continue;
                if (!contains(sides, dir)) continue;
                BlockPos o = offset(offset(t, cf::kDown), dir);
                if (!directInput(region, o, dir)) {
                    if (dir == movement && !solidFace(region, o, cf::oppositeFace(dir))) out.emplace_back(t, dir);
                    continue;
                }
                out.emplace_back(t, dir);
            }
        }
    }
    return out;
}

cf::SelectionMode modeOf(GlobalPos const& pos) {
    int m = 0;
    if (auto const* d = docOf(pos)) m = d->value("mode", 0);
    return static_cast<cf::SelectionMode>(std::clamp(m, 0, cf::kSelectionModes - 1));
}

void setStack(GlobalPos const& pos, Runtime& r, ItemStack const& stack, int enteredFrom) {
    r.stack       = items::isEmpty(stack) ? items::empty() : stack;
    r.enteredFrom = enteredFrom;
    r.progress    = -1;
    if (!items::isEmpty(stack)) r.newItem = true;
    saveRt(pos, r);
}

bool canTakeItems(Runtime const& r) { return items::isEmpty(r.stack) && !r.synced; }

/// Create's canInsert (inbound side filters, one stack at a time).
bool canInsert(BlockSource& region, GlobalPos const& pos, Block const& b, int side, ItemStack const& stack) {
    if (contains(sidesOf(b), side)) {
        auto f = common::filterslot::get(pos, sideSlot(side));
        if (!items::isEmpty(f) && !common::filterslot::matches(pos, stack, sideSlot(side))) return false;
    }
    if (!distributes(region, toBlockPos(pos), b)) return true;
    return items::isEmpty(rt(pos).stack);
}

/// Create's BrassTunnelBlockEntity.distribute over one list of targets.
void distribute(BlockSource& region, GlobalPos const& pos, Runtime& r, std::vector<std::pair<BlockPos, int>> const& targets) {
    int n = static_cast<int>(targets.size());
    if (n == 0 || items::isEmpty(r.stack)) return;
    auto mode  = modeOf(pos);
    int  start = cf::distributionStart(mode, r.previousIndex, n, rng()());
    auto probe = [&](int t, int count) -> std::optional<int> {
        auto tpos = toGlobal(pos.dim, targets[static_cast<std::size_t>(t)].first);
        auto rest = insertIntoTunnel(region, tpos, targets[static_cast<std::size_t>(t)].second, items::withCount(r.stack, count), true);
        if (!rest) return std::nullopt;
        return items::countOf(*rest);
    };
    auto plan = cf::planDistribution(mode, items::countOf(r.stack), n, start, probe);
    int  kept = plan.kept;
    for (auto const& [t, count] : plan.give) {
        auto tpos = toGlobal(pos.dim, targets[static_cast<std::size_t>(t)].first);
        auto rest = insertIntoTunnel(region, tpos, targets[static_cast<std::size_t>(t)].second, items::withCount(r.stack, count), false);
        kept += rest ? items::countOf(*rest) : count;
    }
    auto stack = items::withCount(r.stack, kept);
    r.stack    = stack;
    if (items::isEmpty(stack)) r.enteredFrom = -1;
    r.previousIndex = (r.previousIndex + 1) % n;
    saveRt(pos, r);
}

std::optional<std::array<double, 3>> turned(int side, double x, double y, double z) {
    switch (side) {
    case cf::kNorth: return std::array<double, 3>{1 - x, y, 1 - z};
    case cf::kEast: return std::array<double, 3>{z, y, 1 - x};
    case cf::kWest: return std::array<double, 3>{1 - z, y, x};
    case cf::kSouth: return std::array<double, 3>{x, y, z};
    default: return std::nullopt;
    }
}

common::items::Handler capability() {
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return 1; };
    h.getSlot = [](common::Access const& at, int) -> ItemStack {
        return rt(game::globalPos(at.region, at.pos)).stack;
    };
    h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto        pos = game::globalPos(at.region, at.pos);
        auto const& b   = at.region.getBlock(at.pos);
        if (!isBrass(b) || !distributes(at.region, at.pos, b)) {
            // Create: the tunnel hands items to the belt below
            return common::items::insert(at.region, offset(at.pos, cf::kDown), cf::kUp, stack, simulate);
        }
        auto& r = rt(pos);
        if (!canTakeItems(r) || items::isEmpty(stack)) return stack;
        int take = std::min(items::countOf(stack), static_cast<int>(stack.getMaxStackSize()));
        if (!simulate) setStack(pos, r, items::withCount(stack, take), -1);
        return items::withCount(stack, items::countOf(stack) - take);
    };
    h.extract = [](common::Access const& at, common::items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        return common::items::extract(at.region, offset(at.pos, cf::kDown), cf::kUp, filter, amount, simulate);
    };
    return h;
}

/// Create's BeltTunnelInteractionHandler for an item entering the segment under the tunnel.
beltlink::Passing onBeltItem(BlockSource& region, BlockPos const& above, BlockPos const& beltPos, ItemStack& stack, int movement) {
    auto const* b = tunnelBlock(region, above);
    if (!b) return beltlink::Passing::Continue;
    auto pos = game::globalPos(region, above);
    if (isBrass(*b)) {
        if (!canInsert(region, pos, *b, cf::oppositeFace(movement), stack)) return beltlink::Passing::Stuck;
        if (!distributes(region, above, *b)) return beltlink::Passing::Continue;
        auto& r = rt(pos);
        if (!canTakeItems(r)) return beltlink::Passing::Stuck;
        setStack(pos, r, stack, cf::oppositeFace(movement));
        stack = items::empty();
        return beltlink::Passing::Taken;
    }
    // andesite: split one item off to every side output of a junction
    int axis = axisOf(*b);
    if (items::countOf(stack) > 1 && cf::isJunction(shapeOf(*b)) && cf::axisOf(movement) == axis) {
        auto flaps = flapsOf(region, above, *b);
        for (int d : cf::kHorizontals) {
            if (cf::axisOf(d) == axis || !contains(flaps, d)) continue;
            BlockPos out = offset(beltPos, d);
            if (!loadedBlock(region, out)) return beltlink::Passing::Stuck;
            if (!directInput(region, out, d)) continue;
            auto rest = common::items::insert(region, out, cf::oppositeFace(d), items::withCount(stack, 1), false);
            if (!items::isEmpty(rest)) return beltlink::Passing::Stuck;
            stack = items::withCount(stack, items::countOf(stack) - 1);
            if (items::countOf(stack) <= 1) break;
        }
    }
    return beltlink::Passing::Continue;
}

} // namespace

void tickTunnel(BlockSource& region, GlobalPos const& pos, Block const& b, std::uint64_t) {
    if (!isBrass(b)) return;
    auto& r  = rt(pos);
    auto  bp = toBlockPos(pos);
    if (r.progress > 0) r.progress--;
    auto segment = beltlink::segmentAt(region, offset(bp, cf::kDown));
    if (!segment || segment->speed == 0) return;
    if (items::isEmpty(r.stack) && !r.synced) return;

    if (r.progress == -1) {
        auto group   = groupOf(region, bp);
        auto mode    = modeOf(pos);
        if (mode == cf::SelectionMode::Synchronize) {
            bool allEmpty = true, allFull = true;
            for (auto const& t : group) {
                bool has = !items::isEmpty(rt(toGlobal(pos.dim, t)).stack);
                allEmpty &= !has;
                allFull &= has;
            }
            if (allFull || allEmpty) {
                for (auto const& t : group) {
                    auto& tr = rt(toGlobal(pos.dim, t));
                    if (tr.synced != !allEmpty) {
                        tr.synced = !allEmpty;
                        saveRt(toGlobal(pos.dim, t), tr);
                    }
                }
            }
            if (!r.synced) return;
        }
        if (items::isEmpty(r.stack)) return;
        bool any = false;
        for (auto const& [t, side] : validOutputs(region, bp, group)) {
            if (insertIntoTunnel(region, toGlobal(pos.dim, t), side, r.stack, true)) {
                any = true;
                break;
            }
        }
        if (!any) return;
        if (r.newItem) {
            r.newItem  = false;
            r.progress = 2;
        } else {
            if (mode != cf::SelectionMode::Synchronize || r.synced) r.progress = std::clamp(config().brassTunnelTimer, 1, 10);
            return;
        }
    }
    if (r.progress != 0) return;

    auto group   = groupOf(region, bp);
    auto outputs = validOutputs(region, bp, group);
    // filtered exits first, then the rest (Create's distributionTargets couple)
    std::vector<std::pair<BlockPos, int>> filtered, plain;
    for (auto const& [t, side] : outputs) {
        auto tpos = toGlobal(pos.dim, t);
        if (t == bp && side == r.enteredFrom) continue;
        if (!insertIntoTunnel(region, tpos, side, r.stack, true)) continue;
        (flapFilterEmpty(region, tpos, side) ? plain : filtered).emplace_back(t, side);
    }
    distribute(region, pos, r, filtered);
    distribute(region, pos, r, plain);
    r.progress = -1;
}

bool useTunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& b, ItemStack const& held) {
    if (!isBrass(b) || !items::isEmpty(held)) return false;
    std::vector<ItemStack> stacks;
    for (auto const& t : groupOf(region, toBlockPos(pos))) {
        auto  tpos = toGlobal(pos.dim, t);
        auto& tr   = rt(tpos);
        if (items::isEmpty(tr.stack)) continue;
        stacks.push_back(tr.stack);
        setStack(tpos, tr, items::empty(), -1);
    }
    if (stacks.empty()) return false;
    for (auto const& s : stacks) giveOrDrop(player, s);
    playSound(region, toBlockPos(pos), "random.pop", .2f, 1.f + static_cast<float>(rng()() % 1000) / 1000.f);
    log().info("funnels: {} retrieved {} stack(s) from the brass tunnel at {}", player.getRealName(), stacks.size(), pos.key());
    return true;
}

bool wrenchTunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& b) {
    auto shape = shapeOf(b);
    if (!cf::hasWindow(shape)) return false;
    auto next = shape == cf::TunnelShape::Closed ? cf::TunnelShape::Window : cf::TunnelShape::Closed;
    setStates(region, toBlockPos(pos), {{kShape, static_cast<int>(next)}});
    playSound(region, toBlockPos(pos), "random.click", .5f, 1.2f);
    log().info("funnels: tunnel at {} {} by {}", pos.key(), next == cf::TunnelShape::Closed ? "closed" : "opened", player.getRealName());
    return true;
}

void updateTunnelShape(BlockSource& region, GlobalPos const& pos) {
    auto        bp = toBlockPos(pos);
    auto const* b  = tunnelBlock(region, bp);
    if (!b) return;
    BlockPos below = offset(bp, cf::kDown);
    int      axis  = axisOf(*b);
    if (auto seg = beltlink::segmentAt(region, below); seg && cf::isHorizontal(seg->movementFacing)) axis = cf::axisOf(seg->movementFacing);
    // Create's hasValidOutput: a belt across, or something taking items from that side
    auto output = [&](int side) {
        BlockPos o = offset(below, side);
        if (auto seg = beltlink::segmentAt(region, o)) return cf::isHorizontal(seg->movementFacing) && cf::axisOf(seg->movementFacing) == cf::axisOf(side);
        auto id = idAt(region, o);
        return id.starts_with("levitate:") && common::items::has(region, o, cf::oppositeFace(side));
    };
    int  left       = cf::tunnelLeft(axis);
    int  fw         = cf::positiveFace(axis);
    bool canWindow  = tunnelBlock(region, offset(bp, fw)) && tunnelBlock(region, offset(bp, cf::oppositeFace(fw)));
    auto shape      = cf::tunnelShape(output(left), output(cf::oppositeFace(left)), canWindow);
    auto current    = shapeOf(*b);
    // Create keeps the wrench's window / closed choice while the tunnel stays straight between
    // two tunnels
    if (axis == axisOf(*b) && shape == cf::TunnelShape::Window && current == cf::TunnelShape::Closed) shape = current;
    setStates(region, bp, {{kAxis, axis}, {kShape, static_cast<int>(shape)}});
}

void forgetTunnel(GlobalPos const& pos) { runtimes().erase(pos.key()); }

void installTunnels() {
    for (char const* id : {kAndesiteTunnel, kBrassTunnel}) {
        common::items::registerBlock(id, capability());
        beltlink::registerAbove(id, onBeltItem);
    }
    for (int side : cf::kHorizontals) {
        common::filterslot::registerSlot(
            kBrassTunnel,
            {sideSlot(side),
             [side](Block const& b, int face) -> std::optional<std::array<double, 3>> {
                 if (face != side || !contains(sidesOf(b), side)) return std::nullopt;
                 return turned(side, 8 / 16.0, 13 / 16.0, 15.5 / 16.0);
             },
             0.5,
             nullptr}
        );
    }
    common::valuebox::BoxSpec mode;
    mode.id            = "mode";
    mode.label         = "create.logistics.when_multiple_outputs_available";
    mode.labelFallback = "Distribution Method";
    mode.board         = [](Block const&) {
        return core::common::optionBoard(cf::kSelectionModes, tr("create.logistics.when_multiple_outputs_available", "Distribution Method"));
    };
    mode.centre = [](Block const&, int face) -> std::optional<std::array<double, 3>> {
        if (face != cf::kUp) return std::nullopt;
        return std::array<double, 3>{.5, 1., .5};
    };
    mode.get = [](GlobalPos const& p) { return core::common::ValueSettings{0, static_cast<int>(modeOf(p))}; };
    mode.set = [](GlobalPos const& p, core::common::ValueSettings v, Player&) {
        int value = std::clamp(v.value, 0, cf::kSelectionModes - 1);
        // the whole group takes the setting (Create propagates it to connected tunnels)
        auto* region = regionOf(p.dim);
        if (!region) return;
        for (auto const& t : groupOf(*region, toBlockPos(p))) editDoc(toGlobal(p.dim, t))["mode"] = value;
    };
    mode.format = [](Block const&, core::common::ValueSettings v) {
        auto m = static_cast<cf::SelectionMode>(std::clamp(v.value, 0, cf::kSelectionModes - 1));
        return tr(cf::selectionModeKey(m), cf::selectionModeEnglish(m));
    };
    common::valuebox::registerBox(kBrassTunnel, std::move(mode));
}

} // namespace lv::feature::funnels::detail
