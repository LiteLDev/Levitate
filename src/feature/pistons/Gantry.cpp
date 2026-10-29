// Gantries (Create's GantryShaftBlock / GantryCarriageBlockEntity / GantryContraptionEntity):
// shaft parts and redstone power, carriage assembly, and the moving gantry contraptions.

#include <algorithm>
#include <cmath>
#include <set>

#include "mc/world/actor/player/Player.h"

#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pistons/Pistons.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::pistons {

namespace ct = feature::contraption;

namespace {

constexpr char kDbShafts[] = "lv:pistons:shafts";

struct GantryState {
    std::set<GlobalPos>   shafts;
    bool                  shaftsDirty = false;
    std::set<GlobalPos>   queued; ///< carriages to assemble next tick
    std::set<ct::Id>      moving; ///< gantry contraptions
    std::uint64_t         ticks = 0;
};
GantryState& st() {
    static GantryState s;
    return s;
}

void persistShafts() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : st().shafts) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbShafts, j);
    st().shaftsDirty = false;
}

bool isShaft(GlobalPos const& q, cc::Dir facing) { return idAt(q) == kGantryShaft && facingAt(q) == facing; }

cp::ShaftPart partAt(GlobalPos const& q) { return static_cast<cp::ShaftPart>(std::clamp(stateOr(q, kPart, 3), 0, 3)); }

float pinionAt(GlobalPos const& shaft) {
    if (idAt(shaft) != kGantryShaft) return 0;
    return cp::pinionMovementSpeed(kinetics::speedAt(shaft));
}

bool canAssembleOnShaft(GlobalPos const& shaft) {
    if (idAt(shaft) != kGantryShaft) return false;
    return cp::canAssembleOn(partAt(shaft), stateOr(shaft, kPowered, 0) != 0, pinionAt(shaft));
}

/// Create's GantryShaftBlockEntity.checkAttachedCarriageBlocks.
void checkAttachedCarriages(GlobalPos const& shaft) {
    if (!canAssembleOnShaft(shaft)) return;
    auto axis = cc::axisOf(facingAt(shaft));
    for (auto d : cc::kAllDirs) {
        if (cc::axisOf(d) == axis) continue;
        auto q = rel(shaft, d);
        if (idAt(q) != kGantryCarriage || facingAt(q) != d) continue;
        st().queued.insert(q);
    }
}

void updatePart(GlobalPos const& q) {
    auto f    = facingAt(q);
    auto part = cp::shaftPart(isShaft(rel(q, f), f), isShaft(rel(q, cc::opposite(f)), f));
    auto old  = partAt(q);
    if (part == old) return;
    {
        EditGuard g;
        setStates(q, {{kPart, int(part)}});
    }
    // Create's onPlace: a shaft that became a middle / stopped being single re-checks carriages
    if ((old != cp::ShaftPart::Middle && part == cp::ShaftPart::Middle) || (old == cp::ShaftPart::Single && part != cp::ShaftPart::Single))
        checkAttachedCarriages(q);
}

bool shouldBePowered(GlobalPos const& q, cc::Dir facing) {
    bool power = contraption::ext::powered(q.dim, cpos(q));
    for (auto d : {cc::dirOf(cc::axisOf(facing), true), cc::dirOf(cc::axisOf(facing), false)}) {
        auto n = rel(q, d);
        if (!loaded(n) || !isShaft(n, facing)) continue;
        power = power || stateOr(n, kPowered, 0) != 0;
    }
    return power;
}

/// Create's GantryShaftBlock.neighborChanged (polled: custom blocks get no neighbour updates).
void updatePower(GlobalPos const& q) {
    auto facing = facingAt(q);
    bool prev   = stateOr(q, kPowered, 0) != 0;
    bool should = contraption::ext::powered(q.dim, cpos(q));
    if (!prev && !should && shouldBePowered(q, facing)) {
        EditGuard g;
        setStates(q, {{kPowered, 1}});
        return;
    }
    if (prev == should) return;
    std::vector<GlobalPos> toUpdate;
    auto                   axis = cc::axisOf(facing);
    for (auto d : {cc::dirOf(axis, true), cc::dirOf(axis, false)}) {
        auto cur = rel(q, d);
        for (int guard = 0; guard < 4096; ++guard) {
            if (!loaded(cur) || !isShaft(cur, facing)) break;
            bool curPowered = stateOr(cur, kPowered, 0) != 0;
            if (!should && curPowered && contraption::ext::powered(cur.dim, cpos(cur))) return;
            if (curPowered == should) break;
            toUpdate.push_back(cur);
            cur = rel(cur, d);
        }
    }
    toUpdate.push_back(q);
    {
        EditGuard g;
        for (auto const& u : toUpdate) setStates(u, {{kPowered, should ? 1 : 0}}, 2);
    }
    log().info("gantry shaft line at {} {} {} {} ({} blocks)", q.x, q.y, q.z, should ? "powered" : "unpowered", toUpdate.size());
}

void setCarriageError(GlobalPos const& carriage, std::string const& key, std::string const& fallback, std::vector<std::string> const& args) {
    auto& d     = data(carriage);
    d["error"]  = {{"key", key}, {"text", fallback}, {"args", args}};
    log().info("gantry carriage at {} {} {} could not assemble: {}", carriage.x, carriage.y, carriage.z, core::formatMessage(std::string(translate(key, fallback)), args));
}

/// Create's GantryCarriageBlockEntity.tryAssemble.
void tryAssemble(GlobalPos const& carriage) {
    if (idAt(carriage) != kGantryCarriage) return;
    auto facing = facingAt(carriage);
    auto shaft  = rel(carriage, cc::opposite(facing));
    if (idAt(shaft) != kGantryShaft) return;
    float   pinion   = pinionAt(shaft);
    cc::Dir shaftDir = facingAt(shaft);
    cc::Dir moveDir  = pinion < 0 ? cc::opposite(shaftDir) : shaftDir;

    ct::AssembleOptions opts;
    opts.anchor = shaft; // the shaft is Create's anchoring block, never moved
    // Create's moveGantryPinion: the block in front and gantry shafts along the pinion's axis
    opts.start = {cpos(carriage), cpos(rel(carriage, facing))};
    auto pinionAxis = cp::directionalAxis(facing, stateOr(carriage, kAxisFirst, 0) != 0);
    for (auto d : {cc::dirOf(pinionAxis, true), cc::dirOf(pinionAxis, false)}) {
        auto q = rel(carriage, d);
        if (idAt(q) == kGantryShaft && cc::axisOf(facingAt(q)) == pinionAxis) opts.start.push_back(cpos(q));
    }
    opts.kind = "gantry";
    auto dry    = ct::ext::dryRun(opts);
    if (!dry.ok()) {
        auto e = ct::ext::errorOf(dry);
        setCarriageError(carriage, e.key, e.fallback, e.args);
        return;
    }
    if (dry.blocks.empty()) return;
    data(carriage).erase("error");
    auto planned = ct::ext::structureAt(carriage.dim, dry.blocks, cpos(carriage));
    if (ct::ext::collidesWithWorld(carriage.dim, planned, cpos(carriage) + cc::offset(moveDir), moveDir)) return;

    auto r = ct::assemble(opts);
    if (!r.ok()) {
        setCarriageError(carriage, r.errorKey, r.errorText, r.errorArgs);
        return;
    }
    auto* c = ct::find(*r.id);
    if (!c) return;
    // Create's limitMovement: a sequenced gearshift's "turn to move" instruction reaching the shaft
    // stops the gantry after exactly that distance (sequencedOffsetLimit, -1: none)
    double limit = -1;
    if (auto context = kinetics::sequenceContextAt(shaft); context && context->kind == kinetics::SequenceContext::Kind::MoveDistance)
        limit = context->value;
    c->anchorData()["gantry"] = {
        {"facing", int(facing)  },
        {"axis",   int(shaftDir)},
        {"ticks",  0            },
        {"seq",    limit        }
    };
    st().moving.insert(*r.id);
    sound(carriage, "open.wooden_trapdoor", 0.5f, 0.5f);
    log().info("gantry carriage at {} {} {} assembled: {} blocks", carriage.x, carriage.y, carriage.z, c->blocks().size());
}

void disassembleGantry(ct::Contraption& c, char const* why) {
    auto id  = c.id();
    auto pos = c.placement(cc::offset(static_cast<cc::Dir>(c.anchorData()["gantry"].value("facing", 1))));
    log().info("gantry contraption #{} stops at {} {} {} ({})", id, pos.x, pos.y, pos.z, why);
    ct::disassemble(id);
    sound(pos, "close.wooden_trapdoor", 0.35f, 0.75f);
    st().moving.erase(id);
}

/// Create's GantryContraptionEntity.tickContraption + checkPinionShaft.
void tickGantry(ct::Id id) {
    auto* c = ct::find(id);
    if (!c) {
        st().moving.erase(id);
        return;
    }
    auto& g = c->anchorData()["gantry"];
    if (!g.is_object()) {
        st().moving.erase(id);
        return;
    }

    auto facing = static_cast<cc::Dir>(std::clamp(g.value("facing", 1), 0, 5));
    int  ticks  = g.value("ticks", 0) + 1;
    g["ticks"]  = ticks;

    // stalled by an actor: the target set last tick was not applied
    bool stalled = false;
    if (g.contains("target")) {
        auto const& t = g["target"];
        cc::Vec3d   want{t[0].get<double>(), t[1].get<double>(), t[2].get<double>()};
        stalled = (c->transform().offset - want).lengthSqr() > 1e-6;
    }

    cc::Vec3d current = c->toWorld(cc::Vec3d::center(cc::offset(facing)));
    cc::Pos   cell{int(std::floor(current.x)), int(std::floor(current.y)), int(std::floor(current.z))};
    GlobalPos shaft = rel(gpos(c->dimension(), cell), cc::opposite(facing));
    if (!loaded(shaft)) return;
    if (idAt(shaft) != kGantryShaft) {
        disassembleGantry(*c, "no shaft");
        return;
    }
    cc::Dir shaftDir = facingAt(shaft);
    float   pinion   = pinionAt(shaft);
    if (stateOr(shaft, kPowered, 0) != 0 || pinion == 0) {
        disassembleGantry(*c, pinion == 0 ? "stopped" : "powered");
        return;
    }
    double limit = g.value("seq", -1.0);
    if (limit >= 0) pinion = static_cast<float>(std::clamp<double>(pinion, -limit, limit));
    cc::Vec3d motion = cc::Vec3d::of(cc::offset(shaftDir)) * pinion;
    cc::Vec3d next   = current + motion;
    auto      axis   = cc::axisOf(shaftDir);
    if (cp::crossesShaftCentre(current.get(axis), next.get(axis), pinion, shaftDir) && !canAssembleOnShaft(shaft)) {
        disassembleGantry(*c, "end of the shaft");
        return;
    }
    // ContraptionCollider.collideBlocks with this tick's motion
    cc::Dir dir    = pinion * (cc::positive(shaftDir) ? 1 : -1) >= 0 ? cc::dirOf(axis, true) : cc::dirOf(axis, false);
    auto    origin = c->transform().origin + c->transform().offset;
    auto    grid   = cp::collisionGrid(origin, dir);
    if (pinion != 0 && ct::ext::collidesWithWorld(c->dimension(), ct::ext::structureOf(*c), grid, dir)) {
        disassembleGantry(*c, "blocked");
        return;
    }
    if (!stalled && ticks > 2) {
        auto t   = c->transform();
        t.offset = t.offset + motion;
        c->setTransform(t);
        g["target"] = Json::array({t.offset.x, t.offset.y, t.offset.z});
        if (limit > 0) g["seq"] = std::max(0.0, limit - std::abs(pinion));
    } else if (stalled && g.contains("target")) {
        auto t      = c->transform();
        g["target"] = Json::array({t.offset.x, t.offset.y, t.offset.z});
    }
}

} // namespace

void trackShaft(GlobalPos const& p) {
    if (st().shafts.insert(p).second) st().shaftsDirty = true;
}

void onShaftSpeed(GlobalPos const& p) { checkAttachedCarriages(p); }

void checkCarriage(GlobalPos const& carriage) {
    if (idAt(carriage) != kGantryCarriage) return;
    auto shaft = rel(carriage, cc::opposite(facingAt(carriage)));
    if (idAt(shaft) != kGantryShaft) return;
    if (stateOr(shaft, kPowered, 0) != 0) return;
    if (!canAssembleOnShaft(shaft)) return;
    st().queued.insert(carriage);
}

void orientShaft(GlobalPos const& p, Player& player, cc::Dir clickedFace) {
    bool    sneak = player.isSneaking();
    auto    pref  = preferredSide(p, false);
    cc::Dir facing;
    if (!pref || sneak) {
        auto look = nearestLooking(player);
        facing    = sneak ? look : cc::opposite(look);
    } else {
        facing = cc::opposite(*pref);
    }
    // Create: continue a neighbouring shaft line along the same axis (sneaking: reversed)
    auto behind  = rel(p, cc::opposite(facing));
    auto clicked = rel(p, cc::opposite(clickedFace));
    auto other   = idAt(behind) == kGantryShaft ? behind : clicked;
    if (idAt(other) == kGantryShaft && cc::axisOf(facingAt(other)) == cc::axisOf(facing)) {
        auto f = facingAt(other);
        facing = sneak ? cc::opposite(f) : f;
    }
    {
        EditGuard g;
        setStates(p, {{kFacing, int(facing)}, {kPowered, shouldBePowered(p, facing) ? 1 : 0}});
    }
    trackShaft(p);
    updatePart(p);
    for (auto d : cc::kAllDirs)
        if (idAt(rel(p, d)) == kGantryShaft) updatePart(rel(p, d));
}

void orientCarriage(GlobalPos const& p, cc::Dir clickedFace) {
    EditGuard g;
    if (facingAt(p) != clickedFace) setStates(p, {{kFacing, int(clickedFace)}});
    // Create's cycleAxisIfNecessary against the shaft behind
    auto facing = facingAt(p);
    auto shaft  = rel(p, cc::opposite(facing));
    if (idAt(shaft) != kGantryShaft) return;
    auto shaftAxis = cc::axisOf(facingAt(shaft));
    if (shaftAxis == cc::axisOf(facing)) return;
    bool af       = stateOr(p, kAxisFirst, 0) != 0;
    auto rotation = cp::directionalAxis(facing, af);
    cc::Axis valid = cc::Axis::Y;
    for (auto a : {cc::Axis::X, cc::Axis::Y, cc::Axis::Z})
        if (a != rotation && a != cc::axisOf(facing)) valid = a;
    if (valid != shaftAxis) setStates(p, {{kAxisFirst, af ? 0 : 1}});
}

std::vector<std::uint64_t> gantryContraptions() { return {st().moving.begin(), st().moving.end()}; }

void tickGantries() {
    auto& s = st();
    // queued carriage assemblies (Create: assembleNextTick)
    auto queued = std::move(s.queued);
    s.queued.clear();
    for (auto const& q : queued) {
        if (!loaded(q)) continue;
        try {
            tryAssemble(q);
        } catch (std::exception const& e) {
            log().error("gantry carriage at {} {} {}: {}", q.x, q.y, q.z, e.what());
        }
    }
    // shafts: parts and redstone power (polled every 2 ticks: custom blocks get no neighbour updates)
    std::vector<GlobalPos> shafts;
    if (currentTick() % 2 == 0) shafts.assign(s.shafts.begin(), s.shafts.end());
    for (auto const& q : shafts) {
        if (!loaded(q)) continue;
        if (idAt(q) != kGantryShaft) {
            s.shafts.erase(q);
            s.shaftsDirty = true;
            continue;
        }
        updatePart(q);
        updatePower(q);
    }
    if (s.shaftsDirty) persistShafts();
    // moving gantries are contraptions of kind "gantry" (state in their anchor data); the engine
    // loads them with the world
    if (s.ticks++ % 20 == 0) {
        for (auto id : ct::all())
            if (auto* c = ct::find(id); c && c->kind() == "gantry") s.moving.insert(id);
    }
    for (auto id : std::vector<ct::Id>(s.moving.begin(), s.moving.end())) {
        try {
            tickGantry(id);
        } catch (std::exception const& e) {
            log().error("gantry contraption #{}: {}", id, e.what());
        }
    }
}

void resetGantries() { st() = GantryState{}; }

void loadGantries() {
    st() = GantryState{};
    if (auto j = game::db::load(kDbShafts); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) st().shafts.insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
}

} // namespace lv::feature::pistons
