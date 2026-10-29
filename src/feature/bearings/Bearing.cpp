// Mechanical, windmill and clockwork bearings (Create's MechanicalBearingBlockEntity,
// WindmillBearingBlockEntity, ClockworkBearingBlockEntity): kinetic specs, assembly through the
// contraption engine, rotation each tick, disassembly rules and their value boxes.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "ll/api/service/Bedrock.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/BlockData.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::bearings {

namespace k  = ::lv::feature::kinetics;
namespace ct = ::lv::feature::contraption;
namespace bd = ::lv::feature::common::blockdata;
namespace vb = ::lv::feature::common::valuebox;
namespace ck = ::lv::core::kinetics;

namespace {

// ---- kinetic specs ------------------------------------------------------------------------------

ck::KineticSpec bearingSpec(bool generator, std::string texture, bool rotatingTop) {
    ck::KineticSpec s;
    s.states          = {"levitate:facing"};
    s.rotationAxis    = ck::specs::axisFromFacing("levitate:facing");
    s.hasShaftTowards = [](ck::BlockState const& st, ck::Face f) {
        return f == ck::opposite(static_cast<ck::Face>(std::clamp(st.get("levitate:facing", 1), 0, 5)));
    };
    s.generator    = generator;
    s.rotorState   = "levitate:rotor";
    s.rotorModel   = "geometry.levitate.bearing_rotor";
    s.rotorTexture = std::move(texture);
    s.parts        = [rotatingTop](ck::RenderView const& v, std::vector<ck::RotorPart>& out) {
        auto          facing = static_cast<ck::Face>(std::clamp(v.state ? v.state->get("levitate:facing", 1) : 1, 0, 5));
        ck::RotorPart shaft;
        shaft.bone = ck::isPositive(facing) ? "shaft_pos" : "shaft_neg";
        shaft.axis = ck::axisOf(facing);
        out.push_back(shaft);
        if (!rotatingTop) return;
        ck::RotorPart top;
        top.bone = ck::isPositive(facing) ? "top_pos" : "top_neg";
        top.axis = ck::axisOf(facing);
        out.push_back(top);
    };
    return s;
}

LV_REGISTER_KINETIC_BLOCK("levitate:mechanical_bearing", bearingSpec(false, "levitate_mechanical_bearing_model", true));
LV_REGISTER_KINETIC_BLOCK("levitate:windmill_bearing", bearingSpec(true, "levitate_windmill_bearing_model", true));
LV_REGISTER_KINETIC_BLOCK("levitate:clockwork_bearing", bearingSpec(false, "levitate_clockwork_bearing_model", false));

// ---- state ---------------------------------------------------------------------------------------

/// What Create keeps in the bearing's block entity (BlockData "bearings").
struct State {
    bool     running  = false;
    double   angle    = 0;     ///< first structure (clockwork: the hour hand's angle)
    ct::Id   id       = 0;     ///< first structure
    ct::Id   second   = 0;     ///< clockwork: second structure
    double   angle2   = 0;     ///< clockwork: minute hand's angle
    int      mode     = 0;     ///< RotationMode / windmill direction / ClockHands
    bool     next     = false; ///< assembleNextTick
    float    speed    = 0;     ///< last theoretical speed seen (Create's onSpeedChanged)
    int      sails    = 0;
    float    gen      = 0;     ///< windmill: generated speed
    bool     placeBack = false; ///< windmill: angle set to 0, disassemble next tick
    double   limit    = -1;    ///< Create's sequencedAngleLimit (degrees left; -1: none)
    std::uint64_t seq = 0;     ///< serial of the sequence context the limit came from
    std::string  error;
    std::int64_t by = 0;       ///< player who asked for the assembly (messages)

    static State of(Json const& j) {
        State s;
        if (!j.is_object()) return s;
        s.running   = j.value("running", false);
        s.angle     = j.value("angle", 0.0);
        s.id        = j.value("id", ct::Id{0});
        s.second    = j.value("second", ct::Id{0});
        s.angle2    = j.value("angle2", 0.0);
        s.mode      = j.value("mode", 0);
        s.next      = j.value("next", false);
        s.speed     = j.value("speed", 0.f);
        s.sails     = j.value("sails", 0);
        s.gen       = j.value("gen", 0.f);
        s.placeBack = j.value("placeBack", false);
        s.limit     = j.value("limit", -1.0);
        s.seq       = j.value("seq", std::uint64_t{0});
        s.error     = j.value("error", std::string{});
        s.by        = j.value("by", std::int64_t{0});
        return s;
    }
    [[nodiscard]] Json json() const {
        return {
            {"running",   running  },
            {"angle",     angle    },
            {"id",        id       },
            {"second",    second   },
            {"angle2",    angle2   },
            {"mode",      mode     },
            {"next",      next     },
            {"speed",     speed    },
            {"sails",     sails    },
            {"gen",       gen      },
            {"placeBack", placeBack},
            {"limit",     limit    },
            {"seq",       seq      },
            {"error",     error    },
            {"by",        by       },
        };
    }
};

State load(GP const& pos) {
    auto const* d = bd::get(pos);
    if (!d) return {};
    auto it = d->find("bearings");
    return it == d->end() ? State{} : State::of(*it);
}

void store(GP const& pos, State const& s) {
    auto j = s.json();
    if (auto const* d = bd::get(pos)) {
        auto it = d->find("bearings");
        if (it != d->end() && *it == j) return; // unchanged: keep the chunk clean
    }
    bd::edit(pos)["bearings"] = std::move(j);
}

/// Contraptions held by running bearings (cleanup when the bearing block disappears).
std::unordered_map<GP, std::vector<ct::Id>, ck::GlobalPosHash>& anchored() {
    static std::unordered_map<GP, std::vector<ct::Id>, ck::GlobalPosHash> m;
    return m;
}

void remember(GP const& pos, State const& s) {
    if (!s.running) {
        anchored().erase(pos);
        return;
    }
    std::vector<ct::Id> ids;
    if (s.id) ids.push_back(s.id);
    if (s.second) ids.push_back(s.second);
    anchored()[pos] = ids;
}

std::string kindName(Kind kind) {
    switch (kind) {
    case Kind::Windmill:
        return "windmill_bearing";
    case Kind::Clockwork:
        return "clockwork_bearing";
    default:
        return "mechanical_bearing";
    }
}

cc::Vec3d centerOf(GP const& p) { return {p.x + .5, p.y + .5, p.z + .5}; }

void report(State& s, GP const& pos, std::string key, std::string text) {
    s.error = key;
    log().info("bearing at {} could not assemble: {}", pos.key(), text);
    if (auto* p = playerById(s.by)) {
        actionBar(*p, trFor(*p, "create.gui.assembly.exception", "This Contraption was unable to assemble:") + " " + text);
    }
}

/// Assembles the structure in front of a bearing (`offset` blocks in front). Returns the id.
std::optional<ct::Id> assembleFront(State& s, BlockSource& region, GP const& pos, cc::Dir facing, int offset, Kind kind) {
    cc::Pos front = ccp(pos) + cc::offset(facing) * offset;
    if (game::replaceable(region, bp(front))) return std::nullopt; // nothing attached (Create: no exception)
    ct::AssembleOptions o;
    o.anchor = pos;
    o.start  = {front};
    o.axis   = cc::axisOf(facing);
    o.pivot  = cc::Vec3d{0.5, 0.5, 0.5} + cc::Vec3d::of(cc::offset(facing) * offset);
    o.kind   = kindName(kind);
    auto r   = ct::assemble(o);
    if (!r.ok()) {
        report(s, pos, r.errorKey, r.errorText);
        return std::nullopt;
    }
    return r.id;
}

void applyRotation(ct::Id id, double angle, cc::Axis axis) {
    auto* c = ct::find(id);
    if (!c) return;
    auto t  = c->transform();
    t.angle = angle;
    t.axis  = axis;
    c->setTransform(t);
}

/// The contraption didn't take the last angle (an actor stalled it): Create's isStalled.
bool stalled(ct::Id id, double& angle) {
    auto* c = ct::find(id);
    if (!c) return false;
    double now = c->transform().angle;
    if (std::abs(now - angle) < 1e-4) return false;
    angle = now;
    return true;
}

int countSails(ct::Contraption const& c) {
    int n = 0;
    for (auto const& b : c.blocks())
        if (isWindmillSail(b.id)) ++n;
    return n;
}

// ---- mechanical / windmill ----------------------------------------------------------------------

void disassemble(State& s, GP const& pos, Kind kind) {
    if (!s.running && !s.id) return;
    s.angle = 0;
    if (s.id && ct::find(s.id)) {
        ct::disassemble(s.id);
        disassembleSound(pos.dim, centerOf(pos));
    }
    if (s.second && ct::find(s.second)) ct::disassemble(s.second);
    log().info("{} at {} disassembled", kindName(kind), pos.key());
    s.id = s.second = 0;
    s.angle2    = 0;
    s.running   = false;
    s.next      = false;
    s.placeBack = false;
    if (kind == Kind::Windmill) {
        s.gen = 0;
        k::setGeneratedSpeed(pos, 0);
    }
}

void assemble(State& s, BlockSource& region, GP const& pos, cc::Dir facing, Kind kind) {
    auto id = assembleFront(s, region, pos, facing, 1, kind);
    if (!id) return;
    auto* c = ct::find(*id);
    if (kind == Kind::Windmill) {
        int sails = c ? countSails(*c) : 0;
        int min   = config::get<Config>().minimumWindmillSails;
        if (sails < min) {
            ct::disassemble(*id); // Create checks before lifting: put it back unchanged
            report(
                s,
                pos,
                "create.gui.assembly.exception.not_enough_sails",
                tr("create.gui.assembly.exception.not_enough_sails",
                   "Attached structure does not include enough sail-like blocks: {0}\nA minimum of {1} are required",
                   sails,
                   min)
            );
            return;
        }
        s.sails = sails;
    }
    s.error   = "";
    s.id      = *id;
    s.running = true;
    s.angle   = 0;
    assembleSound(pos.dim, centerOf(pos));
    log().info("{} at {} assembled contraption #{} ({} blocks)", kindName(kind), pos.key(), *id, c ? c->blocks().size() : 0);
    if (kind == Kind::Windmill) {
        s.gen = cb::windmillSpeed(s.sails, config::get<Config>().windmillSailsPerRPM, s.mode == 0);
        k::setGeneratedSpeed(pos, s.gen);
        log().info("windmill_bearing at {}: {} sails, {} RPM", pos.key(), s.sails, s.gen);
    }
}

/// Create's sequencedAngleLimit: a sequenced gearshift turning by an angle reaches the bearing
/// (scaled by the speed ratio); the bearing stops after exactly that angle. Read on a speed change
/// and when a new instruction arrives at an unchanged speed (two equal steps in a row).
void readSequenceLimit(State& s, GP const& pos, Kind kind, bool speedChanged) {
    if (kind == Kind::Windmill) return;
    auto context = k::sequenceContextAt(pos);
    if (context && context->serial == s.seq) return; // an instruction is read once
    if (!speedChanged && !context) return;
    s.limit = -1;
    s.seq   = context ? context->serial : 0;
    if (context && context->kind == k::SequenceContext::Kind::TurnAngle) s.limit = context->value;
}

void tickRotating(State& s, BlockSource& region, GP const& pos, cc::Dir facing, Kind kind) {
    float theo = k::theoreticalSpeedAt(pos);
    readSequenceLimit(s, pos, kind, theo != s.speed);
    if (theo != s.speed) {
        float prev = s.speed;
        s.speed    = theo;
        // Create's onSpeedChanged (a windmill's own generated speed doesn't assemble it again)
        if (kind != Kind::Windmill || s.running) s.next = true;
        if (s.id && ct::find(s.id) && prev != 0 && (prev > 0) != (theo > 0)) {
            double probe = s.angle;
            if (!stalled(s.id, probe)) {
                s.angle = std::round(s.angle);
                applyRotation(s.id, s.angle, cc::axisOf(facing));
            }
        }
    }
    if (s.placeBack) {
        // windmill: placed back at the assembly angle (Create sets the angle to 0 first)
        auto* c = ct::find(s.id);
        if (!c || std::abs(c->transform().angle) < 1e-6) disassemble(s, pos, kind);
        else applyRotation(s.id, 0, cc::axisOf(facing));
        return;
    }
    if (s.next) {
        s.next = false;
        if (s.running) {
            auto* c   = ct::find(s.id);
            bool  can = kind == Kind::Windmill || cb::canDisassemble(static_cast<cb::RotationMode>(s.mode), s.angle);
            if (theo == 0 && (can || !c || c->blocks().empty())) {
                disassemble(s, pos, kind);
                return;
            }
        } else {
            if (theo == 0 && kind != Kind::Windmill) return;
            assemble(s, region, pos, facing, kind);
        }
    }
    if (!s.running) return;
    if (!ct::find(s.id)) {
        // the structure is gone (removed by a command or another unit): Create's bearing stops
        log().info("{} at {}: contraption #{} is gone", kindName(kind), pos.key(), s.id);
        s.running = false;
        s.id      = 0;
        s.angle   = 0;
        if (kind == Kind::Windmill) k::setGeneratedSpeed(pos, s.gen = 0);
        return;
    }
    if (!stalled(s.id, s.angle)) {
        float speed   = kind == Kind::Windmill ? s.gen : k::speedAt(pos);
        float angular = cb::angularSpeed(speed);
        if (k::speedAt(pos) == 0) angular = 0;
        if (s.limit >= 0) {
            angular = static_cast<float>(std::clamp<double>(angular, -s.limit, s.limit));
            s.limit = std::max(0.0, s.limit - std::abs(angular));
        }
        s.angle = cb::advanceAngle(s.angle, angular);
    }
    applyRotation(s.id, s.angle, cc::axisOf(facing));
}

// ---- clockwork ----------------------------------------------------------------------------------

void assembleClock(State& s, BlockSource& region, GP const& pos, cc::Dir facing) {
    auto first = assembleFront(s, region, pos, facing, 1, Kind::Clockwork);
    if (!first) return;
    auto* c = ct::find(*first);
    // Create's ClockworkContraption: the hour arm's blocks straight in front of the bearing,
    // then the minute arm starts behind them
    int armBlocks = 0;
    for (int i = 0; i < 16; ++i) {
        cc::Pos local = cc::offset(facing) * (i + 1);
        bool    has   = false;
        if (c)
            for (auto const& b : c->blocks())
                if (b.local == local) has = true;
        if (has) continue;
        armBlocks = i;
        break;
    }
    std::optional<ct::Id> second;
    if (armBlocks > 0) {
        cc::Pos start = ccp(pos) + cc::offset(facing) * (armBlocks + 1);
        if (!game::replaceable(region, bp(start))) {
            second = assembleFront(s, region, pos, facing, armBlocks + 1, Kind::Clockwork);
            if (!second) {
                ct::disassemble(*first); // the whole clock fails (Create returns null)
                return;
            }
        }
    }
    s.error   = "";
    s.id      = *first;
    s.second  = second.value_or(0);
    s.running = true;
    s.angle = s.angle2 = 0;
    assembleSound(pos.dim, centerOf(pos));
    log().info("clockwork_bearing at {} assembled contraption #{} and #{}", pos.key(), s.id, s.second);
}

void tickClock(State& s, BlockSource& region, GP const& pos, cc::Dir facing) {
    float theo = k::theoreticalSpeedAt(pos);
    if (theo != s.speed) {
        s.speed = theo;
        s.next  = true;
    }
    if (s.next) {
        s.next = false;
        if (s.running) {
            if (theo == 0) disassemble(s, pos, Kind::Clockwork);
        } else {
            assembleClock(s, region, pos, facing);
        }
        return;
    }
    if (!s.running) return;
    if (!ct::find(s.id)) {
        log().info("clockwork_bearing at {}: contraption #{} is gone", pos.key(), s.id);
        if (s.second && ct::find(s.second)) ct::disassemble(s.second);
        s.running = false;
        s.id = s.second = 0;
        return;
    }
    auto  mode     = static_cast<cb::ClockHands>(s.mode);
    bool  hourFirst = mode != cb::ClockHands::MinuteFirst;
    int   step     = cc::positive(facing) ? 1 : -1;
    auto  level    = ll::service::getLevel();
    long long time = level ? level->getTime() : 0;
    float maxSpeed = std::abs(cb::angularSpeed(k::speedAt(pos)));
    // the first structure is the hour hand unless the minute hand comes first
    double& hourAngle   = hourFirst ? s.angle : s.angle2;
    double& minuteAngle = hourFirst ? s.angle2 : s.angle;
    ct::Id  hourId      = hourFirst ? s.id : s.second;
    ct::Id  minuteId    = hourFirst ? s.second : s.id;
    if (hourId && !stalled(hourId, hourAngle)) {
        double target = cb::hourTarget(time, step, mode == cb::ClockHands::HourFirst24);
        hourAngle     = cb::advanceAngle(hourAngle, cb::handStep(hourAngle, target, maxSpeed / 2));
    }
    if (minuteId && !stalled(minuteId, minuteAngle)) {
        double target = cb::minuteTarget(time, step);
        minuteAngle   = cb::advanceAngle(minuteAngle, cb::handStep(minuteAngle, target, maxSpeed));
    }
    if (hourId) applyRotation(hourId, hourAngle, cc::axisOf(facing));
    if (minuteId) applyRotation(minuteId, minuteAngle, cc::axisOf(facing));
}

// ---- value boxes -----------------------------------------------------------------------------------

/// Create's DirectionalExtenderScrollOptionSlot: the centre of a side face (not on the bearing's
/// axis), 2 px towards the back.
std::optional<core::common::Vec> sideSlot(Block const& block, int face) {
    auto facing = facingOf(block);
    auto f      = static_cast<cc::Dir>(std::clamp(face, 0, 5));
    if (cc::axisOf(f) == cc::axisOf(facing)) return std::nullopt;
    auto n = cc::Vec3d::of(cc::offset(f));
    auto b = cc::Vec3d::of(cc::offset(facing));
    auto c = cc::Vec3d{0.5, 0.5, 0.5} + n * 0.5 - b * (2.0 / 16.0);
    return core::common::Vec{c.x, c.y, c.z};
}

void registerOptionBox(
    std::string_view                   blockId,
    std::string                        boxId,
    std::string                        label,
    std::string                        labelFallback,
    std::vector<std::pair<std::string, std::string>> options,
    std::function<void(GP const&, State&)> changed = {}
) {
    vb::BoxSpec box;
    box.id            = std::move(boxId);
    box.label         = label;
    box.labelFallback = labelFallback;
    int count         = static_cast<int>(options.size());
    box.board         = [count, label, labelFallback](Block const&) {
        return core::common::optionBoard(count, tr(label, labelFallback));
    };
    box.centre = sideSlot;
    box.get    = [](GP const& p) { return core::common::ValueSettings{0, load(p).mode}; };
    box.set    = [count, changed](GP const& p, core::common::ValueSettings v, Player&) {
        auto s = load(p);
        s.mode = std::clamp(v.value, 0, count - 1);
        if (changed) changed(p, s);
        store(p, s);
    };
    box.format = [options](Block const&, core::common::ValueSettings v) {
        if (v.value < 0 || v.value >= static_cast<int>(options.size())) return std::to_string(v.value);
        return tr(options[v.value].first, options[v.value].second);
    };
    vb::registerBox(std::string(blockId), std::move(box));
}

} // namespace


// ---- API of the unit ------------------------------------------------------------------------------

void registerBearingBoxes() {
    static bool erasing = false;
    if (!erasing) {
        erasing = true;
        // the bearing's data goes with its block: its structures are placed (Create's remove()),
        // also when the bearing was never ticked since the world loaded
        bd::onErase([](GP const& pos, Json const& doc, std::string const&) {
            auto it = doc.find("bearings");
            if (it == doc.end()) return;
            auto s = State::of(*it);
            if (!s.running) return;
            std::vector<ct::Id> ids;
            if (s.id) ids.push_back(s.id);
            if (s.second) ids.push_back(s.second);
            defer([pos, ids] {
                for (auto id : ids) {
                    if (!ct::find(id)) continue;
                    ct::disassemble(id);
                    log().info("bearing at {} removed: contraption #{} disassembled", pos.key(), id);
                }
                anchored().erase(pos);
            });
        });
    }
    registerOptionBox(
        kMechanicalBearing,
        "mode",
        "create.contraptions.movement_mode",
        "Movement Mode",
        {
            {"create.contraptions.movement_mode.rotate_place",          "Always Place when Stopped"       },
            {"create.contraptions.movement_mode.rotate_place_returned", "Only Place near Initial Angle"   },
            {"create.contraptions.movement_mode.rotate_never_place",    "Only Place when Anchor Destroyed"},
        }
    );
    registerOptionBox(
        kWindmillBearing,
        "direction",
        "create.contraptions.windmill.rotation_direction",
        "Rotation Direction",
        {
            {"create.generic.clockwise",         "Clockwise"        },
            {"create.generic.counter_clockwise", "Counter-Clockwise"},
        },
        [](GP const& p, State& s) {
            // Create's onDirectionChanged: a running windmill turns the other way at once
            if (!s.running) return;
            s.gen = cb::windmillSpeed(s.sails, config::get<Config>().windmillSailsPerRPM, s.mode == 0);
            k::setGeneratedSpeed(p, s.gen);
        }
    );
    registerOptionBox(
        kClockworkBearing,
        "hands",
        "create.contraptions.clockwork.clock_hands",
        "Clock Hands",
        {
            {"create.contraptions.clockwork.hour_first",    "Hour hand first"   },
            {"create.contraptions.clockwork.minute_first",  "Minute hand first" },
            {"create.contraptions.clockwork.hour_first_24", "24-Hour hand first"},
        }
    );
}

void bearingUse(Player& player, GP const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto kind = kindOf(idAt(*region, pos));
    if (!kind || *kind == Kind::Turntable || *kind == Kind::CartAssembler || *kind == Kind::ControllerRail) return;
    track(pos);
    auto s = load(pos);
    if (s.running) {
        // Create: a click while running places the structure at once (a windmill first turns
        // back to its assembly angle: placed on the next tick)
        if (*kind == Kind::Windmill && ct::find(s.id)) s.placeBack = true;
        else disassemble(s, pos, *kind);
    } else {
        s.next = true;
        s.by   = uidOf(player);
    }
    store(pos, s);
    remember(pos, s);
    log().info("{} at {} used by {}", kindName(*kind), pos.key(), player.getRealName());
}

void tickBearing(BlockSource& region, GP const& pos, Kind kind) {
    auto const& block  = region.getBlock(bp(pos));
    auto        facing = facingOf(block);
    auto        s      = load(pos);
    if (kind == Kind::Clockwork) tickClock(s, region, pos, facing);
    else tickRotating(s, region, pos, facing, kind);
    store(pos, s);
    remember(pos, s);
}

void bearingRemoved(GP const& pos) {
    auto it = anchored().find(pos);
    if (it == anchored().end()) return;
    for (auto id : it->second) {
        if (ct::find(id)) {
            ct::disassemble(id); // Create's remove(): the structure is placed back
            log().info("bearing at {} removed: contraption #{} disassembled", pos.key(), id);
        }
    }
    anchored().erase(it);
}

void forgetBearings() { anchored().clear(); }

Json bearingState(GP const& pos) { return load(pos).json(); }

void orientBearing(BlockSource& region, GP const& pos, Player& player) {
    // Create's DirectionalKineticBlock#getStateForPlacement
    std::optional<cc::Dir> preferred;
    bool                   conflict = false;
    for (auto d : cc::kAllDirs) {
        GP          n     = gp(pos.dim, ccp(pos) + cc::offset(d));
        if (!loaded(region, n)) continue;
        auto const& block = region.getBlock(bp(n));
        auto const* spec  = k::specOf(block.getTypeName());
        if (!spec) continue;
        ck::BlockState st;
        st.id = block.getTypeName();
        for (auto const& name : spec->states) st.values.emplace_back(name, game::intState(block, name).value_or(0));
        if (!spec->shaftTowards(st, static_cast<ck::Face>(cc::opposite(d)))) continue;
        if (preferred && cc::axisOf(*preferred) != cc::axisOf(d)) {
            conflict = true;
            break;
        }
        preferred = d;
    }
    if (conflict) preferred.reset();
    cc::Dir facing;
    if (!preferred || player.isSneaking()) {
        Vec3    look = player.getViewVector();
        cc::Dir near = cc::Dir::North;
        double  best = -1;
        for (auto d : cc::kAllDirs) {
            auto   o   = cc::offset(d);
            double dot = o.x * look.x + o.y * look.y + o.z * look.z;
            if (dot > best) {
                best = dot;
                near = d;
            }
        }
        facing = player.isSneaking() ? near : cc::opposite(near);
    } else {
        facing = cc::opposite(*preferred);
    }
    setStates(region, pos, {{"levitate:facing", static_cast<int>(facing)}});
}

} // namespace lv::feature::bearings
