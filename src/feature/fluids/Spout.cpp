// Spout (Create's SpoutBlockEntity): a 1000 mB tank that fills the item held two blocks below it
// or a cauldron there. Filling takes 20 ticks; the item is filled 15 ticks in.
//
// - Belts and depots (U11) hold the item under the spout through their processing behaviour
//   (belts/BeltApi.h registerProcessor: Create's onItemReceived / whenItemHeld), so items on a
//   moving belt stop, are filled one at a time and move on.
// - Other Levitate blocks that expose the item as slot 0 from the top through the item capability
//   (the selftests' depot) are filled through the capability.
// - Particles (Create's spawnProcessingParticles / spawnSplash) for every client; the client mod
//   draws the stream and squeezes the nozzle (src/client/fluids/FluidsRender.cpp).

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/advancements/Advancements.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/Items.h"
#include "feature/fluids/FluidsInternal.h"
#include "feature/pipes/FluidFX.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace items = common::items;
namespace cfl   = common::fluids;

namespace {

constexpr int kCapacity    = 1000; ///< Create: SpoutBlockEntity tank
constexpr int kFillingTime = 20;   ///< Create: FILLING_TIME
constexpr int kFillAt      = 5;    ///< the item is filled when processingTicks reaches 5
constexpr int kSplashCount = 20;   ///< Create: SPLASH_PARTICLE_COUNT

namespace fx = pipes::fx;

/// Create's spawnProcessingParticles: a drop leaves the nozzle every tick while filling.
void processingParticle(GlobalPos const& p, std::string const& fluid) {
    fx::spawn(p.dim, {p.x + .5, p.y + .5 - .5, p.z + .5}, {0, -.1, 0}, fluid, fx::Kind::Fluid);
}

/// Create's spawnSplash: 20 drops bounce off the filled item.
void splash(GlobalPos const& p, std::string const& fluid) {
    for (int i = 0; i < kSplashCount; ++i) {
        auto r = [] { return (fx::random01() - .5) * 2 * .125; };
        fx::spawn(p.dim, {p.x + .5, p.y + .5 - (2 - 5 / 16.), p.z + .5}, {r(), std::abs(r()), r()}, fluid, fx::Kind::Fluid);
    }
}

FluidStack spoutTank(GlobalPos const& p) {
    auto const* d = dataAt(p);
    return d ? tankOf(*d) : FluidStack{};
}

/// The item handler of the processing position (a Levitate block: depots, belts; vanilla
/// containers have no processing behaviour in Create).
std::optional<common::items::Handler> processingHandler(BlockSource& region, GlobalPos const& at) {
    auto id = blockIdAt(at);
    if (!id.starts_with("levitate:")) return std::nullopt;
    if (belts::isTransporter(at)) return std::nullopt; // belts and depots hold it for the processor below
    common::Access acc{region, toBlockPos(at), 1};
    auto           h = items::find(acc);
    if (!h || !h->slots || !h->getSlot || !h->insert || !h->extract || h->slots(acc) < 1) return std::nullopt;
    return h;
}

bool isCauldron(GlobalPos const& at) {
    auto id = blockIdAt(at);
    return id == "minecraft:cauldron" || id == "minecraft:lava_cauldron";
}

/// What the spout would fill now: the item below or a cauldron.
struct Target {
    bool       cauldron = false;
    ItemStack  item;
    int        amount = -1;
};

std::optional<Target> findTarget(BlockSource& region, GlobalPos const& p, FluidStack const& tank) {
    GlobalPos gap = offsetPos(p, 0, 1), at = offsetPos(p, 0, 2);
    if (!passableAt(gap)) return std::nullopt; // Create's BeltProcessingBehaviour.isBlocked
    if (auto h = processingHandler(region, at)) {
        common::Access acc{region, toBlockPos(at), 1};
        auto           stack = h->getSlot(acc, 0);
        if (items::isEmpty(stack) || !canBeFilled(stack, true)) return std::nullopt;
        int need = requiredAmount(stack, tank, true);
        if (need <= 0) return std::nullopt;
        return Target{false, stack, need};
    }
    if (isCauldron(at) && !tank.empty()) {
        if (cfl::fill(region, toBlockPos(at), 1, {tank.fluid, cf::kBucket}, true) != cf::kBucket) return std::nullopt;
        return Target{true, items::empty(), cf::kBucket};
    }
    return std::nullopt;
}

void finish(BlockSource& region, GlobalPos const& p, FluidStack& tank) {
    GlobalPos at = offsetPos(p, 0, 2);
    auto      t  = findTarget(region, p, tank);
    if (!t || t->amount > tank.amount) return;
    if (t->cauldron) {
        if (cfl::fill(region, toBlockPos(at), 1, {tank.fluid, cf::kBucket}, false) != cf::kBucket) return;
        log().info("spout at {} {} {} filled a cauldron with {}", p.x, p.y, p.z, tank.fluid);
        tankDrain(tank, {tank.fluid, cf::kBucket}, false);
        playSound(p, "bucket.empty_water", 0.75f, 1.f);
        return;
    }
    auto           h = processingHandler(region, at);
    common::Access acc{region, toBlockPos(at), 1};
    if (!h) return;
    auto out = fillItem(t->item, tank, true);
    if (items::isEmpty(out)) return;
    auto taken = h->extract(acc, [&](ItemStack const& s) { return items::sameItemSameData(s, t->item); }, 1, false);
    if (items::isEmpty(taken)) return;
    auto in    = items::idOf(t->item);
    auto fluid = tank.fluid;
    tankDrain(tank, {tank.fluid, t->amount}, false);
    auto rest = h->insert(acc, out, false);
    if (!items::isEmpty(rest)) spawnItem(at.dim, at.x + .5, at.y + 1.0, at.z + .5, rest);
    log().info("spout at {} {} {} filled {} into {}", p.x, p.y, p.z, in, items::idOf(out));
    playSound(p, "bucket.fill_water", 0.4f, 1.4f);
    splash(p, fluid);
    advancements::onSpoutFilled(p, items::idOf(out));
}

// ---- belts and depots (Create's BeltProcessingBehaviour on the spout) ------------------------------

/// Create's SpoutBlockEntity.onItemReceived.
belts::ProcessingResult onItemReceived(belts::ProcessorContext const& c, belts::TransportedItem& t) {
    if (!canBeFilled(t.stack, true)) return belts::ProcessingResult::Pass;
    auto tank = spoutTank(c.processor);
    if (tank.empty()) return belts::ProcessingResult::Hold;
    if (requiredAmount(t.stack, tank, true) == -1) return belts::ProcessingResult::Pass;
    return belts::ProcessingResult::Hold;
}

/// Create's SpoutBlockEntity.whenItemHeld.
belts::ProcessingResult whenItemHeld(belts::ProcessorContext const& c, belts::TransportedItem& t) {
    GlobalPos const& p          = c.processor;
    auto const*      d          = dataAt(p);
    int              processing = d ? d->value("processing", -1) : -1;
    if (processing != -1 && processing != kFillAt) return belts::ProcessingResult::Hold;
    if (!canBeFilled(t.stack, true)) return belts::ProcessingResult::Pass;
    FluidStack tank = d ? tankOf(*d) : FluidStack{};
    if (tank.empty()) return belts::ProcessingResult::Hold;
    int need = requiredAmount(t.stack, tank, true);
    if (need == -1) return belts::ProcessingResult::Pass;
    if (need > tank.amount) return belts::ProcessingResult::Hold;

    if (processing == -1) {
        auto& e         = editData(p);
        e["processing"] = kFillingTime;
        e["belt"]       = true; // finished here, not by the spout's tick
        playSound(p, "bucket.fill_water", 0.75f, 0.9f + 0.2f * static_cast<float>(fx::random01()));
        syncSpout(p, tank.fluid);
        return belts::ProcessingResult::Hold;
    }

    // the filling is done (Create: FillingBySpout.fillItem, then convertToAndLeaveHeld)
    ItemStack held = t.stack;
    auto      out  = fillItem(held, tank, true);
    if (!items::isEmpty(out)) {
        auto in    = items::idOf(held);
        auto fluid = tank.fluid;
        bool converted = false;
        belts::handleCenteredProcessing(c.transporter, .5f, [&](belts::TransportedItem& it) {
            if (converted || !it.locked || !items::sameItemSameData(it.stack, held)) return belts::TransportedResult::doNothing();
            converted = true;
            int rest = items::countOf(it.stack) - 1;
            if (rest <= 0) return belts::TransportedResult::convertTo({out});
            return belts::TransportedResult::convertToAndLeaveHeld({out}, items::withCount(it.stack, rest));
        });
        if (!converted) return belts::ProcessingResult::Hold;
        tankDrain(tank, {tank.fluid, need}, false);
        setTank(editData(p), tank);
        log().info("spout at {} {} {} filled {} into {}", p.x, p.y, p.z, in, items::idOf(out));
        playSound(p, "bucket.fill_water", 0.4f, 1.4f);
        splash(p, fluid);
        advancements::onSpoutFilled(p, items::idOf(out));
    }
    return belts::ProcessingResult::Hold;
}

} // namespace

void tickSpout(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return;
    auto const* d          = dataAt(p);
    int         processing = d ? d->value("processing", -1) : -1;
    FluidStack  tank       = d ? tankOf(*d) : FluidStack{};
    if (processing < 0) {
        if (tank.empty()) return;
        auto t = findTarget(*region, p, tank);
        if (!t || t->amount > tank.amount) return; // Create: wait (HOLD) until there is enough
        editData(p)["processing"] = kFillingTime;
        playSound(p, "bucket.fill_water", 0.75f, 0.9f);
        syncSpout(p, tank.fluid);
        return;
    }
    --processing;
    bool belt = d && d->value("belt", false);
    if (processing >= 8 && !tank.empty()) processingParticle(p, tank.fluid);
    if (processing == kFillAt && !belt) finish(*region, p, tank);
    auto& e = editData(p);
    if (processing < 0) {
        e.erase("processing");
        e.erase("belt");
    } else {
        e["processing"] = processing;
    }
    if (!belt) setTank(e, tank); // the belt processor keeps the tank itself
}

void registerSpout() {
    cfl::Handler h;
    h.tanks = [](common::Access const& at) {
        return std::vector<cfl::Tank>{{spoutTank(game::globalPos(at.region, at.pos)), kCapacity}};
    };
    h.fill = [](common::Access const& at, FluidStack const& stack, bool simulate) {
        if (at.face == 0) return 0; // the nozzle side
        auto p    = game::globalPos(at.region, at.pos);
        auto tank = spoutTank(p);
        int  n    = tankFill(tank, kCapacity, stack, simulate);
        if (!simulate && n > 0) setTank(editData(p), tank);
        return n;
    };
    h.drain = [](common::Access const& at, FluidStack const& request, bool simulate) {
        auto p    = game::globalPos(at.region, at.pos);
        auto tank = spoutTank(p);
        auto out  = tankDrain(tank, request, simulate);
        if (!simulate && !out.empty()) setTank(editData(p), tank);
        return out;
    };
    cfl::registerBlock(kSpout, h);
    belts::registerProcessor(kSpout, belts::Processor{.received = onItemReceived, .held = whenItemHeld});
}

} // namespace lv::feature::fluids
