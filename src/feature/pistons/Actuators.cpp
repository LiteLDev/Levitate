// Linear actuators (Create's LinearActuatorBlockEntity and its subclasses): mechanical pistons,
// rope pulleys and elevator pulleys. State lives in BlockData (`data["pistons"]`):
//   running, offset, waiting (waitingForSpeedChange), assemble (assembleNextTick), mode (movement
//   mode), id (contraption), len (piston extension length), init (initial offset), needs
//   (needsContraption), error {key, args}, parent / children (mirrored pulleys), target (last
//   transform offset set, to notice stalls), elevator: prev (speed), arrived, ey (anchor y),
//   col {x, z, side}, cy (contact y offset), minY / maxY (reachable contact levels).

#include <algorithm>
#include <cmath>
#include <set>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pistons/Pistons.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::pistons {

namespace ct = feature::contraption;

namespace {

constexpr char kDbActive[] = "lv:pistons:active";

std::set<GlobalPos>& active() {
    static std::set<GlobalPos> s;
    return s;
}

void persistActive() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : active()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbActive, j);
}

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(int dim, Json const& j) {
    if (!j.is_array() || j.size() < 3) return std::nullopt;
    return GlobalPos{dim, j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

int maxRope() { return std::max(1, config::get<Config>().maxRopeLength); }

int rangeOf(GlobalPos const& p, Kind k, Json const& d) {
    if (k == Kind::Piston) return d.value("len", 0);
    auto* r    = regionOf(p.dim);
    int   minY = r ? r->getMinHeight() : -64;
    return std::max(0, std::min(maxRope(), (p.y - 1) - minY));
}

/// Create's getSpeed(): the effective speed (0 when overstressed).
float rpmAt(GlobalPos const& p) { return kinetics::speedAt(p); }

cc::Vec3d displacementOf(cc::Dir facing, Kind k, float offset, int init) {
    if (k == Kind::Piston) return cc::Vec3d::of(cc::offset(facing)) * (static_cast<double>(offset) - init);
    return {0, static_cast<double>(init) - offset, 0};
}

cc::Dir motionDir(cc::Dir facing, Kind k, float v) {
    if (k == Kind::Piston) return v >= 0 ? facing : cc::opposite(facing);
    return v >= 0 ? cc::Dir::Down : cc::Dir::Up;
}

std::string kindName(Kind k) {
    switch (k) {
    case Kind::Piston:
        return "piston";
    case Kind::RopePulley:
        return "pulley";
    default:
        return "elevator";
    }
}

struct Ctx {
    GlobalPos p;
    Kind      kind;
    Json      d;
    bool      removed = false;
    bool      discard = false;
    Json      orig; ///< as loaded (written back only when changed)

    [[nodiscard]] float offset() const { return d.value("offset", 0.0f); }
    void                setOffset(float o) { d["offset"] = o; }
    [[nodiscard]] int   init() const { return d.value("init", 0); }
    [[nodiscard]] bool  running() const { return d.value("running", false); }
    [[nodiscard]] ct::Id id() const { return d.value("id", ct::Id{0}); }
    [[nodiscard]] ct::Contraption* contraption() const { return id() ? ct::find(id()) : nullptr; }
    [[nodiscard]] std::optional<GlobalPos> parent() const {
        return d.contains("parent") ? posFrom(p.dim, d["parent"]) : std::nullopt;
    }
    /// The piston's facing (kept in the data: the block may be gone).
    [[nodiscard]] cc::Dir facing() const {
        if (!isPiston(idAt(p)) && d.contains("facing")) return static_cast<cc::Dir>(std::clamp(d["facing"].get<int>(), 0, 5));
        return facingAt(p);
    }
    [[nodiscard]] cp::MovementMode mode() const {
        if (kind == Kind::ElevatorPulley) return cp::MovementMode::MoveNeverPlace;
        return static_cast<cp::MovementMode>(std::clamp(d.value("mode", 0), 0, cp::kMovementModes - 1));
    }
};

void writeBack(Ctx& c) {
    if (c.discard || c.d == c.orig) return;
    data(c.p) = c.d;
    c.orig    = c.d;
}

void setError(Ctx& c, std::string const& key, std::string const& fallback, std::vector<std::string> const& args) {
    c.d["error"] = {
        {"key",  key },
        {"text", fallback},
        {"args", args}
    };
    log().info(
        "{} at {} {} {} could not assemble: {}",
        kindName(c.kind),
        c.p.x,
        c.p.y,
        c.p.z,
        core::formatMessage(std::string(translate(key, fallback)), args)
    );
}

void setRunningState(Ctx& c, bool running) {
    c.d["running"] = running;
    if (c.kind != Kind::Piston) {
        EditGuard g;
        setStates(c.p, {{kRunning, running ? 1 : 0}});
    }
}

// ---- movement --------------------------------------------------------------------------------

/// Create's sequencedOffsetLimit (blocks left to move; -1: none).
double sequenceLimit(Ctx const& c) { return c.d.value("seq", -1.0); }

/// Reads the limit of a sequenced gearshift's "turn to move" instruction reaching the actuator
/// (Create's LinearActuatorBlockEntity.onSpeedChanged); also when a new instruction arrives at an
/// unchanged speed (two equal steps in a row).
void readSequenceLimit(Ctx& c, bool speedChanged) {
    if (c.kind == Kind::ElevatorPulley) return;
    auto context = kinetics::sequenceContextAt(c.p);
    // an instruction is read once: its speed change may be reported after the tick that read it
    if (context && context->serial == c.d.value("seqId", std::uint64_t{0})) return;
    if (!speedChanged && !context) return;
    c.d["seq"]   = -1.0;
    c.d["seqId"] = context ? context->serial : 0;
    if (c.parent()) return; // Create: passive (mirrored) pulleys keep no limit
    if (context && context->kind == kinetics::SequenceContext::Kind::MoveDistance) c.d["seq"] = context->value;
}

/// Create's getMovementSpeed: the sequence limit clamps the step.
float clampToSequence(Ctx const& c, float v) {
    double limit = sequenceLimit(c);
    if (limit < 0) return v;
    return static_cast<float>(std::clamp<double>(v, -limit, limit));
}

float movementSpeed(Ctx& c) {
    float rpm = rpmAt(c.p);
    switch (c.kind) {
    case Kind::Piston:
        return clampToSequence(c, cp::pistonMovementSpeed(rpm, c.facing(), c.offset(), rangeOf(c.p, c.kind, c.d)));
    case Kind::RopePulley:
        return clampToSequence(c, cp::actuatorSpeed(rpm));
    default:
        break;
    }
    // elevator: towards the targeted contact
    int  target = static_cast<int>(c.offset());
    auto col    = c.d.value("col", Json());
    if (col.is_object() && c.contraption()) {
        ColumnCoords cc{c.p.dim, col.value("x", 0), col.value("z", 0), static_cast<cc::Dir>(col.value("side", 2))};
        if (auto ty = columnTargetOf(cc)) {
            int y = *ty;
            if (y >= c.d.value("minY", INT32_MIN) && y <= c.d.value("maxY", INT32_MAX))
                target = c.p.y - y + c.d.value("cy", 0) - 1;
        }
    }
    auto step       = cp::elevatorStep(rpm, c.offset(), target, c.d.value("prev", 0.0f));
    c.d["prev"]     = step.speed;
    c.d["arrived"]  = step.arrived;
    c.d["tgt"]      = target;
    return step.speed;
}

/// Where the contraption is drawn for `offset` (elevators settle on their floor separately).
cc::Vec3d displayDisplacement(Ctx& c, float offset) {
    if (c.kind == Kind::ElevatorPulley && c.d.contains("ey")) {
        double ey = c.d["ey"].get<double>();
        return {0, ey - c.d.value("ay", ey), 0};
    }
    return displacementOf(c.facing(), c.kind, offset, c.init());
}

void applyTransform(Ctx& c, cc::Vec3d const& displacement) {
    auto* con = c.contraption();
    if (!con) return;
    auto t   = con->transform();
    t.offset = displacement;
    con->setTransform(t);
    c.d["target"] = Json::array({displacement.x, displacement.y, displacement.z});
}

void resetContraptionToOffset(Ctx& c) { applyTransform(c, displayDisplacement(c, c.offset())); }

bool stalled(Ctx& c) {
    auto* con = c.contraption();
    if (!con || !c.d.contains("target")) return false;
    auto const& t = c.d["target"];
    cc::Vec3d   want{t[0].get<double>(), t[1].get<double>(), t[2].get<double>()};
    return (con->transform().offset - want).lengthSqr() > 1e-6;
}

/// Blocks of other piston / pulley contraptions near a world position (Create: the other
/// ControlledContraptionEntity's blocks).
bool collidesWithOthers(ct::Contraption const& self, std::vector<cc::Pos> const& worldColliders) {
    for (auto otherId : ct::all()) {
        if (otherId == self.id()) continue;
        auto* o = ct::find(otherId);
        if (!o || o->dimension() != self.dimension()) continue;
        if (o->kind() != "piston" && o->kind() != "pulley" && o->kind() != "elevator") continue;
        auto    pos = o->transform().origin + o->transform().offset;
        cc::Pos grid{int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))};
        // bounds first: most structures are far apart
        cc::Pos lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
        for (auto const& b : o->blocks()) {
            lo = {std::min(lo.x, b.local.x), std::min(lo.y, b.local.y), std::min(lo.z, b.local.z)};
            hi = {std::max(hi.x, b.local.x), std::max(hi.y, b.local.y), std::max(hi.z, b.local.z)};
        }
        bool near = false;
        for (auto const& w : worldColliders) {
            auto l = w - grid;
            if (l.x >= lo.x && l.x <= hi.x && l.y >= lo.y && l.y <= hi.y && l.z >= lo.z && l.z <= hi.z) near = true;
        }
        if (!near) continue;
        std::set<cc::Pos> mine;
        for (auto const& b : o->blocks()) mine.insert(b.local);
        for (auto const& w : worldColliders)
            if (mine.contains(w - grid)) return true;
    }
    return false;
}

/// Create's moveAndCollideContraption: true when the structure, moved by `v`, hits something.
bool moveAndCollide(Ctx& c, float v) {
    auto* con = c.contraption();
    if (!con) return false;
    if (stalled(c)) return false;
    if (c.kind == Kind::ElevatorPulley) return false; // elevators never collide (Create)
    if (v == 0) return false;
    cc::Dir dir    = motionDir(c.facing(), c.kind, v);
    auto    origin = con->transform().origin + displacementOf(c.facing(), c.kind, c.offset() + v, c.init());
    auto    grid   = cp::collisionGrid(origin, dir);
    auto    blocks = ct::ext::structureOf(*con);
    // Create: a pulley's anchor block moving up into its own magnet does not collide
    cc::Pos anchorLocal = con->anchor() == c.p ? cc::Pos{0, -(c.init() + 1), 0} : cc::Pos{};
    auto    ignore      = [&](cc::Pos const& world) {
        return c.kind != Kind::Piston && dir == cc::Dir::Up && world - grid == anchorLocal
            && idAt(gpos(c.p.dim, world)) == kMagnet;
    };
    if (ct::ext::collidesWithWorld(c.p.dim, blocks, grid, dir, ignore)) return true;
    std::vector<core::pistons::ColliderBlock> cb;
    for (auto const& b : blocks) cb.push_back({b.local, b.solid});
    std::vector<cc::Pos> world;
    for (auto const& l : cp::colliders(cb, dir)) world.push_back(l + grid);
    return collidesWithOthers(*con, world);
}

// ---- disassembly -----------------------------------------------------------------------------

void notifyMirrorsOfDisassembly(Ctx& c);

void disassemblePiston(Ctx& c) {
    if (!c.running() && !c.contraption()) return;
    auto* con = c.contraption();
    if (con) {
        // the piece of the pole line that lands on the piston's own block is the virtual one
        // (Create's customBlockPlacement): make room, let the engine place, then put the piston back
        Block const* piston = blockAt(c.p);
        bool         wasPiston = piston && isPiston(piston->getTypeName());
        auto         facing    = c.facing();
        std::optional<Json> doc;
        if (!c.removed) doc = common::blockdata::take(c.p);
        {
            EditGuard g;
            if (wasPiston) clearAt(c.p, 2);
            ct::disassemble(c.id());
            std::string landed = idAt(c.p);
            if (c.removed || !wasPiston) {
                if (landed == kHead || landed == kPole) clearAt(c.p, 3);
            } else {
                bool head  = landed == kHead;
                auto const* restored = withStates(*piston, {{kState, int(head ? PistonState::Retracted : PistonState::Extended)}});
                placeBlock(c.p, restored ? *restored : *piston, 3);
            }
        }
        if (doc && !c.removed) common::blockdata::put(c.p, std::move(*doc));
        sound(c.p, "close.wooden_trapdoor", 0.35f, 0.75f);
        log().info("piston at {} {} {} disassembled at offset {}", c.p.x, c.p.y, c.p.z, static_cast<int>(c.offset()));
        if (c.removed) destroyPistonChain(c.p, facing, true);
    } else if (!c.removed) {
        EditGuard g;
        setStates(c.p, {{kState, int(PistonState::Extended)}});
    }
    c.d["running"] = false;
    c.d["id"]      = 0;
    c.d.erase("target");
}

/// Create: getDestroySpeed == -1 (bedrock, barriers...); air and liquids are replaced.
bool unbreakable(GlobalPos const& q) {
    auto const* b = blockAt(q);
    if (!b) return true;
    if (b->isAir() || replaceableAt(q)) return false;
    return destroySpeedAt(q) < 0;
}

/// Breaks what is in the rope's way like the game (other units / actuators see it go); rope
/// pieces of ours are just replaced.
void clearForRope(GlobalPos const& q, int dim) {
    auto const* b = blockAt(q);
    if (!b || b->isAir() || isRopeLike(b->getTypeName())) return;
    destroyAt(q, !ct::ext::hasCollision(dim, cpos(q)));
}

void createRopes(Ctx& c) {
    int off = static_cast<int>(c.offset());
    if (off > 0) {
        auto magnetPos = rel(c.p, cc::Dir::Down, off);
        if (!unbreakable(magnetPos)) {
            clearForRope(magnetPos, c.p.dim);
            EditGuard g;
            place(magnetPos, kMagnet, {}, 2);
        }
    }
    for (int i = 1; i <= off - 1; ++i) {
        auto ropePos = rel(c.p, cc::Dir::Down, i);
        if (!unbreakable(ropePos)) clearForRope(ropePos, c.p.dim);
    }
    EditGuard g;
    for (int i = 1; i <= off - 1; ++i) {
        auto ropePos = rel(c.p, cc::Dir::Down, i);
        if (unbreakable(ropePos)) continue;
        place(ropePos, kRope, {}, 2);
    }
}

void disassemblePulley(Ctx& c) {
    if (!c.running() && !c.contraption() && !c.parent()) return;
    c.setOffset(static_cast<float>(cp::gridOffset(c.offset(), rangeOf(c.p, c.kind, c.d))));
    if (c.contraption()) resetContraptionToOffset(c);
    bool elevator = c.kind == Kind::ElevatorPulley;
    if (elevator && c.contraption()) {
        auto col = c.d.value("col", Json());
        if (col.is_object())
            columnSetActive({c.p.dim, col.value("x", 0), col.value("z", 0), static_cast<cc::Dir>(col.value("side", 2))}, false);
    }
    if (!c.removed && !elevator) createRopes(c);
    if (c.contraption() && !c.parent()) {
        ct::disassemble(c.id());
        sound(c.p, "close.wooden_trapdoor", 0.35f, 0.75f);
    }
    notifyMirrorsOfDisassembly(c);
    log().info("{} at {} {} {} disassembled at offset {}", kindName(c.kind), c.p.x, c.p.y, c.p.z, static_cast<int>(c.offset()));
    c.d["id"]   = 0;
    c.d["init"] = 0;
    c.d.erase("target");
    c.d.erase("ey");
    c.d.erase("ay");
    setRunningState(c, false);
    if (elevator) c.setOffset(-1);
    syncRope(c.p, c.offset(), 0, false, elevator);
}

void disassemble(Ctx& c) {
    if (c.kind == Kind::Piston) disassemblePiston(c);
    else disassemblePulley(c);
}

void tryDisassemble(Ctx& c) {
    int initial = c.kind == Kind::Piston ? (c.contraption() ? c.init() : 0) : c.init();
    if (cp::stopAction(c.mode(), c.offset(), initial, c.removed) == cp::StopAction::Wait) {
        c.d["waiting"] = true;
        return;
    }
    disassemble(c);
}

void runMirrorChild(GlobalPos const& child, std::function<void(Ctx&)> const& fn);

void notifyMirrorsOfDisassembly(Ctx& c) {
    if (!c.d.contains("children")) return;
    float off      = c.offset();
    auto  children = c.d["children"];
    c.d.erase("children");
    for (auto const& j : children) {
        auto child = posFrom(c.p.dim, j);
        if (!child) continue;
        runMirrorChild(*child, [&](Ctx& cc) {
            cc.setOffset(off);
            disassemble(cc);
            cc.d.erase("parent");
        });
    }
}

// ---- assembly --------------------------------------------------------------------------------

void assembleLog(Ctx& c, ct::Contraption const* con) {
    log().info(
        "{} at {} {} {} assembled: {} blocks, offset {}",
        kindName(c.kind),
        c.p.x,
        c.p.y,
        c.p.z,
        con ? con->blocks().size() : 0,
        static_cast<int>(c.offset())
    );
}

/// Sets assemblingPiston() for a scope (also when the assembly throws).
struct PistonFlag {
    PistonFlag() { assemblingPiston() = true; }
    ~PistonFlag() { assemblingPiston() = false; }
    PistonFlag(PistonFlag const&)            = delete;
    PistonFlag& operator=(PistonFlag const&) = delete;
};

bool isPoleAlong(GlobalPos const& q, cc::Axis axis) { return idAt(q) == kPole && cc::axisOf(facingAt(q)) == axis; }

void assemblePiston(Ctx& c) {
    std::string id = idAt(c.p);
    if (!isPiston(id)) return;
    auto const& cfg     = config::get<Config>();
    cc::Dir     facing  = facingAt(c.p);
    bool        sticky  = id == kStickyPiston;
    float       rpm     = rpmAt(c.p);
    float       raw     = cp::pistonMovementSpeed(rpm, facing, 1e6f, 2000000); // unclamped direction
    bool        retract = raw < 0;
    cc::Axis    axis    = cc::axisOf(facing);

    // extension poles (Create's PistonContraption.collectExtensions)
    std::vector<GlobalPos> poles;
    int                    front = 0, back = 0;
    if (stateOr(c.p, kState, 0) == int(PistonState::Extended)) {
        for (;;) {
            auto next = rel(c.p, facing, front + 1);
            bool head = idAt(next) == kHead && facingAt(next) == facing;
            if (!isPoleAlong(next, axis) && !head) break;
            poles.push_back(next);
            ++front;
            if (head) break;
            if (front > cfg.maxPistonPoles) {
                setError(c, "create.gui.assembly.exception.tooManyPistonPoles", "There are too many extension Poles attached to this Piston.\nThe configured maximum is: {0}", {std::to_string(cfg.maxPistonPoles)});
                return;
            }
        }
    }
    for (;;) {
        auto next = rel(c.p, cc::opposite(facing), back + 1);
        if (!isPoleAlong(next, axis)) break;
        poles.push_back(next);
        ++back;
        if (front + back > cfg.maxPistonPoles) {
            setError(c, "create.gui.assembly.exception.tooManyPistonPoles", "There are too many extension Poles attached to this Piston.\nThe configured maximum is: {0}", {std::to_string(cfg.maxPistonPoles)});
            return;
        }
    }
    int length = front + back;
    if (length == 0) {
        setError(c, "create.gui.assembly.exception.noPistonPoles", "The Piston is missing some extension Poles", {});
        return;
    }

    // the line in front of the head (Create's addToInitialFrontier)
    std::vector<cc::Pos> starts;
    cc::Dir              forced = retract ? cc::opposite(facing) : facing;
    if (!(retract && !sticky)) {
        for (int o = 0; o <= ct::ext::maxChassisRange(); ++o) {
            if (o == 1 && retract) break;
            auto cur = rel(c.p, facing, 1 + o + front);
            auto* r  = regionOf(c.p.dim);
            if (retract && r && (cur.y < r->getMinHeight() || cur.y >= r->getMaxHeight())) break;
            if (!loaded(cur)) {
                setError(c, "create.gui.assembly.exception.chunkNotLoaded", "The Block at [{0},{1},{2}] was not in a loaded chunk", {std::to_string(cur.x), std::to_string(cur.y), std::to_string(cur.z)});
                return;
            }
            auto info = ct::ext::describe(cur.dim, cpos(cur));
            if (!info.necessary) break;
            if (info.brittle && !info.id.ends_with("carpet")) break;
            if (info.id == kHead && facingAt(cur) == cc::opposite(forced)) break;
            if (!info.movable) {
                if (retract) break;
                setError(c, "create.gui.assembly.exception.unmovableBlock", "Unmovable Block ({3}) at [{0},{1},{2}]", {std::to_string(cur.x), std::to_string(cur.y), std::to_string(cur.z), info.id});
                return;
            }
            if (retract && info.pushOnly) break;
            starts.push_back(cpos(cur));
            if (cc::has(info.notSupportive, facing)) break;
        }
    }

    // structure: the piston's own block stands for the virtual head / pole (Create's line)
    std::vector<cc::Pos> start{cpos(c.p)};
    for (auto const& q : poles) start.push_back(cpos(q));
    start.insert(start.end(), starts.begin(), starts.end());
    ct::AssembleOptions opts;
    opts.anchor        = c.p;
    opts.start         = start;
    opts.forced        = forced;
    opts.kind          = "piston";
    opts.includeAnchor = true;
    auto virtualInfo   = [&](cc::Pos const& q, cc::BlockInfo& info) {
        if (q != cpos(c.p)) return;
        info           = cc::BlockInfo{};
        info.necessary = true;
        info.id        = std::string(front == 0 ? kHead : kPole);
        info.notSupportive = 0x3F;
    };
    cc::AssemblyResult dry;
    {
        PistonFlag flag;
        dry = ct::ext::dryRun(opts, virtualInfo);
    }
    if (!dry.ok()) {
        auto e = ct::ext::errorOf(dry);
        setError(c, e.key, e.fallback, e.args);
        return;
    }
    // Create: refuse when the structure would hit something on its first step (no message)
    cc::Dir moveDir = forced;
    auto    planned = ct::ext::structureAt(c.p.dim, dry.blocks, cpos(c.p));
    for (auto& b : planned)
        if (b.local == cc::Pos{}) b = {cc::Pos{}, std::string(front == 0 ? kHead : kPole), true};
    if (ct::ext::collidesWithWorld(c.p.dim, planned, cpos(c.p) + cc::offset(moveDir), moveDir)) return;

    // not at a limit already
    float resulting = static_cast<float>(front) + (retract ? -0.5f : 0.5f);
    if (resulting <= 0 || resulting >= static_cast<float>(length)) return;

    // lift: the piston becomes its virtual head / pole for the engine, then comes back as moving
    Block const*        piston = blockAt(c.p);
    std::optional<Json> doc    = common::blockdata::take(c.p);
    ct::AssembleResult  r;
    {
        EditGuard g;
        if (front == 0) place(c.p, kHead, {{kFacing, int(facing)}, {kSticky, sticky ? 1 : 0}}, 2);
        else place(c.p, kPole, {{kFacing, int(facing)}}, 2);
        {
            PistonFlag flag;
            r = ct::assemble(opts);
        }
        auto const* moving = piston ? withStates(*piston, {{kState, int(r.ok() ? PistonState::Moving : PistonState(stateOr(c.p, kState, 0)))}}) : nullptr;
        if (r.ok()) {
            if (moving) placeBlock(c.p, *moving, 3);
        } else if (piston) {
            placeBlock(c.p, *piston, 3);
        }
    }
    if (doc) common::blockdata::put(c.p, std::move(*doc));
    if (!r.ok()) {
        setError(c, r.errorKey, r.errorText, r.errorArgs);
        return;
    }
    c.d["len"]     = length;
    c.d["facing"]  = int(facing);
    c.d["init"]    = front;
    c.d["id"]      = *r.id;
    c.d["waiting"] = false;
    c.d.erase("error");
    c.d.erase("target");
    c.setOffset(static_cast<float>(front));
    c.d["running"] = true;
    sound(c.p, "open.wooden_trapdoor", 0.5f, 0.5f);
    assembleLog(c, ct::find(*r.id));
}

/// The contacts captured by an elevator's structure: {contact world pos, facing}.
std::vector<std::pair<GlobalPos, cc::Dir>> horizontalContacts(int dim, std::vector<cc::Pos> const& blocks) {
    std::vector<std::pair<GlobalPos, cc::Dir>> out;
    for (auto const& b : blocks) {
        auto g = gpos(dim, b);
        if (idAt(g) != kRedstoneContact) continue;
        auto f = facingAt(g);
        if (cc::axisOf(f) == cc::Axis::Y) continue;
        out.emplace_back(g, f);
    }
    return out;
}

void startMirroring(GlobalPos const& child, Ctx& parent);

void assemblePulley(Ctx& c) {
    std::string id = idAt(c.p);
    bool        elevator = c.kind == Kind::ElevatorPulley;
    if (elevator ? id != kElevatorPulley : id != kRopePulley) return;
    float rpm = rpmAt(c.p);
    if (rpm == 0 && !c.parent()) return;
    int maxLength = maxRope();
    int i         = 1;
    while (i <= maxLength) {
        auto below = rel(c.p, cc::Dir::Down, i);
        if (elevator) {
            // Create: the cabin is the first block below with a collision shape that isn't replaceable
            if (!loaded(below)) break;
            if (ct::ext::hasCollision(c.p.dim, cpos(below)) && !replaceableAt(below)) break;
        } else if (!isRopeLike(idAt(below))) {
            break;
        }
        ++i;
    }
    float offset = static_cast<float>(i - 1);
    int   range  = rangeOf(c.p, c.kind, c.d);
    if (!elevator) {
        if (offset >= static_cast<float>(range) && rpm > 0) return;
        if (offset <= 0 && rpm < 0) return;
    }

    auto removeRopes = [&](int count) {
        EditGuard g;
        for (int k = count; k > 0; --k) {
            auto q = rel(c.p, cc::Dir::Down, k);
            if (isRopeLike(idAt(q))) clearAt(q, 2);
        }
    };

    if (!c.parent()) {
        c.d["needs"]  = false;
        int  init     = static_cast<int>(std::floor(offset));
        auto anchorAt = rel(c.p, cc::Dir::Down, init + 1);
        ct::AssembleOptions opts;
        opts.anchor = c.p;
        opts.start  = {cpos(anchorAt)};
        opts.kind   = elevator ? "elevator" : "pulley";
        // Create's PulleyContraption.isAnchoringBlockAt: the rope column above the anchor
        auto column = [&](cc::Pos const& q, cc::BlockInfo& info) {
            if (q.x == anchorAt.x && q.z == anchorAt.z && q.y > anchorAt.y && q.y <= anchorAt.y + init + 1) info.anchor = true;
        };
        auto dry = ct::ext::dryRun(opts, column);
        if (!dry.ok()) {
            auto e = ct::ext::errorOf(dry);
            setError(c, e.key, e.fallback, e.args);
            return;
        }
        auto planned  = ct::ext::structureAt(c.p.dim, dry.blocks, cpos(anchorAt));
        bool canBuild = true;

        // elevator: exactly one horizontal redstone contact, a free column
        std::optional<ColumnCoords> column2;
        int                         contactY = 0;
        if (elevator) {
            if (dry.blocks.empty()) return;
            auto contacts = horizontalContacts(c.p.dim, dry.blocks);
            if (contacts.empty()) {
                setError(c, "create.gui.assembly.exception.no_contacts", "Elevators require a horizontal Redstone Contact attached to the structure", {});
                return;
            }
            if (contacts.size() > 1) {
                setError(c, "create.gui.assembly.exception.too_many_contacts", "Cannot attach more than one Redstone Contact to Elevators", {});
                return;
            }
            auto [cpos2, cface] = contacts.front();
            auto front          = rel(cpos2, cface);
            column2             = ColumnCoords{c.p.dim, front.x, front.z, cc::opposite(cface)};
            contactY            = front.y - anchorAt.y;
            if (columnActive(*column2)) {
                setError(c, "create.gui.assembly.exception.column_conflict", "Another Elevator is already targeting this column of contacts", {});
                return;
            }
        } else if (!dry.blocks.empty()) {
            cc::Dir moveDir = rpm > 0 ? cc::Dir::Down : cc::Dir::Up;
            auto    grid    = cpos(anchorAt) + cc::offset(moveDir);
            auto    ignore  = [&](cc::Pos const& world) {
                return moveDir == cc::Dir::Up && world == cpos(rel(anchorAt, cc::Dir::Up)) && idAt(gpos(c.p.dim, world)) == kMagnet;
            };
            if (ct::ext::collidesWithWorld(c.p.dim, planned, grid, moveDir, ignore)) canBuild = false;
        }
        if (!canBuild && rpm > 0) return;

        c.d["init"] = init;
        if (!dry.blocks.empty()) {
            auto r = ct::assemble(opts);
            if (!r.ok()) {
                setError(c, r.errorKey, r.errorText, r.errorArgs);
                return;
            }
            if (!elevator) removeRopes(static_cast<int>(offset));
            c.d["id"]    = *r.id;
            c.d["needs"] = true;
            c.d.erase("target");
            c.d.erase("error");
            sound(c.p, "open.wooden_trapdoor", 0.5f, 0.5f);
            // mirrored pulleys above the structure's top layer (Create's startMirroringOther)
            std::set<cc::Pos> solid;
            for (auto const& b : planned)
                if (b.solid) solid.insert(b.local);
            for (auto const& b : planned) {
                if (!b.solid || b.local.y != 0 || solid.contains(b.local + cc::Pos{0, 1, 0})) continue;
                GlobalPos above{c.p.dim, anchorAt.x + b.local.x, c.p.y, anchorAt.z + b.local.z};
                if (above == c.p || idAt(above) != (elevator ? kElevatorPulley : kRopePulley)) continue;
                startMirroring(above, c);
            }
            if (elevator) {
                c.setOffset(offset);
                c.d["col"] = {
                    {"x",    column2->x       },
                    {"z",    column2->z       },
                    {"side", int(column2->side)}
                };
                c.d["cy"]   = contactY;
                c.d["maxY"] = c.p.y + contactY - 1;
                c.d["minY"] = c.p.y + contactY - 1 - maxLength;
                c.d["ay"]   = static_cast<double>(anchorAt.y);
                c.d["ey"]   = static_cast<double>(anchorAt.y);
                int target  = static_cast<int>(c.p.y + contactY - 1 - offset);
                columnTarget(*column2, target);
                gatherColumn(*column2);
                columnSetActive(*column2, true);
                floorReached(*column2, target);
                c.d["arrived"] = true;
                c.d["prev"]    = 0.0f;
            }
            c.setOffset(offset);
            assembleLog(c, ct::find(*r.id));
        } else {
            if (!elevator) removeRopes(static_cast<int>(offset));
            c.d["id"] = 0;
            log().info("{} at {} {} {} running without a structure", kindName(c.kind), c.p.x, c.p.y, c.p.z);
        }
    } else {
        removeRopes(static_cast<int>(offset));
    }
    c.setOffset(offset);
    setRunningState(c, true);
    syncRope(c.p, offset, 0, true, elevator);
}

void assemble(Ctx& c) {
    if (c.kind == Kind::Piston) assemblePiston(c);
    else assemblePulley(c);
}

void visitNewPosition(Ctx& c, float v) {
    if (c.kind != Kind::RopePulley) return;
    if (c.contraption()) return;
    if (rpmAt(c.p) <= 0) return;
    auto below = rel(c.p, cc::Dir::Down, static_cast<int>(c.offset() + v) + 1);
    if (!loaded(below)) return;
    auto info = ct::ext::describe(below.dim, cpos(below));
    if (!info.necessary || info.brittle) return;
    disassemble(c);
    c.d["assemble"] = true;
}

void collided(Ctx& c, float v) {
    c.setOffset(static_cast<float>(cp::gridOffset(c.offset() - v, rangeOf(c.p, c.kind, c.d))));
    resetContraptionToOffset(c);
    tryDisassemble(c);
    if (c.kind == Kind::Piston && !c.running() && cp::pistonMovementSpeed(rpmAt(c.p), facingAt(c.p), c.offset(), rangeOf(c.p, c.kind, c.d)) > 0)
        c.d["assemble"] = true;
}

void elevatorAfterTick(Ctx& c, bool wasArrived, std::uint64_t tick) {
    auto* con = c.contraption();
    if (!con) return;
    c.d["waiting"] = false;
    bool arrived   = c.d.value("arrived", false);
    auto col       = c.d.value("col", Json());
    if (!col.is_object()) return;
    ColumnCoords cc{c.p.dim, col.value("x", 0), col.value("z", 0), static_cast<cc::Dir>(col.value("side", 2))};
    int          cy = c.d.value("cy", 0);
    double       ey = c.d.value("ey", con->transform().origin.y);
    if (!arrived) {
        // the cabin follows the offset
        ey = c.d.value("ay", con->transform().origin.y) + c.init() - c.offset();
        c.d["ey"] = ey;
        return;
    }
    int  targetLevel = static_cast<int>(std::floor(0.5 + ey)) + cy;
    auto target      = columnTargetOf(cc);
    if (target && *target >= c.d.value("minY", INT32_MIN) && *target <= c.d.value("maxY", INT32_MAX)) targetLevel = *target;
    if (!wasArrived) {
        triggerContact(cc, targetLevel);
        floorReached(cc, targetLevel);
        sound(rel(c.p, cc::Dir::Down, static_cast<int>(c.offset())), "close.wooden_trapdoor", 0.75f, 0.8f);
        log().info("elevator at {} {} {} arrived at contact y {}", c.p.x, c.p.y, c.p.z, targetLevel);
    }
    ey        = cp::elevatorSettle(ey, static_cast<double>(targetLevel - cy));
    c.d["ey"] = ey;
    // Create's lazyTick (every 3 ticks): keep the floor's contact active while standing there
    if (tick % 3 == 0 && c.d.value("tgt", 0) == static_cast<int>(c.offset())) triggerContact(cc, static_cast<int>(std::floor(0.5 + ey)) + cy);
    // a controls request from the cabin (U22's contraption controls write it into anchorData)
    auto& ad = con->anchorData();
    if (ad.contains("elevator") && ad["elevator"].is_object() && ad["elevator"].contains("request")) {
        auto req = ad["elevator"]["request"];
        ad["elevator"].erase("request");
        if (!req.is_number()) return;
        int y = static_cast<int>(req.get<double>());
        GlobalPos contact{c.p.dim, cc.x, y, cc.z};
        if (idAt(contact) == kElevatorContact) callToContact(contact, false);
    }
}

/// Publishes the floors to the cabin's anchorData (for U22's contraption controls).
void publishFloors(Ctx& c) {
    auto* con = c.contraption();
    auto  col = c.d.value("col", Json());
    if (!con || !col.is_object()) return;
    ColumnCoords cc{c.p.dim, col.value("x", 0), col.value("z", 0), static_cast<cc::Dir>(col.value("side", 2))};
    auto&        ad      = con->anchorData();
    auto&        e       = ad["elevator"];
    int          version = columnNamesVersion(cc);
    if (!e.is_object()) e = Json::object();
    if (e.value("version", -1) != version) {
        Json floors = Json::array();
        for (auto const& f : columnFloors(cc)) floors.push_back(Json::array({f.y, f.shortName, f.longName}));
        e["floors"]  = floors;
        e["version"] = version;
    }
    e["column"]   = {cc.x, cc.z, int(cc.side)};
    e["contactY"] = c.d.value("cy", 0);
    e["target"]   = columnTargetOf(cc).value_or(INT32_MIN);
    e["current"]  = static_cast<int>(std::floor(0.5 + c.d.value("ey", 0.0))) + c.d.value("cy", 0);
}

void tickOne(Ctx& c, std::uint64_t tick) {
    if (c.d.value("id", ct::Id{0}) && !c.contraption()) c.d["id"] = 0;
    if (auto parent = c.parent()) {
        // Create: mirrored pulleys are passive; one whose parent is gone places its rope
        bool listed = false;
        if (loaded(*parent)) {
            if (auto const* pd = dataIf(*parent); pd && pd->contains("children"))
                for (auto const& j : (*pd)["children"])
                    if (posFrom(c.p.dim, j) == c.p) listed = true;
            if (!listed) {
                disassemble(c);
                c.d.erase("parent");
            }
        }
        return;
    }
    bool wasArrived = c.d.value("arrived", true);

    if (c.d.value("waiting", false)) {
        if (c.kind == Kind::ElevatorPulley) c.d["waiting"] = false;
        else return;
    }
    if (c.d.value("assemble", false)) {
        c.d["assemble"] = false;
        if (c.running()) {
            if (rpmAt(c.p) == 0 && c.kind != Kind::ElevatorPulley) tryDisassemble(c);
            else if (c.kind != Kind::Piston) syncRope(c.p, c.offset(), 0, true, c.kind == Kind::ElevatorPulley);
        } else if (c.kind == Kind::ElevatorPulley || rpmAt(c.p) != 0) {
            assemble(c);
        }
        return;
    }
    if (!c.running()) return;
    bool present = c.contraption() != nullptr;
    if (c.d.value("needs", true) && !present) return;

    readSequenceLimit(c, false);
    float v         = movementSpeed(c);
    if (double limit = sequenceLimit(c); limit > 0) c.d["seq"] = std::max(0.0, limit - std::abs(v));
    float offset    = c.offset();
    float newOffset = offset + v;
    if (static_cast<int>(newOffset) != static_cast<int>(offset)) {
        visitNewPosition(c, v);
        if (!c.running()) return;
    }
    if (present && moveAndCollide(c, v)) {
        c.setOffset(static_cast<float>(cp::gridOffset(offset, rangeOf(c.p, c.kind, c.d))));
        resetContraptionToOffset(c);
        collided(c, v);
        return;
    }
    if (!present || !stalled(c)) c.setOffset(newOffset);

    int range = rangeOf(c.p, c.kind, c.d);
    if (c.offset() <= 0 || c.offset() >= static_cast<float>(range)) {
        c.setOffset(c.offset() <= 0 ? 0.f : static_cast<float>(range));
        if (c.kind == Kind::ElevatorPulley) elevatorAfterTick(c, wasArrived, tick);
        resetContraptionToOffset(c);
        tryDisassemble(c);
        if (c.kind != Kind::Piston) syncRope(c.p, c.offset(), 0, c.running(), c.kind == Kind::ElevatorPulley);
        return;
    }
    if (c.kind == Kind::ElevatorPulley) {
        elevatorAfterTick(c, wasArrived, tick);
        publishFloors(c);
    }
    if (present) resetContraptionToOffset(c);
    if (c.kind != Kind::Piston && tick % 3 == 0) {
        bool elevator = c.kind == Kind::ElevatorPulley;
        syncRope(c.p, c.offset(), v, true, elevator);
        if (c.d.contains("children"))
            for (auto const& j : c.d["children"])
                if (auto child = posFrom(c.p.dim, j)) syncRope(*child, c.offset(), v, true, elevator);
    }
}

std::optional<Ctx> load(GlobalPos const& p) {
    auto id = idAt(p);
    if (id.empty()) return std::nullopt;
    auto kind = actuatorKind(id);
    if (!kind) return std::nullopt;
    Ctx c{p, *kind, Json::object()};
    if (auto const* d = dataIf(p)) c.d = *d;
    c.orig = c.d;
    if (c.orig.empty()) c.orig = nullptr; // a fresh document is always written
    return c;
}

void runMirrorChild(GlobalPos const& child, std::function<void(Ctx&)> const& fn) {
    auto c = load(child);
    if (!c) return;
    fn(*c);
    writeBack(*c);
}

void startMirroring(GlobalPos const& child, Ctx& parent) {
    if (child == parent.p) return;
    auto& children = parent.d["children"];
    if (!children.is_array()) children = Json::array();
    children.push_back(posJson(child));
    runMirrorChild(child, [&](Ctx& c) {
        c.d["parent"] = posJson(parent.p);
        assemble(c);
    });
    trackActuator(child);
    log().info("{} at {} {} {} mirrors {} {} {}", kindName(parent.kind), child.x, child.y, child.z, parent.p.x, parent.p.y, parent.p.z);
}

} // namespace

std::optional<Kind> actuatorKind(std::string_view id) {
    if (isPiston(id)) return Kind::Piston;
    if (id == kRopePulley) return Kind::RopePulley;
    if (id == kElevatorPulley) return Kind::ElevatorPulley;
    return std::nullopt;
}

void trackActuator(GlobalPos const& p) {
    if (active().insert(p).second) persistActive();
}

void requestAssemble(GlobalPos const& p) {
    auto c = load(p);
    if (!c) return;
    c->d["assemble"] = true;
    writeBack(*c);
    trackActuator(p);
}

void elevatorClicked(GlobalPos const& p) {
    auto c = load(p);
    if (!c || c->kind != Kind::ElevatorPulley) return;
    if (auto parent = c->parent()) {
        elevatorClicked(*parent);
        return;
    }
    if (c->running()) disassemble(*c);
    else c->d["assemble"] = true;
    writeBack(*c);
    trackActuator(p);
}

void onActuatorSpeed(GlobalPos const& p, float prevSpeed, float speed) {
    auto c = load(p);
    if (!c || c->kind == Kind::ElevatorPulley) return;
    readSequenceLimit(*c, true);
    if (c->parent()) {
        writeBack(*c);
        return;
    }
    c->d["assemble"] = true;
    c->d["waiting"]  = false;
    if (c->contraption() && prevSpeed != 0 && (prevSpeed > 0) != (speed > 0)) {
        if (!stalled(*c)) {
            // Create: offset = Math.round(offset * 16) / 16 (integer division)
            c->setOffset(static_cast<float>(static_cast<int>(std::lround(c->offset() * 16)) / 16));
            resetContraptionToOffset(*c);
        }
    }
    writeBack(*c);
    trackActuator(p);
}

void actuatorRemoved(GlobalPos const& p, Kind kind, Json const& doc, std::string const& /*previousId*/) {
    Ctx c{p, kind, doc};
    c.removed = true;
    c.discard = true;
    if (auto parent = c.parent()) {
        // a mirrored pulley went: its parent forgets it
        runMirrorChild(*parent, [&](Ctx& pc) {
            if (!pc.d.contains("children")) return;
            Json kept = Json::array();
            for (auto const& j : pc.d["children"])
                if (posFrom(p.dim, j) != p) kept.push_back(j);
            pc.d["children"] = kept;
        });
    }
    if (c.running() || c.contraption()) {
        if (kind == Kind::Piston) disassemblePiston(c);
        else disassemblePulley(c);
    }
    if (kind != Kind::Piston) syncRopeRemoved(p);
    if (active().erase(p)) persistActive();
}

cp::MovementMode movementMode(GlobalPos const& p) {
    auto const* d = dataIf(p);
    return static_cast<cp::MovementMode>(std::clamp(d ? d->value("mode", 0) : 0, 0, cp::kMovementModes - 1));
}

void setMovementMode(GlobalPos const& p, cp::MovementMode mode) {
    auto& d      = data(p);
    d["mode"]    = int(mode);
    d["waiting"] = false; // Create: the callback clears waitingForSpeedChange
    trackActuator(p);
}

float offsetOf(GlobalPos const& p) {
    auto const* d = dataIf(p);
    return d ? d->value("offset", 0.0f) : 0.f;
}
bool runningAt(GlobalPos const& p) {
    auto const* d = dataIf(p);
    return d && d->value("running", false);
}
std::uint64_t contraptionAt(GlobalPos const& p) {
    auto const* d = dataIf(p);
    return d ? d->value("id", std::uint64_t{0}) : 0;
}
std::string lastErrorAt(GlobalPos const& p) {
    auto const* d = dataIf(p);
    if (!d || !d->contains("error")) return {};
    return (*d)["error"].value("key", "");
}

void tickActuators() {
    auto tick = currentTick();
    std::vector<GlobalPos> list(active().begin(), active().end());
    bool                   changed = false;
    for (auto const& p : list) {
        if (!loaded(p)) continue; // frozen while unloaded (Create's block entities don't tick)
        auto c = load(p);
        if (!c) {
            active().erase(p);
            changed = true;
            continue;
        }
        try {
            tickOne(*c, tick);
        } catch (std::exception const& e) {
            log().error("{} at {} {} {} tick failed: {}", kindName(c->kind), p.x, p.y, p.z, e.what());
        }
        if (c->kind != Kind::Piston && c->running() && c->d.value("waiting", false) && tick % 20 == 0 && !c->parent()) {
            bool elevator = c->kind == Kind::ElevatorPulley;
            syncRope(p, c->offset(), 0, true, elevator);
            if (c->d.contains("children"))
                for (auto const& j : c->d["children"])
                    if (auto child = posFrom(p.dim, j)) syncRope(*child, c->offset(), 0, true, elevator);
        }
        writeBack(*c);
        bool idle = !c->running() && !c->d.value("assemble", false) && !c->d.value("id", ct::Id{0}) && !c->parent();
        if (idle) {
            active().erase(p);
            changed = true;
        }
    }
    if (changed) persistActive();
}

void resetActuators() { active().clear(); }

void loadActuators() {
    active().clear();
    if (auto j = game::db::load(kDbActive); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) active().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
}

} // namespace lv::feature::pistons
