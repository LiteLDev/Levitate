// Redstone unit (U23): placement, survival, the per-tick behaviour dispatcher and the simple
// components (analog lever, brass diodes, latches, redstone contact).

#include <algorithm>
#include <cmath>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::redstone {

namespace {

bool canBuild(Player& p) { return !p.isSpectator() && p.canUseAbility(AbilitiesIndex::Build); }

bool holds(ItemStack const& s, char const* id) { return !s.isNull() && s.getTypeName() == id; }

std::string posText(GP const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

/// Directions sorted by how directly the player looks along them (Java's getNearestLookingDirections).
std::array<int, 6> lookingOrder(float pitch, float yaw) {
    std::array<int, 6> dirs{0, 1, 2, 3, 4, 5};
    double p = pitch * 3.14159265358979 / 180.0, y = yaw * 3.14159265358979 / 180.0;
    double lx = -std::sin(y) * std::cos(p), ly = -std::sin(p), lz = std::cos(y) * std::cos(p);
    auto   score = [&](int d) {
        auto o = offsetOf(d);
        return o[0] * lx + o[1] * ly + o[2] * lz;
    };
    std::stable_sort(dirs.begin(), dirs.end(), [&](int a, int b) { return score(a) > score(b); });
    return dirs;
}

/// A neighbour a smart observer / threshold switch can read (Create checks the item and fluid
/// capabilities, belts, pipes and funnels; ours come through the capabilities).
bool canDetect(BlockSource& region, BlockPos const& pos) {
    if (!loadedAt(region, pos)) return false;
    common::Access at{region, pos, -1};
    return common::items::find(at).has_value() || common::fluids::find(at).has_value();
}

/// The block a lever / link hangs on, as a direction from the block.
int attachedDirection(Block const& block, Kind kind) {
    if (kind == Kind::Lever) {
        switch (state(block, sFace, 0)) {
        case 0: return kDown;
        case 2: return kUp;
        default: return opposite(state(block, sFacing, kNorth));
        }
    }
    return opposite(state(block, sFacing, kUp));
}

bool supports(BlockSource& region, BlockPos const& pos) {
    if (!loadedAt(region, pos)) return true; // unknown: keep the block
    return !game::replaceable(region, pos);
}

} // namespace

// ---- placement ---------------------------------------------------------------------------------

Block const* placementState(BlockSource& region, BlockPos const& pos, Block const& placed, Kind kind, Placement const& p) {
    switch (kind) {
    case Kind::Link: return withStates(placed, {{sFacing, p.clickedFace}, {sReceiver, 0}, {sPowered, 0}, {sPower, 0}});
    case Kind::Lever: {
        int face = p.clickedFace == kUp ? 0 : p.clickedFace == kDown ? 2 : 1;
        int dir  = face == 1 ? p.clickedFace : horizontalLooking(p.yaw);
        return withStates(placed, {{sFace, face}, {sFacing, dir}, {sLeverState, 0}, {sPower, 0}});
    }
    case Kind::PulseRepeater:
    case Kind::PulseExtender:
    case Kind::PulseTimer:
    case Kind::PoweredLatch:
    case Kind::ToggleLatch: return withStates(placed, {{sFacing, opposite(horizontalLooking(p.yaw))}});
    case Kind::SmartObserver:
    case Kind::ThresholdSwitch: {
        std::optional<int> preferred;
        for (int d : lookingOrder(p.pitch, p.yaw)) {
            if (canDetect(region, step(pos, d))) {
                preferred = d;
                break;
            }
        }
        if (!preferred) {
            int facing = nearestLooking(p.pitch, p.yaw);
            preferred  = p.sneaking ? facing : opposite(facing);
        }
        if (*preferred == kUp || *preferred == kDown) {
            return withStates(placed, {{sTarget, *preferred == kUp ? 2 : 1}, {sFacing, horizontalLooking(p.yaw)}});
        }
        return withStates(placed, {{sTarget, 0}, {sFacing, *preferred}});
    }
    case Kind::Contact: {
        int facing         = opposite(nearestLooking(p.pitch, p.yaw));
        int placeDirection = opposite(p.clickedFace);
        if (p.sneaking || hasValidContact(region, pos, placeDirection)) facing = placeDirection;
        return withStates(placed, {{sFacing, facing}, {sPowered, hasValidContact(region, pos, facing) ? 1 : 0}});
    }
    }
    return &placed;
}

bool canSurvive(BlockSource& region, BlockPos const& pos, Block const& block, Kind kind) {
    switch (kind) {
    case Kind::Link:
    case Kind::Lever: return supports(region, step(pos, attachedDirection(block, kind)));
    case Kind::PulseRepeater:
    case Kind::PulseExtender:
    case Kind::PulseTimer:
    case Kind::PoweredLatch:
    case Kind::ToggleLatch: return supports(region, step(pos, kDown));
    default: return true;
    }
}

// ---- lifecycle ---------------------------------------------------------------------------------

void onAdded(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    rt.neighbour = true;
    if (rt.kind == Kind::ThresholdSwitch) rt.lazy = 0;
    if (rt.kind == Kind::Link) linkLoaded(region, pos, block, rt);
    rt.loaded = true;
}

void onRemoved(GP const& pos, Runtime& rt) {
    if (rt.kind == Kind::Link) linkUnloaded(pos, rt);
}

void onUnloaded(GP const& pos, Runtime& rt) {
    if (rt.kind == Kind::Link) linkUnloaded(pos, rt);
    rt.loaded = false;
}

// ---- analog lever ------------------------------------------------------------------------------

void leverUse(BlockSource& region, GP const& pos, Block const& block, bool back) {
    auto bp   = game::blockPos(pos);
    int  prev = state(block, sLeverState);
    int  next = cr::analogLeverNext(prev, back);
    if (next != prev) {
        setStates(region, bp, {{sLeverState, next}});
        // neighbours see it 15 ticks after the last click
        if (auto* rt = runtimeAt(pos)) counterOf(pos, *rt) = cr::kAnalogLeverDelay;
        else dataFor(pos)["counter"] = cr::kAnalogLeverDelay;
    }
    playSound(pos.dim, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, "random.click", 0.2f, cr::analogLeverPitch(next));
    if (next > 0) spawnParticle(pos.dim, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, "minecraft:redstone_ore_dust_particle");
    log().info("analog lever at {} set to {}", posText(pos), next);
}

namespace {

void tickLever(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    int& left   = counterOf(pos, rt);
    int  handle = state(block, sLeverState);
    if (left > 0) {
        if (--left == 0) setStates(region, game::blockPos(pos), {{sPower, handle}});
    } else if (state(block, sPower) != handle) {
        // placed or set without a click (commands): follow the handle
        setStates(region, game::blockPos(pos), {{sPower, handle}});
    }
    // Create's animateTick: red dust while the lever is on (one in four ticks)
    static std::minstd_rand rng{0x5eed};
    if (handle > 0 && rng() % 16 == 0) {
        spawnParticle(pos.dim, pos.x + 0.5, pos.y + 0.4, pos.z + 0.5, "minecraft:redstone_ore_dust_particle");
    }
}

// ---- brass diodes ------------------------------------------------------------------------------

void tickBrassDiode(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    auto bp      = game::blockPos(pos);
    bool input   = rt.inBack;
    bool powered = state(block, sPowered) != 0;
    if (cr::advanceSchedule(rt.sched, powered != input, cr::kBrassDiodeInputDelay)) {
        powered = cr::diodeInputTick(powered, input, rt.sched, cr::kBrassDiodeInputDelay);
    }
    int            delay = diodeDelay(pos, rt.kind);
    int&           count = counterOf(pos, rt);
    cr::DiodeState s{count, state(block, sPowering) != 0};
    cr::tickDiode(diodeKind(rt.kind), delay, powered, s);
    count = s.state;
    setStates(region, bp, {{sPowered, powered ? 1 : 0}, {sPowering, s.powering ? 1 : 0}});
}

// ---- latches -----------------------------------------------------------------------------------

void tickLatch(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    auto bp   = game::blockPos(pos);
    bool back = rt.inBack;
    if (rt.kind == Kind::PoweredLatch) {
        bool           side = rt.inSide;
        cr::LatchState s{state(block, sPowered) != 0, state(block, sPoweredSide) != 0, state(block, sPowering) != 0};
        if (cr::advanceSchedule(rt.sched, cr::poweredLatchNeedsTick(s, back, side), cr::kLatchInputDelay)) {
            s = cr::poweredLatchTick(s, back, side);
            setStates(region, bp, {{sPowered, s.powered}, {sPoweredSide, s.poweredSide}, {sPowering, s.powering}});
        }
        return;
    }
    cr::ToggleLatchState s{state(block, sPowered) != 0, state(block, sPowering) != 0};
    if (cr::advanceSchedule(rt.sched, s.powered != back, cr::kLatchInputDelay)) {
        s = cr::toggleLatchTick(s, back, rt.sched);
        setStates(region, bp, {{sPowered, s.powered}, {sPowering, s.powering}});
    }
}

// ---- contacts ----------------------------------------------------------------------------------

void tickContact(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    bool recheck = rt.neighbour;
    if (rt.blockTick >= 0 && --rt.blockTick < 0) {
        rt.contraptionContact = false; // the contraption's contact moved on: Create's scheduled tick
        recheck               = true;
    }
    if (!recheck || rt.contraptionContact) return;
    bool valid = hasValidContact(region, game::blockPos(pos), state(block, sFacing, kUp));
    if ((state(block, sPowered) != 0) != valid) setStates(region, game::blockPos(pos), {{sPowered, valid ? 1 : 0}});
}

} // namespace

char const* counterKey(Kind kind) {
    switch (kind) {
    case Kind::Lever: return "lastChange";
    case Kind::SmartObserver: return "turnOff";
    default: return "state";
    }
}

int& counterOf(GP const& pos, Runtime& rt) {
    if (!rt.counterLoaded) {
        auto const* d    = dataOf(pos);
        rt.counter       = d ? d->value(counterKey(rt.kind), d->value("counter", 0)) : 0;
        rt.counterLoaded = true;
    }
    return rt.counter;
}

void persistCounter(GP const& pos, Runtime const& rt) {
    if (!rt.counterLoaded) return;
    auto const* d = dataOf(pos);
    if (!d && rt.counter == 0) return;
    if (d && d->value(counterKey(rt.kind), -1) == rt.counter) return;
    dataFor(pos)[counterKey(rt.kind)] = rt.counter;
}

void readInputs(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    auto bp = game::blockPos(pos);
    if (isDiodeShaped(rt.kind)) {
        int facing = state(block, sFacing, kNorth);
        rt.inBack  = signalFrom(region, bp, facing) > 0;
        rt.inSide  = rt.kind == Kind::PoweredLatch
               && (signalFrom(region, bp, clockwise(facing)) > 0 || signalFrom(region, bp, counterClockwise(facing)) > 0);
    } else if (rt.kind == Kind::Link && !state(block, sReceiver)) {
        rt.inLink = linkInput(region, bp, state(block, sFacing, kUp));
    }
}

bool hasValidContact(BlockSource& region, BlockPos const& pos, int facing) {
    auto n = step(pos, facing);
    if (!loadedAt(region, n)) return false;
    auto const& other = region.getBlock(n);
    auto const& id    = other.getTypeName();
    if (id != kContact && id != kElevatorContact) return false;
    return state(other, sFacing, kUp) == opposite(facing);
}

// ---- diode delay value box ---------------------------------------------------------------------

int diodeDelay(GP const& pos, Kind kind) {
    auto const* d = dataOf(pos);
    int         v = d ? d->value("delay", 0) : 0;
    if (v <= 0) v = cr::defaultDelay(diodeKind(kind));
    return std::clamp(v, cr::kMinDelay, cr::kMaxDelay);
}

void setDiodeDelay(GP const& pos, int ticks) {
    auto& d  = dataFor(pos);
    ticks    = std::clamp(ticks, cr::kMinDelay, cr::kMaxDelay);
    d["delay"] = ticks;
    // Create's onMaxDelayChanged: the counter stays within the new delay
    if (d.value("state", 0) > ticks) d["state"] = ticks;
    if (auto* rt = runtimeAt(pos); rt && rt->counterLoaded && rt->counter > ticks) rt->counter = ticks;
}

void registerValueBoxes() {
    for (auto kind : {Kind::PulseRepeater, Kind::PulseExtender, Kind::PulseTimer}) {
        common::valuebox::BoxSpec box;
        box.id            = "delay";
        box.label         = "create.logistics.redstone_interval";
        box.labelFallback = "Redstone Interval";
        box.board         = [](Block const&) {
            core::common::BoardSpec b;
            b.maxValue          = cr::kDelayBoardMax;
            b.milestoneInterval = cr::kDelayBoardMilestone;
            b.rows              = {
                tr("lv.redstone.unit.ticks", "Ticks"),
                tr("lv.redstone.unit.seconds", "Seconds"),
                tr("lv.redstone.unit.minutes", "Minutes"),
            };
            return b;
        };
        // Create's BrassDiodeScrollSlot: on top of the diode, centre (8, 2.6, 8)
        box.centre = [](Block const&, int face) -> std::optional<core::common::Vec> {
            if (face != kUp) return std::nullopt;
            return core::common::Vec{0.5, 2.6 / 16.0, 0.5};
        };
        box.scale = 0.5;
        box.get   = [kind](GP const& pos) {
            auto s = cr::delayToSettings(diodeDelay(pos, kind));
            return core::common::ValueSettings{s.row, s.value};
        };
        box.set = [](GP const& pos, core::common::ValueSettings s, Player&) {
            int ticks = cr::settingsToDelay({s.row, s.value});
            setDiodeDelay(pos, ticks);
            playSound(pos.dim, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, "random.click", 0.25f, 2.0f);
        };
        box.format = [](Block const&, core::common::ValueSettings s) { return cr::formatDelaySettings({s.row, s.value}); };
        common::valuebox::registerBox(idOf(kind), std::move(box));
    }
}

// ---- dispatch ----------------------------------------------------------------------------------

void tickBlock(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    switch (rt.kind) {
    case Kind::Link: tickLink(region, pos, block, rt); break;
    case Kind::Lever: tickLever(region, pos, block, rt); break;
    case Kind::PulseRepeater:
    case Kind::PulseExtender:
    case Kind::PulseTimer: tickBrassDiode(region, pos, block, rt); break;
    case Kind::PoweredLatch:
    case Kind::ToggleLatch: tickLatch(region, pos, block, rt); break;
    case Kind::SmartObserver: tickSmartObserver(region, pos, block, rt); break;
    case Kind::ThresholdSwitch: tickThresholdSwitch(region, pos, block, rt); break;
    case Kind::Contact: tickContact(region, pos, block, rt); break;
    }
    rt.neighbour = false;
}

bool use(Click const& c, Block const& block, Kind kind) {
    auto& region = c.player.getDimensionBlockSource();
    auto  bp     = game::blockPos(c.pos);
    bool  empty  = c.held.isNull();
    bool  wrench = holds(c.held, kWrench);
    switch (kind) {
    case Kind::Link: {
        if (!canBuild(c.player)) return false;
        // Create: sneak + empty hand or the wrench switches transmitter / receiver
        if ((c.sneaking && empty) || (wrench && !c.sneaking)) {
            toggleLinkMode(region, c.pos);
            return true;
        }
        if (c.sneaking || wrench || holds(c.held, kLinkedController)) return false;
        // Create's LinkHandler: every slot the click is on takes a copy of the held item (the
        // two hit areas overlap slightly in the middle), second slot first
        int  facing = state(block, sFacing, kUp);
        bool used   = false;
        auto name   = empty ? trFor(c.player, "lv.redstone.frequency_empty", "(empty)") : common::items::displayName(c.held);
        for (bool first : {false, true}) {
            if (!core::common::testBoxHit(c.hit, frequencySlotCentre(facing, first), kFrequencySlotScale)) continue;
            setFrequency(c.pos, first, c.held);
            playSound(c.pos.dim, c.pos.x + 0.5, c.pos.y + 0.5, c.pos.z + 0.5, "block.itemframe.add_item", 0.25f, 0.1f);
            actionBar(
                c.player,
                (first ? trFor(c.player, "create.logistics.firstFrequency", "Frequency #1")
                       : trFor(c.player, "create.logistics.secondFrequency", "Frequency #2"))
                    + ": " + name
            );
            log().info(
                "redstone link at {} frequency #{} set to {} by {}",
                posText(c.pos),
                first ? 1 : 2,
                empty ? "(empty)" : c.held.getTypeName(),
                c.player.getRealName()
            );
            used = true;
        }
        return used;
    }
    case Kind::Lever:
        // Create's useWithoutItem: any click but a sneaking one with an item in hand
        if (c.sneaking && !empty) return false;
        if (!canBuild(c.player)) return false;
        leverUse(region, c.pos, block, c.sneaking);
        return true;
    case Kind::PulseRepeater:
    case Kind::PulseExtender:
    case Kind::PulseTimer: {
        if (!canBuild(c.player) || c.sneaking || wrench) return false;
        bool inverted = state(block, sInverted) != 0;
        setStates(region, bp, {{sInverted, inverted ? 0 : 1}});
        playSound(c.pos.dim, c.pos.x + 0.5, c.pos.y + 0.5, c.pos.z + 0.5, "random.click", 0.3f, inverted ? 0.5f : 0.6f);
        log().info("{} at {} {}", idOf(kind), posText(c.pos), inverted ? "no longer inverted" : "inverted");
        return true;
    }
    case Kind::PoweredLatch:
    case Kind::ToggleLatch: {
        if (!canBuild(c.player) || c.sneaking || wrench) return false;
        bool powering = state(block, sPowering) != 0;
        if (kind == Kind::PoweredLatch) {
            cr::LatchState s{state(block, sPowered) != 0, state(block, sPoweredSide) != 0, powering};
            if (!cr::poweredLatchActivate(s)) return false;
        }
        setStates(region, bp, {{sPowering, powering ? 0 : 1}});
        playSound(c.pos.dim, c.pos.x + 0.5, c.pos.y + 0.5, c.pos.z + 0.5, "random.click", 0.3f, powering ? 0.5f : 0.6f);
        log().info("{} at {} switched {}", idOf(kind), posText(c.pos), powering ? "off" : "on");
        return true;
    }
    case Kind::ThresholdSwitch:
        if (wrench || (c.sneaking && !empty)) return false;
        openThresholdScreen(c.player, c.pos);
        return true;
    case Kind::SmartObserver:
    case Kind::Contact: return false;
    }
    return false;
}

} // namespace lv::feature::redstone
