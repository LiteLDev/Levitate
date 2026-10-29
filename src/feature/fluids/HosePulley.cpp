// Hose pulley (Create's HosePulleyBlockEntity + HosePulleyFluidHandler + FluidDrainingBehaviour /
// FluidFillingBehaviour): a kinetic block lowering a hose by speed / 512 blocks per tick (rotation
// direction decides up or down) through replaceable blocks. While still, pipes on its back drain
// the fluid body around the hose end (top layer first, farthest first) or fill the space there
// (lowest first, nearest first), through a 1500 mB buffer tank. Bodies of more than
// `hosePulleyBlockThreshold` blocks are infinite: drained without removing blocks (bottomless
// fluids only) and filled without placing them (unless `fillInfinite`).

#include <algorithm>
#include <map>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/fluids/FluidsInternal.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace cfl = common::fluids;
namespace kin = lv::kinetics;
namespace ck  = core::kinetics;

namespace {

constexpr int kCapacity = 1500; ///< Create: the hose pulley's internal tank

/// Create: the rotation axis is the clockwise neighbour of the facing (N/S -> X, E/W -> Z).
std::optional<ck::Axis> hoseAxis(ck::BlockState const& s) {
    int f = s.get(kFacing, 2);
    if (f == 2 || f == 3) return ck::Axis::X;
    if (f == 4 || f == 5) return ck::Axis::Z;
    return std::nullopt;
}

ck::KineticSpec hoseSpec() {
    ck::KineticSpec s;
    s.states          = {kFacing};
    s.rotationAxis    = hoseAxis;
    s.hasShaftTowards = [](ck::BlockState const& st, ck::Face f) {
        auto a = hoseAxis(st);
        return a && *a == ck::axisOf(f);
    };
    s.impact     = 4; // config kinetics.json "hose_pulley"
    s.rotorState = kRotor;
    return ck::specs::withModel(s, "geometry.levitate.hose_pulley", "levitate_hose_pulley");
}
LV_REGISTER_KINETIC_BLOCK(kHosePulley, hoseSpec());

/// Search results kept between transfers (Create's behaviours keep their queues too).
struct Cache {
    std::optional<cf::DrainPlan> drain;
    std::optional<cf::FillPlan>  fill;
    std::string                  fillFluid;
    std::uint64_t                age = 0;
    std::uint64_t                retryAt = 0; ///< a search that found nothing waits a second
};
std::map<GlobalPos, Cache>& caches() {
    static std::map<GlobalPos, Cache> c;
    return c;
}

bool isWaterId(std::string const& id) { return id == "minecraft:water" || id == "minecraft:flowing_water"; }
bool isLavaId(std::string const& id) { return id == "minecraft:lava" || id == "minecraft:flowing_lava"; }

/// What a fluid search sees in the world.
cf::Cell cellAt(BlockSource& region, int dim, cf::Pos const& q) {
    BlockPos bp{q.x, q.y, q.z};
    cf::Cell c;
    if (q.y < region.getMinHeight() || q.y >= region.getMaxHeight() || !region.hasChunksAt(bp, 0, false)) return c;
    auto const& b  = region.getBlock(bp);
    auto        id = b.getTypeName();
    if (isWaterId(id) || isLavaId(id)) {
        c.fluid    = isWaterId(id) ? std::string(cf::kWater) : std::string(cf::kLava);
        c.source   = game::intState(b, "liquid_depth").value_or(0) == 0;
        c.fillable = !c.source;
        return c;
    }
    if (id == kHoneyBlock || id == kChocolateBlock) {
        c.fluid  = id == kHoneyBlock ? std::string(cf::kHoney) : std::string(cf::kChocolate);
        c.source = true;
        return c;
    }
    auto const& extra = region.getExtraBlock(bp);
    if (!extra.isAir() && isWaterId(extra.getTypeName()) && game::intState(extra, "liquid_depth").value_or(0) == 0) {
        c.fluid  = std::string(cf::kWater); // waterlogged
        c.source = true;
        return c;
    }
    c.fillable = b.isAir() || game::replaceable(region, bp);
    (void)dim;
    return c;
}

bool bottomless(std::string const& fluid) {
    auto const& cfg  = config();
    auto        in   = [&](std::vector<std::string> const& v) { return std::find(v.begin(), v.end(), fluid) != v.end(); };
    auto const& mode = cfg.bottomlessFluidMode;
    if (mode == "ALLOW_ALL") return true;
    if (mode == "DENY_ALL") return false;
    if (mode == "DENY_BY_TAG") return !in(cfg.bottomlessDeny);
    return in(cfg.bottomlessAllow); // ALLOW_BY_TAG
}

int threshold() {
    int t = config().hosePulleyBlockThreshold;
    return t < 0 ? 1 << 30 : t;
}

Block const* sourceBlock(std::string const& fluid) {
    if (fluid == cf::kWater) return game::blockWith("minecraft:water");
    if (fluid == cf::kLava) return game::blockWith("minecraft:lava");
    if (fluid == cf::kHoney) return game::blockWith(kHoneyBlock);
    if (fluid == cf::kChocolate) return game::blockWith(kChocolateBlock);
    return nullptr;
}

struct Hose {
    GlobalPos  pos;
    float      offset = 0;
    bool       moving = false;
    FluidStack tank;
};

Hose load(GlobalPos const& p) {
    Hose h{p};
    if (auto const* d = dataAt(p)) {
        h.offset = d->value("offset", 0.f);
        h.moving = d->value("moving", false);
        h.tank   = tankOf(*d);
    }
    return h;
}

void save(Hose const& h) {
    auto& d     = editData(h.pos);
    d["offset"] = h.offset;
    d["moving"] = h.moving;
    setTank(d, h.tank);
}

GlobalPos rootOf(Hose const& h) { return offsetPos(h.pos, 0, cf::hoseEndDepth(h.offset)); }

/// FluidDrainingBehaviour.pullNext: removes (or, infinite, just reads) the next source block.
FluidStack pullNext(Hose const& h, bool simulate) {
    auto* region = regionOf(h.pos.dim);
    if (!region || h.moving || h.offset <= 0) return {};
    GlobalPos root  = rootOf(h);
    auto&     cache = caches()[h.pos];
    auto      query = [&](cf::Pos const& q) { return cellAt(*region, h.pos.dim, q); };
    if (!cache.drain || cache.drain->sources.empty() || ++cache.age > 600) {
        if (currentTick() < cache.retryAt) return {};
        cache.drain = cf::planDrain({root.x, root.y, root.z}, query, config().hosePulleyRange, threshold());
        cache.age   = 0;
        if (cache.drain->sources.empty() && !cache.drain->infinite) cache.retryAt = currentTick() + 20;
        if (cache.drain->infinite)
            log().info("hose pulley at {} {} {} found an infinite {} source", h.pos.x, h.pos.y, h.pos.z, cache.drain->fluid);
    }
    auto& plan = *cache.drain;
    if (plan.fluid.empty()) {
        cache.drain.reset();
        cache.retryAt = currentTick() + 20;
        return {};
    }
    if (plan.infinite && bottomless(plan.fluid)) {
        if (query({root.x, root.y, root.z}).fluid != plan.fluid) {
            cache.drain.reset();
            return {};
        }
        return {plan.fluid, cf::kBucket};
    }
    while (!plan.sources.empty()) {
        auto q = plan.sources.front();
        auto c = query(q);
        if (c.fluid != plan.fluid || !c.source) {
            plan.sources.erase(plan.sources.begin());
            continue;
        }
        if (simulate) return {plan.fluid, cf::kBucket};
        BlockPos bp{q.x, q.y, q.z};
        auto const& b = region->getBlock(bp);
        if (b.getTypeName() == kHoneyBlock || b.getTypeName() == kChocolateBlock || isWaterId(b.getTypeName()) || isLavaId(b.getTypeName())) {
            game::clearBlock(*region, bp);
        } else if (auto const* air = game::blockWith("minecraft:air")) {
            region->setExtraBlock(bp, *air, 3); // waterlogged: drain the water layer
        }
        plan.sources.erase(plan.sources.begin());
        playSound({h.pos.dim, q.x, q.y, q.z}, plan.fluid == cf::kLava ? "bucket.fill_lava" : "bucket.fill_water", 0.5f, 1.f);
        log().info("hose pulley at {} {} {} drained {} at {} {} {}", h.pos.x, h.pos.y, h.pos.z, plan.fluid, q.x, q.y, q.z);
        return {plan.fluid, cf::kBucket};
    }
    cache.drain.reset();
    return {};
}

/// FluidFillingBehaviour.tryDeposit: places one source block (or accepts the fluid into an
/// infinite body / the nether's heat).
bool tryDeposit(Hose const& h, std::string const& fluid, bool simulate) {
    auto* region = regionOf(h.pos.dim);
    if (!region || h.moving || h.offset <= 0 || !common::fluidtypes::hasBlockState(fluid)) return false;
    GlobalPos root  = rootOf(h);
    auto&     cache = caches()[h.pos];
    auto      query = [&](cf::Pos const& q) { return cellAt(*region, h.pos.dim, q); };
    if (!cache.fill || cache.fillFluid != fluid || cache.fill->targets.empty() || ++cache.age > 600) {
        if (currentTick() < cache.retryAt) return false;
        cache.fill      = cf::planFill({root.x, root.y, root.z}, fluid, query, config().hosePulleyRange, threshold());
        cache.fillFluid = fluid;
        cache.age       = 0;
        if (cache.fill->targets.empty() && !cache.fill->infinite) cache.retryAt = currentTick() + 20;
    }
    auto& plan = *cache.fill;
    if (plan.infinite && !config().fillInfinite) return true; // voided into the infinite body
    if (!config().fluidFillPlaceFluidSourceBlocks) return true; // Create: the fluid is voided
    while (!plan.targets.empty()) {
        auto q = plan.targets.front();
        if (!query(q).fillable) {
            plan.targets.erase(plan.targets.begin());
            continue;
        }
        if (simulate) return true;
        plan.targets.erase(plan.targets.begin());
        GlobalPos at{h.pos.dim, q.x, q.y, q.z};
        if (fluid == cf::kWater && h.pos.dim == 1) {
            playSound(at, "random.fizz", 0.5f, 2.6f); // the nether evaporates water
            return true;
        }
        auto const* b = sourceBlock(fluid);
        if (!b) return false;
        game::setBlock(*region, BlockPos{q.x, q.y, q.z}, *b);
        playSound(at, fluid == cf::kLava ? "bucket.empty_lava" : "bucket.empty_water", 0.5f, 1.f);
        log().info("hose pulley at {} {} {} placed {} at {} {} {}", h.pos.x, h.pos.y, h.pos.z, fluid, q.x, q.y, q.z);
        return true;
    }
    cache.fill.reset();
    return false;
}

/// Create's HosePulleyFluidHandler.fill.
int handlerFill(GlobalPos const& p, FluidStack const& resource, bool simulate) {
    auto h = load(p);
    if (resource.empty() || !common::fluidtypes::hasBlockState(resource.fluid)) return 0;
    if (!h.tank.empty() && h.tank.fluid != resource.fluid) return 0;
    int        diff     = resource.amount;
    int        total    = diff + (h.tank.empty() ? 0 : h.tank.amount);
    FluidStack remaining = resource;
    bool       deposited = false;
    if (!h.moving && total >= cf::kBucket) {
        if (tryDeposit(h, resource.fluid, simulate)) {
            caches()[p].drain.reset(); // drainer.counterpartActed
            caches()[p].retryAt = 0;
            remaining.amount -= cf::kBucket;
            diff -= cf::kBucket;
            deposited = true;
        }
    }
    if (simulate) {
        if (diff <= 0) return resource.amount;
        return tankFill(h.tank, kCapacity, remaining, true) + (deposited ? cf::kBucket : 0);
    }
    if (diff <= 0) {
        tankDrain(h.tank, {h.tank.fluid, -diff}, false);
        save(h);
        return resource.amount;
    }
    int n = tankFill(h.tank, kCapacity, remaining, false);
    save(h);
    return n + (deposited ? cf::kBucket : 0);
}

/// Create's HosePulleyFluidHandler.drainInternal.
FluidStack handlerDrain(GlobalPos const& p, FluidStack const& request, bool simulate) {
    auto h = load(p);
    if (request.amount <= 0) return {};
    if (!request.fluid.empty() && !h.tank.empty() && request.fluid != h.tank.fluid) return {};
    auto drainTank = [&]() {
        auto out = tankDrain(h.tank, request, simulate);
        if (!simulate && !out.empty()) save(h);
        return out;
    };
    if (!h.tank.empty() && h.tank.amount >= cf::kBucket) return drainTank();
    // what the next pull would give
    auto peek = pullNext(h, true);
    if (h.moving || peek.empty()) return drainTank();
    if (!h.tank.empty() && h.tank.fluid != peek.fluid) return drainTank();
    if (!request.fluid.empty() && request.fluid != peek.fluid) return {};
    if (!simulate) {
        if (pullNext(h, false).empty()) return drainTank();
        caches()[p].fill.reset(); // filler.counterpartActed
        caches()[p].retryAt = 0;
    }
    int available = cf::kBucket + (h.tank.empty() ? 0 : h.tank.amount);
    int drained   = std::min(request.amount, available);
    if (!simulate) {
        int left = available - drained;
        h.tank   = left > 0 ? FluidStack{peek.fluid, left} : FluidStack{};
        save(h);
    }
    return {peek.fluid, drained};
}

/// Create: pipes connect on the back of the hose pulley (the side opposite its facing).
bool pipeSide(common::Access const& at) {
    if (at.face < 0) return true;
    int facing = game::intState(at.region.getBlock(at.pos), kFacing).value_or(2);
    return at.face == cf::oppositeFace(facing);
}

} // namespace

float hoseOffset(GlobalPos const& p) { return load(p).offset; }

void tickHose(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return;
    auto  h     = load(p);
    float speed = kin::speedAt(p);
    if (speed == 0 && !h.moving) {
        syncHose(p, h.offset, 0);
        return;
    }
    auto step = cf::stepHose(h.offset, speed, [&](int depth) {
        GlobalPos q = offsetPos(p, 0, depth);
        auto      c = cellAt(*region, p.dim, {q.x, q.y, q.z});
        return c.fillable || !c.fluid.empty();
    });
    bool wasMoving = h.moving;
    bool changed   = step.offset != h.offset || step.moving != h.moving;
    h.offset       = step.offset;
    h.moving       = step.moving;
    if (h.moving) caches().erase(p); // drainer.reset / filler.reset
    if (changed) save(h);
    if (wasMoving && !h.moving)
        log().info("hose pulley at {} {} {} stopped at {} blocks", p.x, p.y, p.z, cf::hoseEndDepth(h.offset));
    syncHose(p, h.offset, h.moving ? cf::linearSpeed(speed) : 0);
}

void registerHose() {
    cfl::Handler fh;
    fh.tanks = [](common::Access const& at) {
        auto h = load(game::globalPos(at.region, at.pos));
        return std::vector<cfl::Tank>{{h.tank, kCapacity}};
    };
    fh.fill = [](common::Access const& at, FluidStack const& stack, bool simulate) {
        if (!pipeSide(at)) return 0;
        return handlerFill(game::globalPos(at.region, at.pos), stack, simulate);
    };
    fh.drain = [](common::Access const& at, FluidStack const& request, bool simulate) {
        if (!pipeSide(at)) return FluidStack{};
        return handlerDrain(game::globalPos(at.region, at.pos), request, simulate);
    };
    cfl::registerBlock(kHosePulley, fh);
    game::db::onWorldClosed([] { caches().clear(); });
}

} // namespace lv::feature::fluids
