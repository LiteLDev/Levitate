// Pipes unit: the flow model on the running world (Host adapter), block change tracking (Create's
// onPlace / neighborChanged / onRemove scheduling), pumps and valves on the kinetic network, the
// smart pipe filter slot, player interactions (wrench, copper casing, brackets, placement),
// persistence and the `/create pipes` debug command.

#include <climits>
#include <cmath>
#include <deque>
#include <map>
#include <set>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/FluidRegistry.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/pipes/OpenEnds.h"
#include "feature/pipes/PipeEffects.h"
#include "feature/pipes/Pipes.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("pipes", lv::feature::pipes::PipesConfig);

namespace lv::feature::pipes {

namespace k  = lv::kinetics;
namespace ia = common::interactions;
namespace ft = common::fluidtypes;

// ---- kinetic blocks -----------------------------------------------------------------------------------

namespace {

Face facingOf(k::BlockState const& s) { return static_cast<Face>(std::clamp(s.get("levitate:facing", 1), 0, 5)); }

k::KineticSpec pumpSpec() {
    k::KineticSpec s;
    s.states       = {"levitate:facing"};
    s.rotationAxis = [](k::BlockState const& st) -> std::optional<Axis> { return cp::axisOf(facingOf(st)); };
    // Create's pump is driven through its cog only (pipes take both ends of its axis)
    s.hasShaftTowards = [](k::BlockState const&, Face) { return false; };
    s.cog             = [](k::BlockState const&) { return k::CogType::Small; };
    s.impact          = 4;
    s.rotorState      = "levitate:rotor";
    s.rotorModel      = "geometry.levitate.mechanical_pump_cog";
    s.rotorTexture    = "levitate_mechanical_pump";
    return s;
}

k::KineticSpec valveSpec() {
    k::KineticSpec s;
    s.states       = {"levitate:facing", "levitate:along_first"};
    auto shaft     = [](k::BlockState const& st) {
        return cp::valveAxes(facingOf(st), st.get("levitate:along_first") != 0).shaft;
    };
    s.rotationAxis    = [shaft](k::BlockState const& st) -> std::optional<Axis> { return shaft(st); };
    s.hasShaftTowards = [shaft](k::BlockState const& st, Face f) { return cp::axisOf(f) == shaft(st); };
    s.rotorState      = "levitate:rotor";
    s.rotorModel      = "geometry.levitate.fluid_valve_shaft";
    s.rotorTexture    = "levitate_fluid_valve";
    return s;
}

} // namespace

LV_REGISTER_KINETIC_BLOCK(kPump, pumpSpec());
LV_REGISTER_KINETIC_BLOCK(kValve, valveSpec());

// ---- the world as the flow model sees it -------------------------------------------------------------

namespace {

BlockSource* regionFor(GlobalPos const& p, bool requireLoaded = true) {
    auto* r = regionOf(p.dim);
    if (!r) return nullptr;
    if (requireLoaded && !r->hasChunksAt(toBlockPos(p), 0, false)) return nullptr;
    return r;
}

common::fluids::FluidStack toStack(cp::Fluid const& f) { return {f.id, f.amount}; }
cp::Fluid                  fromStack(common::fluids::FluidStack const& s) { return {s.fluid, s.amount}; }

class WorldHost final : public cp::Host {
public:
    std::int64_t                                                       time = 0;
    std::set<GlobalPos>                                                flowChanged;
    std::vector<std::tuple<GlobalPos, cp::Fluid, cp::Fluid>>           collisions;

    bool loaded(GlobalPos const& p) override { return regionFor(p) != nullptr; }

    cp::Kind kind(GlobalPos const& p) override {
        auto* r = regionFor(p);
        if (!r) return cp::Kind::None;
        auto k = kindOf(r->getBlock(toBlockPos(p)));
        return k == BlockKind::Pump ? cp::Kind::Pump : isPipeLike(k) ? cp::Kind::Pipe : cp::Kind::None;
    }

    bool canHaveFlowToward(GlobalPos const& p, Face f) override {
        auto* r = regionFor(p);
        return r && cp::has(flowFaces(r->getBlock(toBlockPos(p))), f);
    }

    bool canPullFluidFrom(GlobalPos const& p, Face, cp::Fluid const& fluid) override {
        auto* r = regionFor(p);
        if (!r) return false;
        auto const& b = r->getBlock(toBlockPos(p));
        switch (kindOf(b)) {
        case BlockKind::Valve:
            return game::intState(b, "levitate:enabled").value_or(0) != 0;
        case BlockKind::Smart: {
            if (fluid.empty()) return true;
            auto f = common::filterslot::get(p);
            return common::items::isEmpty(f) || common::filter::matchesFluid(f, toStack(fluid));
        }
        default:
            return true;
        }
    }

    Face pumpFront(GlobalPos const& p) override {
        auto* r = regionFor(p);
        return r ? pipes::pumpFront(r->getBlock(toBlockPos(p))) : Face::Up;
    }
    float pumpSpeed(GlobalPos const& p) override { return k::speedAt(p); }
    int   pumpRange() override { return std::max(1, config::get<PipesConfig>().mechanicalPumpRange); }

    bool isOpenEnd(GlobalPos const& pipe, Face side) override {
        auto* r = regionFor(pipe);
        if (!r) return false;
        BlockPos n = toBlockPos(cp::shifted(pipe, side));
        if (!r->hasChunksAt(n, 0, false)) return false;
        auto const& nb = r->getBlock(n);
        auto        nk = kindOf(nb);
        Face        back = cp::opposite(side);
        if (isPipeLike(nk) && nk != BlockKind::Pump && cp::has(flowFaces(nb), back)) return false;
        if (nk == BlockKind::Pump && cp::axisOf(pipes::pumpFront(nb)) == cp::axisOf(side)) return false;
        if (canProvideFluidWithoutCapability(nb)) return true;
        if (nb.canProvideFullSupport(static_cast<uchar>(back))) return false;
        if (common::fluids::find(common::Access{*r, n, static_cast<int>(back)})) return false;
        // Create: replaceable (and breakable) blocks, or waterloggable ones
        if (!game::replaceable(*r, n) && !canContainWater(nb)) return false;
        return true;
    }

    bool hasHandler(GlobalPos const& t, Face face) override {
        auto* r = regionFor(t);
        if (!r) return false;
        BlockPos bp = toBlockPos(t);
        if (canProvideFluidWithoutCapability(r->getBlock(bp))) return false; // Create: an open end
        return common::fluids::find(common::Access{*r, bp, static_cast<int>(face)}).has_value();
    }

    std::vector<cp::Fluid> handlerTanks(GlobalPos const& t, Face face) override {
        std::vector<cp::Fluid> out;
        if (auto* r = regionFor(t))
            for (auto const& tank : common::fluids::tanks(*r, toBlockPos(t), static_cast<int>(face))) out.push_back(fromStack(tank.contents));
        return out;
    }

    int handlerFill(GlobalPos const& t, Face face, cp::Fluid const& f, bool simulate) override {
        auto* r = regionFor(t);
        return r ? common::fluids::fill(*r, toBlockPos(t), static_cast<int>(face), toStack(f), simulate) : 0;
    }

    cp::Fluid handlerDrain(GlobalPos const& t, Face face, cp::Fluid const& req, bool simulate) override {
        auto* r = regionFor(t);
        return r ? fromStack(common::fluids::drain(*r, toBlockPos(t), static_cast<int>(face), toStack(req), simulate)) : cp::Fluid{};
    }

    int openEndFill(cp::BlockFace const& at, cp::OpenEnd& st, cp::Fluid const& f, bool simulate) override {
        auto* r = regionFor(at.pos);
        return r ? pipes::openEndFill(*r, at, st, f, simulate, time) : 0;
    }

    cp::Fluid openEndDrain(cp::BlockFace const& at, cp::OpenEnd& st, cp::Fluid const& req, bool simulate) override {
        auto* r = regionFor(at.pos);
        return r ? pipes::openEndDrain(*r, at, st, req, simulate) : cp::Fluid{};
    }

    void onFlowCollision(GlobalPos const& p, cp::Fluid const& a, cp::Fluid const& b) override { collisions.emplace_back(p, a, b); }
    void onFlowsChanged(GlobalPos const& p) override { flowChanged.insert(p); }
};

struct Valve {
    float pointer = 0, target = 0, rate = 0;
};

struct Change {
    std::optional<FaceMask> oldFaces;
    bool                    removed = false;
};

struct Runtime {
    WorldHost                                        host;
    cp::Model                                        model{host};
    std::map<GlobalPos, Change>                      changes;
    std::vector<std::function<void()>>               deferred;
    std::map<GlobalPos, PlaceContext>                intents;
    std::map<GlobalPos, Valve>                       valves;
    std::map<std::int64_t, std::int64_t>             lastUse;
    bool                                             loaded = false;
};

Runtime& rt() {
    static Runtime r;
    return r;
}

constexpr char kDbKey[] = "lv:pipes:model";

void saveWorld() {
    auto& r = rt();
    if (!r.loaded || !game::db::available()) return;
    nlohmann::json valves = nlohmann::json::array();
    for (auto const& [p, v] : r.valves) valves.push_back({p.dim, p.x, p.y, p.z, v.pointer, v.target, v.rate});
    game::db::save(kDbKey, {{"model", r.model.save()}, {"valves", valves}});
}

void loadWorld() {
    auto& r = rt();
    r.model.clear();
    r.valves.clear();
    r.changes.clear();
    r.deferred.clear();
    r.intents.clear();
    clearPipeEffects();
    if (auto j = game::db::load(kDbKey); j && j->is_object()) {
        try {
            r.model.load(j->value("model", nlohmann::json::object()));
            for (auto const& v : j->value("valves", nlohmann::json::array())) {
                GlobalPos p{v.at(0).get<int>(), v.at(1).get<int>(), v.at(2).get<int>(), v.at(3).get<int>()};
                r.valves[p] = {v.at(4).get<float>(), v.at(5).get<float>(), v.at(6).get<float>()};
            }
        } catch (std::exception const& e) {
            log().error("pipes: could not read the saved pipe networks: {}", e.what());
        }
    }
    r.loaded = true;
}

void closeWorld() {
    auto& r = rt();
    r.model.clear();
    r.valves.clear();
    r.changes.clear();
    r.deferred.clear();
    r.intents.clear();
    r.host.flowChanged.clear();
    r.host.collisions.clear();
    clearPipeEffects();
    r.loaded = false;
}

int glassSlotFor(cp::Fluid const& f) {
    if (f.empty()) return 0;
    auto const* t = ft::find(f.id);
    int         s = t ? t->renderSlot : 0;
    return s <= 0 || s > 7 ? 6 : s;
}

void refreshGlass(GlobalPos const& p) {
    auto* region = regionFor(p);
    if (!region) return;
    BlockPos    bp    = toBlockPos(p);
    auto const& block = region->getBlock(bp);
    if (kindOf(block) != BlockKind::Glass) return;
    int slot = glassSlotFor(rt().model.visibleFluid(p));
    if (game::intState(block, "levitate:fluid").value_or(0) == slot) return;
    auto const* s = block.getBlockType().getBlockState(HashedString("levitate:fluid"));
    if (!s) return;
    if (auto next = block.setState(*s, slot)) game::setBlock(*region, bp, *next.as_ptr());
}

// ---- valves (Create's FluidValveBlockEntity) ------------------------------------------------------

void valveSpeedChanged(GlobalPos const& p, float speed) {
    auto& v  = rt().valves[p];
    v.target = speed > 0 ? 1.f : 0.f;
    v.rate   = std::clamp(std::abs(speed) / 16.f / 20.f, 0.f, 1.f);
}

void tickValves() {
    auto& r = rt();
    for (auto it = r.valves.begin(); it != r.valves.end();) {
        auto* region = regionFor(it->first);
        if (!region) {
            ++it;
            continue;
        }
        BlockPos    bp    = toBlockPos(it->first);
        auto const& block = region->getBlock(bp);
        if (kindOf(block) != BlockKind::Valve) {
            it = r.valves.erase(it);
            continue;
        }
        auto& v = it->second;
        if (v.pointer < v.target) v.pointer = std::min(v.target, v.pointer + v.rate);
        else if (v.pointer > v.target) v.pointer = std::max(v.target, v.pointer - v.rate);
        bool enabled = game::intState(block, "levitate:enabled").value_or(0) != 0;
        int  next    = -1;
        if (enabled && v.pointer <= 0.f) next = 0;
        if (!enabled && v.pointer >= 1.f) next = 1;
        if (next >= 0) {
            if (auto const* s = block.getBlockType().getBlockState(HashedString("levitate:enabled"))) {
                if (auto b = block.setState(*s, next)) {
                    game::setBlock(*region, bp, *b.as_ptr());
                    log().info("pipes: valve at {} {} {} {}", bp.x, bp.y, bp.z, next ? "opened" : "closed");
                }
            }
        }
        ++it;
    }
}

// ---- block changes ------------------------------------------------------------------------------

void queueChange(GlobalPos const& p, std::optional<FaceMask> oldFaces, bool removed) {
    auto& c = rt().changes[p];
    if (oldFaces) c.oldFaces = c.oldFaces.value_or(0) | *oldFaces;
    c.removed = removed;
}

bool isLiquid(Block const& b) {
    auto const& id = b.getTypeName();
    return id == "minecraft:water" || id == "minecraft:flowing_water" || id == "minecraft:lava" || id == "minecraft:flowing_lava";
}

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    auto& r = rt();
    if (!r.loaded || ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    Block const& before = ev.previousBlock();
    Block const& after  = ev.newBlock();
    BlockPos     pos    = ev.pos();
    auto         gp     = game::globalPos(region, pos);
    auto         kb = kindOf(before), ka = kindOf(after);

    if (kb != BlockKind::None || ka != BlockKind::None) {
        if (ka == BlockKind::None) {
            queueChange(gp, flowFaces(before), true);
        } else if (kb == BlockKind::None) {
            queueChange(gp, std::nullopt, false);
            if (ka == BlockKind::Pump) r.model.addPump(gp);
            PlaceContext ctx;
            bool         intent = false;
            if (auto it = r.intents.find(gp); it != r.intents.end()) {
                ctx    = it->second;
                intent = true;
                r.intents.erase(it);
            }
            // a pipe set without a player (commands, structures) still connects to its neighbours
            if (intent || (ka == BlockKind::Pipe && connections(after) == 0)) {
                int dim = gp.dim;
                r.deferred.push_back([dim, pos, ctx] {
                    if (auto* reg = regionOf(dim)) orientPlaced(*reg, pos, ctx);
                });
            }
            if (ka == BlockKind::Valve) r.valves.try_emplace(gp);
        } else {
            if (flowSignature(before) != flowSignature(after)) queueChange(gp, flowFaces(before), false);
            if (ka == BlockKind::Pump && kb == BlockKind::Pump && pumpFront(before) != pumpFront(after)) {
                r.deferred.push_back([gp] { rt().model.pumpUpdatePressure(gp); });
            }
            if (ka == BlockKind::Glass && kb != BlockKind::Glass) r.host.flowChanged.insert(gp);
        }
    }

    // display-only changes of our own blocks (glass fluid, rims, rotor, valve pointer) don't concern neighbours
    if (kb != BlockKind::None && kb == ka && flowSignature(before) == flowSignature(after)
        && game::intState(before, "levitate:bracket") == game::intState(after, "levitate:bracket"))
        return;
    // neighbours (Create's updateShape / neighborChanged)
    for (Face f : cp::kFaces) {
        auto     o = cp::offsetOf(f);
        BlockPos n{pos.x + o.x, pos.y + o.y, pos.z + o.z};
        if (!region.hasChunksAt(n, 0, false)) continue;
        auto const& nb = region.getBlock(n);
        auto        nk = kindOf(nb);
        if (nk == BlockKind::None) continue;
        Face towards = cp::opposite(f); // from the neighbour to the changed block
        int  dim     = gp.dim;
        if (nk == BlockKind::Pipe) {
            r.deferred.push_back([dim, n, towards] {
                if (auto* reg = regionOf(dim)) updatePipeShape(*reg, n, towards);
            });
            if (!cp::straightAxis(connections(nb))) continue;
        }
        // Create's validateNeighbourChange: only non-pipe, non-fluid neighbours on an open side
        if (isPipeLike(ka) || isLiquid(after)) continue;
        if (nk == BlockKind::Pump ? cp::axisOf(towards) != cp::axisOf(pumpFront(nb)) : !cp::has(flowFaces(nb), towards)) continue;
        queueChange(game::globalPos(region, n), std::nullopt, false);
    }
}

void processChanges() {
    auto& r       = rt();
    auto  changes = std::exchange(r.changes, {});
    for (auto const& [p, c] : changes) {
        auto* region = regionFor(p);
        if (!region) continue;
        auto const& now = region->getBlock(toBlockPos(p));
        if (c.removed && !isPipeLike(kindOf(now))) {
            r.model.propagateChangedPipe(p, c.oldFaces.value_or(0));
            r.model.remove(p);
            continue;
        }
        if (c.oldFaces) {
            // faces closed by the change lead to pipes that also lost their connection here
            r.model.propagateChangedPipe(p, static_cast<FaceMask>(*c.oldFaces | flowFaces(now)));
        } else {
            r.model.propagateChangedPipe(p);
        }
        if (kindOf(now) == BlockKind::Pump) r.model.pumpUpdatePressure(p);
    }
}

void runTick() {
    auto& r = rt();
    if (!r.loaded) return;
    ++r.host.time;
    for (int round = 0; round < 4 && !r.deferred.empty(); ++round) {
        auto work = std::exchange(r.deferred, {});
        for (auto& fn : work) fn();
    }
    processChanges();
    tickValves();
    r.model.tick(r.host.time);
    auto collisions = std::exchange(r.host.collisions, {});
    for (auto const& [p, a, b] : collisions) {
        auto* region = regionFor(p);
        if (region && isPipeLike(kindOf(region->getBlock(toBlockPos(p))))) flowCollision(*region, toBlockPos(p), a, b);
    }
    auto glass = std::exchange(r.host.flowChanged, {});
    for (auto const& p : glass) refreshGlass(p);
    tickPipeEffects(r.model);
    r.intents.clear();
}

// ---- player interaction --------------------------------------------------------------------------

std::string idOf(ItemStack const& s) { return s.isNull() ? std::string{} : s.getTypeName(); }

bool cooldown(Player& p) {
    auto& last = rt().lastUse[p.getOrCreateUniqueID().rawID];
    bool  busy = rt().host.time - last < 5 && last != 0;
    if (!busy) last = rt().host.time;
    return busy;
}

/// The world click position relative to the block's corner.
Vec3 localHit(BlockPos const& at, Vec3 const& hit) {
    auto in = [](float v) { return v >= -0.01f && v <= 1.01f; };
    if (in(hit.x) && in(hit.y) && in(hit.z)) return hit;
    return Vec3{hit.x - at.x, hit.y - at.y, hit.z - at.z};
}

/// Handles the unit's own interactions; true when the click was used.
bool pipesInteract(Player& player, ItemStack const& item, BlockPos const& at, uchar face, Vec3 const& hitIn, Block const& target) {
    auto k = kindOf(target);
    if (k == BlockKind::None || face > 5) return false;
    std::string held = idOf(item);
    Vec3        hit  = localHit(at, hitIn);
    Face        side = static_cast<Face>(face);
    if (k == BlockKind::Smart && common::filterslot::slotAt(target, face, {hit.x, hit.y, hit.z})) return false;
    bool wrenchUse = held == kWrench && !player.isSneaking();
    bool casing    = held == kCopperCasing && (k == BlockKind::Pipe || k == BlockKind::Glass);
    bool bracket   = (held == kMetalBracket || held == kWoodBracket) && (k == BlockKind::Pipe || k == BlockKind::Glass);
    if (wrenchUse) {
        // Create passes when the wrench does nothing here
        auto& region = player.getDimensionBlockSource();
        if (k == BlockKind::Pipe && bracketOf(game::globalPos(region, at)).empty()
            && !cp::wrenchGlassAxis(connections(target), side, hit.x - .5, hit.y - .5, hit.z - .5))
            wrenchUse = false;
        if (k == BlockKind::Smart && cp::axisOf(side) != Axis::Y) wrenchUse = false;
    }
    if (bracket && k == BlockKind::Pipe && !cp::straightAxis(connections(target))) bracket = false;
    if (!wrenchUse && !casing && !bracket) return false;
    if (cooldown(player)) return true;
    int  dim = static_cast<int>(player.getDimensionId());
    auto uid = player.getOrCreateUniqueID();
    rt().deferred.push_back([=] {
        auto level  = ll::service::getLevel();
        auto* region = regionOf(dim);
        if (!level || !region) return;
        auto* p = level->getPlayer(uid);
        if (wrenchUse) {
            wrench(p, *region, at, side, hit.x, hit.y, hit.z);
        } else if (casing) {
            encase(*region, at);
        } else if (bracket && attachBracket(*region, at, held) && p && !p->isCreative()) {
            ItemStack hand = p->getSelectedItem();
            if (idOf(hand) == held) {
                p->setSelectedItem(common::items::withCount(hand, common::items::countOf(hand) - 1));
                p->sendInventory(false);
            }
        }
    });
    return true;
}

bool placesOurBlock(std::string const& id) { return id == kFluidPipe || id == kPump || id == kValve || id == kSmartPipe; }

} // namespace

/// The unit's right-clicks (wrench, casing, brackets) and placement intents (U36: on the shared dispatch).
ia::Result pipesUseOn(ia::UseOnContext const& c) {
    if (!rt().loaded) return ia::Result::Pass;
    auto&    region = c.player.getDimensionBlockSource();
    BlockPos at{c.pos.x, c.pos.y, c.pos.z};
    Vec3     hit{static_cast<float>(c.hit[0]), static_cast<float>(c.hit[1]), static_cast<float>(c.hit[2])};
    auto     face = static_cast<uchar>(c.face);
    if (pipesInteract(c.player, c.item, at, face, hit, c.block)) {
        c.player.sendInventory(false);
        return ia::Result::Consume;
    }
    if (placesOurBlock(c.itemId)) {
        BlockPos place = at;
        if (!game::replaceable(region, at) && face < 6) {
            auto o = cp::offsetOf(static_cast<Face>(face));
            place  = BlockPos{at.x + o.x, at.y + o.y, at.z + o.z};
        }
        Vec3         look = c.player.getViewVector();
        PlaceContext ctx{look.x, look.y, look.z, c.sneaking};
        rt().intents[game::globalPos(region, place)] = ctx;
    }
    return ia::Result::Pass;
}

// ---- public API of the unit ------------------------------------------------------------------------

cp::Model& model() { return rt().model; }

void tickNow() { runTick(); }

cp::Fluid fluidIn(GlobalPos const& pos) { return rt().model.visibleFluid(pos); }

void scheduleChanged(GlobalPos const& pos, std::optional<FaceMask> oldFaces) { queueChange(pos, oldFaces, false); }

float valvePointer(GlobalPos const& pos) {
    auto it = rt().valves.find(pos);
    return it == rt().valves.end() ? 0.f : it->second.pointer;
}

// ---- module ---------------------------------------------------------------------------------------------

namespace {

/// Create's SmartPipeFilterSlot: voxel position per attach face, turned with the pipe.
std::optional<core::common::Vec> smartSlotCentre(Block const& block, int face) {
    auto s = smartState(block);
    double y, z;
    switch (s.face) {
    case cp::AttachFace::Ceiling:
        y = .55, z = 4.6;
        if (face != 0) return std::nullopt;
        break;
    case cp::AttachFace::Wall:
        y = 11.4, z = .55;
        break;
    default:
        y = 15.45, z = 4.625;
        if (face != 1) return std::nullopt;
        break;
    }
    // angleY = horizontal angle of the facing (+180 on a wall), rotated around the block centre
    static constexpr double kAngle[6]{0, 0, 180, 0, 90, 270};
    double angle = kAngle[static_cast<int>(s.facing)] + (s.face == cp::AttachFace::Wall ? 180 : 0);
    double rad   = angle * 3.14159265358979323846 / 180;
    double x0 = 0, z0 = z / 16 - .5;
    double x  = std::cos(rad) * x0 + std::sin(rad) * z0;
    double zz = std::cos(rad) * z0 - std::sin(rad) * x0;
    return core::common::Vec{x + .5, y / 16, zz + .5};
}

class PipesModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) { onBlockChanged(ev); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runTick(); }));
        static bool once = [] {
            game::db::onWorldReady([] { loadWorld(); });
            game::db::onWorldSave([] { saveWorld(); });
            game::db::onWorldClosed([] { closeWorld(); });
            registerOpenPipeEffects();
            net::onCompanionJoined([](Player& p) {
                if (rt().loaded) syncGlassTo(p);
            });
            common::filterslot::registerSlot(kSmartPipe, {"filter", smartSlotCentre, 0.5, [](GlobalPos const& p) { scheduleChanged(p); }});
            // brackets go back to the world when their pipe is gone
            common::blockdata::onErase([](GlobalPos const& p, nlohmann::json const& doc, std::string const& previous) {
                if (previous.empty() || kindOf(previous) == BlockKind::None) return;
                auto pj = doc.find("pipes");
                if (pj == doc.end() || !pj->is_object()) return;
                auto bracket = pj->value("bracket", std::string{});
                if (bracket.empty()) return;
                auto* region = regionOf(p.dim);
                auto  stack  = common::items::make(bracket);
                auto  level  = ll::service::getLevel();
                if (region && stack && level)
                    level->getSpawner().spawnItem(*region, *stack, nullptr, Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, 0);
            });
            k::onSpeedChanged(kPump, [](k::SpeedChange const& c) {
                if (c.removed || !rt().loaded) return;
                rt().model.pumpSpeedChanged(c.pos, c.prevSpeed, c.speed);
                if (std::abs(c.prevSpeed) != std::abs(c.speed))
                    log().info("pipes: pump at {} {} {} now runs at {} RPM", c.pos.x, c.pos.y, c.pos.z, std::abs(c.speed));
            });
            k::onSpeedChanged(kValve, [](k::SpeedChange const& c) {
                if (c.removed || !rt().loaded) return;
                valveSpeedChanged(c.pos, c.speed);
            });
            return true;
        }();
        (void)once;
        mUseOn = ia::onUseOn({}, pipesUseOn);
        if (game::db::available() && !rt().loaded) loadWorld(); // BDS: the world exists already
    }

    void uninstall() {
        ia::remove(mUseOn);
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveWorld();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
    ia::HandlerId                       mUseOn = 0;
};

} // namespace

// ---- `/create pipes <pos>` --------------------------------------------------------------------------

struct PipesCommandParams {
    CommandPosition pos{};
};

namespace {

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::vector<std::string> describe(GlobalPos const& pos) {
    std::vector<std::string> out;
    auto*                    region = regionFor(pos);
    std::string              at     = std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z);
    if (!region || !isPipeLike(kindOf(region->getBlock(toBlockPos(pos))))) {
        out.push_back(tr("lv.pipes.cmd.none", "No pipe at {0}.", at));
        return out;
    }
    out.push_back(tr("lv.pipes.cmd.block", "{0} at {1}", region->getBlock(toBlockPos(pos)).getTypeName(), at));
    auto const* n = rt().model.node(pos);
    if (!n) {
        out.push_back(tr("lv.pipes.cmd.idle", "No pressure."));
        return out;
    }
    for (auto const& c : n->connections) {
        if (!c) continue;
        std::string line = std::string(core::kinetics::faceName(c->side)) + ": " + tr("lv.pipes.cmd.pressure", "pressure in {0} / out {1}", num(c->pressureIn), num(c->pressureOut));
        if (c->flow)
            line += ", " + tr("lv.pipes.cmd.flow", "{0} flowing {1} ({2}%)", c->flow->fluid.id, c->flow->inbound ? "in" : "out", int(c->flow->progress * 100));
        if (c->network) line += ", " + tr("lv.pipes.cmd.network", "moving {0} mB/t to {1} targets", c->network->transferSpeed(), c->network->targets().size());
        out.push_back(line);
    }
    return out;
}

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<PipesCommandParams>().text("pipes").required("pos").execute(
        [](CommandOrigin const& origin, CommandOutput& output, PipesCommandParams const& params) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.pipes.cmd.need_op", "You need operator permission."));
            Vec3  v   = params.pos.getPosition(INT_MAX, origin, Vec3{0.f, 0.f, 0.f});
            auto* dim = origin.getDimension();
            auto  pos = game::globalPos(dim ? static_cast<int>(dim->getDimensionId()) : 0, game::blockPosOf(v));
            for (auto const& line : describe(pos)) {
                output.success(line);
                log().info("pipes: {}", line);
            }
        }
    );
});

} // namespace lv::feature::pipes

LV_REGISTER_MODULE(lv::feature::pipes::PipesModule);
