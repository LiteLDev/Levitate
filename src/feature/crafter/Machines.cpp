// Mechanical saw and mechanical drill, stationary (Create 6.0.10's SawBlock / SawBlockEntity,
// DrillBlock / DrillBlockEntity and BlockBreakingKineticBlockEntity), see docs/SPEC.md §3.6.
//
// Block states: levitate:facing (0..5, Create's Direction order: the front), saw
// levitate:axis_along_first (Create's AXIS_ALONG_FIRST_COORDINATE), levitate:rotor (client mod).
// The saw's inventory lives in BlockData["saw"]; breaking progress is transient (as Create's).

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include "nlohmann/json.hpp"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "core/crafter/Breaking.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/belts/BeltApi.h"
#include "feature/crafter/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::crafter::machines {

namespace k  = core::kinetics;
namespace ci = common::items;
namespace bd = common::blockdata;
using Json   = nlohmann::json;

namespace {

constexpr char kDbIndex[]   = "lv:crafter:machines";
constexpr int  kSawSlots    = 16;
constexpr int  kDrillLazy   = 10; // SmartBlockEntity's default lazy tick rate

std::map<GlobalPos, SawState>& saws() {
    static std::map<GlobalPos, SawState> m;
    return m;
}
std::map<GlobalPos, BreakerState>& breakers() {
    static std::map<GlobalPos, BreakerState> m;
    return m;
}
std::set<GlobalPos>& trackedSet() {
    static std::set<GlobalPos> s;
    return s;
}
std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
double rand01() { return std::uniform_real_distribution<double>(0, 1)(rng()); }

void persistIndex() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : trackedSet()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbIndex, j);
}

bool isSaw(std::string_view id) { return id == kSaw; }
bool isDrill(std::string_view id) { return id == kDrill; }

struct MachineInfo {
    std::string id;
    Face        facing     = Face::Up;
    bool        alongFirst = false;
};
std::optional<MachineInfo> infoAt(GlobalPos const& pos) {
    auto const* b = world::blockAt(pos);
    if (!b) return std::nullopt;
    std::string id = b->getTypeName();
    if (!isSaw(id) && !isDrill(id)) return std::nullopt;
    MachineInfo i;
    i.id         = id;
    i.facing     = static_cast<Face>(std::clamp(world::intState(*b, kFacing, 1), 0, 5));
    i.alongFirst = world::intState(*b, kAlongFirst, 0) != 0;
    return i;
}

bool sawCanProcess(MachineInfo const& i) { return isSaw(i.id) && i.facing == Face::Up; }
bool shouldBreak(MachineInfo const& i) { return isDrill(i.id) || (isSaw(i.id) && k::axisOf(i.facing) != k::Axis::Y); }

Json sawToJson(SawState const& s) {
    Json inv = Json::array();
    for (auto const& st : s.inv) inv.push_back(ci::isEmpty(st) ? Json() : ci::toJson(st));
    return Json{{"inv", inv}, {"remaining", s.remainingTime}, {"duration", s.recipeDuration}, {"applied", s.appliedRecipe},
                {"recipeIndex", s.recipeIndex}};
}
SawState sawFromJson(Json const& j) {
    SawState s;
    s.inv.assign(kSawSlots, ci::empty());
    if (!j.is_object()) return s;
    if (auto it = j.find("inv"); it != j.end() && it->is_array()) {
        for (std::size_t i = 0; i < it->size() && i < kSawSlots; ++i)
            if (!(*it)[i].is_null()) s.inv[i] = ci::fromJson((*it)[i]);
    }
    s.remainingTime  = j.value("remaining", -1.f);
    s.recipeDuration = j.value("duration", 0.f);
    s.appliedRecipe  = j.value("applied", false);
    s.recipeIndex    = j.value("recipeIndex", 0);
    return s;
}

bool invEmpty(SawState const& s) {
    return std::all_of(s.inv.begin(), s.inv.end(), [](ItemStack const& st) { return ci::isEmpty(st); });
}
void invClear(SawState& s) {
    s.inv.assign(kSawSlots, ci::empty());
    s.remainingTime  = 0;
    s.recipeDuration = 0;
    s.appliedRecipe  = false;
}

float speedOf(GlobalPos const& pos) { return kinetics::speedAt(pos); }

// ---- saw processing ---------------------------------------------------------------------------------

void start(GlobalPos const& pos, SawState& s, ItemStack const& inserted) {
    auto info = infoAt(pos);
    if (!info || !sawCanProcess(*info) || invEmpty(s)) return;
    auto opts = sawrecipes::forInput(pos, s.inv[0]);
    if (opts.empty()) {
        s.remainingTime = s.recipeDuration = 10;
        s.appliedRecipe                    = false;
        saveSaw(pos);
        return;
    }
    ++s.recipeIndex;
    if (s.recipeIndex >= static_cast<int>(opts.size())) s.recipeIndex = 0;
    int time         = opts[static_cast<std::size_t>(s.recipeIndex)].time;
    s.remainingTime  = static_cast<float>(cr::sawProcessingTime(time, ci::countOf(inserted)));
    s.recipeDuration = s.remainingTime;
    s.appliedRecipe  = false;
    saveSaw(pos);
}

void addToList(ItemStack const& stack, std::vector<ItemStack>& list) {
    ItemStack rest = stack;
    for (auto& e : list) {
        if (ci::isEmpty(rest)) return;
        if (!ci::sameItemSameData(e, rest)) continue;
        int max   = std::max(1, static_cast<int>(e.getMaxStackSize()));
        int space = max - ci::countOf(e);
        if (space <= 0) continue;
        int move = std::min(space, ci::countOf(rest));
        e        = ci::withCount(e, ci::countOf(e) + move);
        rest     = ci::withCount(rest, ci::countOf(rest) - move);
    }
    if (!ci::isEmpty(rest)) list.push_back(rest);
}

void applyRecipe(GlobalPos const& pos, SawState& s) {
    auto opts = sawrecipes::forInput(pos, s.inv[0]);
    if (opts.empty()) return;
    if (s.recipeIndex >= static_cast<int>(opts.size())) s.recipeIndex = 0;
    auto const& recipe = opts[static_cast<std::size_t>(s.recipeIndex)];
    int         rolls  = ci::countOf(s.inv[0]);
    std::string input  = ci::idOf(s.inv[0]);
    invClear(s);
    std::vector<ItemStack> list;
    for (int roll = 0; roll < rolls; ++roll)
        for (auto const& r : recipe.roll()) addToList(r, list);
    for (std::size_t slot = 0; slot < list.size() && slot + 1 < kSawSlots; ++slot) s.inv[slot + 1] = list[slot];
    log().info("saw at {} cut {} x{} ({})", pos.key(), input, rolls, recipe.id);
    advancements::awardOwner(pos, "saw_processing");
}

void tickSaw(GlobalPos const& pos, MachineInfo const& info, float speed) {
    if (!sawCanProcess(info) || speed == 0) return;
    auto* sp = sawAt(pos);
    if (!sp) return;
    auto& s = *sp;
    if (s.remainingTime == -1) {
        if (!ci::isEmpty(s.inv[0]) && !s.appliedRecipe) start(pos, s, s.inv[0]);
        return;
    }
    s.remainingTime -= cr::sawProcessingSpeed(speed);

    if (s.remainingTime > 0 && !ci::isEmpty(s.inv[0]) && static_cast<int>(s.remainingTime) % 4 == 0) {
        world::particle(pos.dim, pos.x + .5, pos.y + .95, pos.z + .5, "minecraft:basic_crit_particle");
    }
    if (s.remainingTime < 5 && !s.appliedRecipe) {
        bool wood = ci::idOf(s.inv[0]).find("log") != std::string::npos || ci::idOf(s.inv[0]).find("wood") != std::string::npos
                 || ci::idOf(s.inv[0]).find("planks") != std::string::npos || ci::idOf(s.inv[0]).find("stem") != std::string::npos;
        world::playSound(pos, wood ? "dig.wood" : "dig.stone", .6f, 1.f); // Create's saw_activate_{wood,stone}
        applyRecipe(pos, s);
        s.appliedRecipe  = true;
        s.recipeDuration = 20;
        s.remainingTime  = 20;
        saveSaw(pos);
        return;
    }
    auto move = cr::sawItemMovement(info.alongFirst, speed);
    Face moveFacing = k::nearestFace(move.x, move.y, move.z);
    if (s.remainingTime > 0) return;
    s.remainingTime = 0;

    // the next block takes the items when it has Create's DirectBeltInputBehaviour (belts, depots,
    // ejectors, basins, saws, crushing wheels, ... registered with the belts unit; chutes and item
    // drains through their item capability until they register)
    GlobalPos next{pos.dim, pos.x + move.x, pos.y + move.y, pos.z + move.z};
    auto*     region = world::loadedRegion(pos);
    bool      direct = region && world::loadedRegion(next) && belts::hasDirectInput(next);
    bool      viaCap = !direct && region && world::loadedRegion(next) && world::acceptsDirectInput(world::idAt(next))
              && ci::has(*region, game::blockPos(next), static_cast<int>(k::opposite(moveFacing)));
    if (direct || viaCap) {
        bool changed = false;
        for (auto& stack : s.inv) {
            if (ci::isEmpty(stack)) continue;
            ItemStack rest;
            if (direct) {
                belts::TransportedItem t;
                t.stack = stack;
                rest    = belts::insertDirect(next, t, static_cast<int>(moveFacing), false);
            } else {
                rest = common::items::insert(*region, game::blockPos(next), static_cast<int>(k::opposite(moveFacing)), stack, false);
            }
            if (ci::countOf(rest) == ci::countOf(stack)) continue;
            stack   = rest;
            changed = true;
        }
        if (invEmpty(s)) {
            invClear(s);
            s.remainingTime = -1;
            changed         = true;
        }
        if (changed) saveSaw(pos);
        return;
    }

    // eject
    double ox = pos.x + .5 + move.x * .5, oy = pos.y + .5 + .5, oz = pos.z + .5 + move.z * .5;
    for (auto const& stack : s.inv) {
        if (ci::isEmpty(stack)) continue;
        world::dropItem(pos.dim, ox, oy, oz, stack, std::array<double, 3>{move.x * .0625, .125, move.z * .0625});
    }
    invClear(s);
    s.remainingTime = -1;
    saveSaw(pos);
}

// ---- block breaking ---------------------------------------------------------------------------------

bool canBreak(MachineInfo const& info, Block const& target) {
    if (!world::breakable(target)) return false;
    if (isSaw(info.id)) return trees::sawable(target);
    return true;
}

void onBlockBroken(GlobalPos const& machine, MachineInfo const& info, GlobalPos const& target) {
    std::string brokenId = world::idAt(target);
    // Create: drops at the block's centre (randomly offset by .125), no motion
    auto drops = world::destroyBlock(target, true);
    double vx = target.x + .5 + (rand01() - .5) * .25, vy = target.y + .5 + (rand01() - .5) * .25, vz = target.z + .5 + (rand01() - .5) * .25;
    for (auto const& d : drops) world::dropItem(target.dim, vx, vy, vz, d, std::array<double, 3>{0, 0, 0});
    log().info("{} at {} broke {} at {}", isSaw(info.id) ? "saw" : "drill", machine.key(), brokenId, target.key());
    if (!isSaw(info.id)) return;
    // the saw fells the tree standing on the cut (dropItemFromCutTree: thrown away from the saw)
    trees::fell(target, brokenId, [&](GlobalPos const& p, ItemStack const& stack) {
        double dist = std::sqrt(double((p.x - target.x) * (p.x - target.x) + (p.y - target.y) * (p.y - target.y) + (p.z - target.z) * (p.z - target.z)));
        double f    = dist / 20.0;
        world::dropItem(p.dim, p.x + .5, p.y + .5, p.z + .5, stack,
                        std::array<double, 3>{(target.x - machine.x) * f, (target.y - machine.y) * f, (target.z - machine.z) * f});
    });
}

void tickBreaker(GlobalPos const& pos, MachineInfo const& info, float speed, std::uint64_t tick) {
    if (!shouldBreak(info)) return;
    auto& b = breakers()[pos];
    // saws resume right after a break, drills on their lazy tick (Create)
    if (b.ticksUntilNextProgress < 0) {
        if (isSaw(info.id) || (tick + static_cast<std::uint64_t>(pos.x * 3 + pos.z * 5)) % kDrillLazy == 0) b.ticksUntilNextProgress = 1;
    }
    if (speed == 0) return;
    GlobalPos target   = world::offset(pos, info.facing);
    b.breakingPos      = target;
    if (b.ticksUntilNextProgress < 0) return;
    if (b.ticksUntilNextProgress-- > 0) return;

    auto const* block = world::blockAt(target);
    if (!block) return;
    float hardness = world::hardness(*block);
    if (!canBreak(info, *block)) {
        if (b.destroyProgress != 0) {
            b.destroyProgress = 0;
            world::crack(target, -1);
        }
        return;
    }
    float breakSpeed = cr::stationaryBreakSpeed(speed);
    b.destroyProgress += cr::progressStep(breakSpeed, hardness, b.destroyProgress);
    world::playHitSound(target, *block);
    if (b.destroyProgress >= 10) {
        onBlockBroken(pos, info, target);
        b.destroyProgress        = 0;
        b.ticksUntilNextProgress = -1;
        world::crack(target, -1);
        return;
    }
    b.ticksUntilNextProgress = cr::ticksUntilNextProgress(hardness, breakSpeed);
    world::crack(target, b.destroyProgress);
}

// ---- entities -----------------------------------------------------------------------------------------

void tickEntities(GlobalPos const& pos, MachineInfo const& info, float speed) {
    if (speed == 0) return;
    // Create's entityInside: anything but items inside the block (deflated by .1) gets hurt
    for (auto* a : world::entitiesIn(pos.dim, pos.x + .1, pos.y + .1, pos.z + .1, pos.x + .9, pos.y + .9, pos.z + .9)) {
        if (world::isItemEntity(*a)) continue;
        world::hurt(*a, static_cast<float>(cr::drillDamage(speed)), isSaw(info.id) ? kSawDeath : kDrillDeath);
    }
    if (sawCanProcess(info)) {
        // items landing on an upward saw (updateEntityAfterFallOn)
        for (auto* a : world::entitiesIn(pos.dim, pos.x, pos.y + .5, pos.z, pos.x + 1., pos.y + 1., pos.z + 1.)) {
            if (!world::isItemEntity(*a)) continue;
            // Create uses the entity's block position: only items whose centre is over this saw
            auto const& at = a->getPosition();
            if (int(std::floor(at.x)) != pos.x || int(std::floor(at.z)) != pos.z || int(std::floor(at.y)) != pos.y) continue;
            insertItemEntity(pos, *a);
        }
    }
}

std::optional<Face> preferredFacing(GlobalPos const& pos) {
    std::optional<Face> preferred;
    for (Face f : k::kFaces) {
        auto n    = kinetics::nodeAt(world::offset(pos, f));
        if (!n) continue;
        auto const* spec = kinetics::specOf(n->state.id);
        if (!spec || !spec->shaftTowards(n->state, k::opposite(f))) continue;
        // Create's getPreferredFacing: shafts on two different axes make it ambiguous
        if (preferred && k::axisOf(*preferred) != k::axisOf(f)) return std::nullopt;
        preferred = f;
    }
    return preferred;
}

bool prefersConnectionTo(GlobalPos const& pos, Face f) {
    auto n = kinetics::nodeAt(world::offset(pos, f));
    if (!n) return false;
    auto const* spec = kinetics::specOf(n->state.id);
    return spec && spec->shaftTowards(n->state, k::opposite(f));
}

} // namespace

// ---- api ------------------------------------------------------------------------------------------------

std::optional<Face> facingOf(Block const& block) {
    std::string_view id = block.getTypeName();
    if (!isSaw(id) && !isDrill(id)) return std::nullopt;
    return static_cast<Face>(std::clamp(world::intState(block, kFacing, 1), 0, 5));
}

SawState* sawAt(GlobalPos const& pos) {
    auto info = infoAt(pos);
    auto& m   = saws();
    auto  it  = m.find(pos);
    if (!info || !isSaw(info->id)) {
        if (it != m.end()) m.erase(it);
        return nullptr;
    }
    if (it != m.end() && bd::has(pos)) return &it->second;
    SawState s;
    if (auto const* d = bd::get(pos); d && d->contains("saw")) s = sawFromJson((*d)["saw"]);
    else s = sawFromJson(Json());
    auto& ref = m[pos] = std::move(s);
    bd::edit(pos)["saw"] = sawToJson(ref);
    track(pos);
    return &ref;
}

BreakerState* breakerAt(GlobalPos const& pos) {
    auto it = breakers().find(pos);
    return it == breakers().end() ? nullptr : &it->second;
}

void saveSaw(GlobalPos const& pos) {
    auto it = saws().find(pos);
    if (it == saws().end()) return;
    bd::edit(pos)["saw"] = sawToJson(it->second);
}

void track(GlobalPos const& pos) {
    if (trackedSet().insert(pos).second) persistIndex();
}

ItemStack sawInsert(GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    auto info = infoAt(pos);
    if (!info || !sawCanProcess(*info) || ci::isEmpty(stack)) return stack;
    auto* s = sawAt(pos);
    if (!s || !invEmpty(*s)) return stack;
    int take = config().bulkCutting ? ci::countOf(stack) : 1;
    take     = std::min(take, ci::countOf(stack));
    if (!simulate) {
        invClear(*s);
        s->inv[0] = ci::withCount(stack, take);
        start(pos, *s, s->inv[0]);
        if (s->remainingTime == 0) s->remainingTime = -1;
        saveSaw(pos);
    }
    return ci::withCount(stack, ci::countOf(stack) - take);
}

void insertItemEntity(GlobalPos const& pos, Actor& item) {
    auto info = infoAt(pos);
    if (!info || !sawCanProcess(*info) || !item.isAlive()) return;
    auto* s = sawAt(pos);
    if (!s || !invEmpty(*s)) return;
    auto stack = world::itemOf(item);
    auto rest  = sawInsert(pos, stack, false);
    if (ci::countOf(rest) == ci::countOf(stack)) return;
    if (ci::isEmpty(rest)) world::discard(item);
    else world::setItem(item, rest);
}

void orientPlaced(GlobalPos const& pos, std::string_view id, Face lookFacing, Face horizontalLook, bool sneaking) {
    if (isDrill(id)) {
        // DirectionalKineticBlock: away from an unambiguous shaft, else towards the player (sneak: away)
        auto preferred = preferredFacing(pos);
        Face facing    = (!preferred || sneaking) ? (sneaking ? lookFacing : k::opposite(lookFacing)) : k::opposite(*preferred);
        world::setStates(pos, {{kFacing, static_cast<int>(facing)}});
        track(pos);
        log().info("drill placed at {} facing {}", pos.key(), k::faceName(facing));
        return;
    }
    // DirectionalAxisKineticBlock (saw)
    Face facing = k::opposite(lookFacing);
    if (sneaking) facing = k::opposite(facing);
    bool alongFirst = false;
    auto faceAxis   = k::axisOf(facing);
    if (faceAxis != k::Axis::Y) {
        alongFirst          = faceAxis == k::Axis::Z;
        Face positivePerp   = faceAxis == k::Axis::X ? Face::South : Face::East;
        bool shaftAbove     = prefersConnectionTo(pos, Face::Up);
        bool shaftBelow     = prefersConnectionTo(pos, Face::Down);
        bool shaftFront     = prefersConnectionTo(pos, positivePerp);
        bool shaftBack      = prefersConnectionTo(pos, k::opposite(positivePerp));
        if (shaftAbove || shaftBelow) alongFirst = faceAxis == k::Axis::X;
        if (shaftFront || shaftBack) alongFirst = faceAxis == k::Axis::Z;
    } else {
        alongFirst = k::axisOf(horizontalLook) == k::Axis::X;
    }
    world::setStates(pos, {{kFacing, static_cast<int>(facing)}, {kAlongFirst, alongFirst ? 1 : 0}});
    track(pos);
    log().info("saw placed at {} facing {}", pos.key(), k::faceName(facing));
}

void tickAll(std::uint64_t tick) {
    std::vector<GlobalPos> gone;
    for (auto const& pos : std::vector<GlobalPos>(trackedSet().begin(), trackedSet().end())) {
        if (!world::loadedRegion(pos)) continue;
        auto info = infoAt(pos);
        if (!info) {
            gone.push_back(pos);
            continue;
        }
        float speed = speedOf(pos);
        tickBreaker(pos, *info, speed, tick);
        tickSaw(pos, *info, speed);
        tickEntities(pos, *info, speed);
    }
    if (!gone.empty()) {
        for (auto const& p : gone) {
            trackedSet().erase(p);
            if (auto* b = breakerAt(p); b && b->breakingPos && b->destroyProgress) world::crack(*b->breakingPos, -1);
            breakers().erase(p);
            saws().erase(p);
        }
        persistIndex();
    }
}

void reset() {
    saws().clear();
    breakers().clear();
    trackedSet().clear();
}

namespace {
void loadIndex() {
    reset();
    if (auto j = game::db::load(kDbIndex); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) trackedSet().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
}
/// World lifecycle listeners at static init (BDS: the world is ready before modules install).
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] { loadIndex(); });
        game::db::onWorldClosed([] { reset(); });
    }
};
Lifecycle const gLifecycle;
} // namespace

void install() {
    if (game::db::available()) loadIndex();
    bd::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
        if (previous != kSaw || !doc.contains("saw")) return;
        // Create's SawBlock.onRemove: the inventory drops
        auto s = sawFromJson(doc["saw"]);
        for (auto const& st : s.inv)
            if (!ci::isEmpty(st)) world::dropItem(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, st);
        saws().erase(pos);
    });
    // Create's SawBlockEntity has a DirectBeltInputBehaviour (belt ends, other saws hand it items)
    belts::registerDirectInput(std::string(kSaw), belts::DirectInput{
        .canInsertFromSide = [](GlobalPos const&, int) { return true; },
        .insert = [](GlobalPos const& pos, belts::TransportedItem const& item, int, bool simulate) {
            return sawInsert(pos, item.stack, simulate);
        },
    });
    common::items::registerBlock(std::string(kSaw), common::items::Handler{
        [](common::Access const&) { return kSawSlots; },
        [](common::Access const& a, int slot) {
            auto* s = sawAt(game::globalPos(a.region, a.pos));
            if (!s || slot < 0 || slot >= kSawSlots) return ci::empty();
            return s->inv[static_cast<std::size_t>(slot)];
        },
        [](common::Access const& a, ItemStack const& stack, bool simulate) {
            return sawInsert(game::globalPos(a.region, a.pos), stack, simulate);
        },
        [](common::Access const& a, common::items::ItemPredicate const& filter, int amount, bool simulate) {
            auto pos = game::globalPos(a.region, a.pos);
            auto* s  = sawAt(pos);
            if (!s) return ci::empty();
            for (std::size_t i = 1; i < s->inv.size(); ++i) {
                auto& st = s->inv[i];
                if (ci::isEmpty(st) || (filter && !filter(st))) continue;
                int n   = std::min(amount, ci::countOf(st));
                auto out = ci::withCount(st, n);
                if (!simulate) {
                    st = ci::withCount(st, ci::countOf(st) - n);
                    if (invEmpty(*s)) {
                        invClear(*s);
                        s->remainingTime = -1;
                    }
                    saveSaw(pos);
                }
                return out;
            }
            return ci::empty();
        },
    });
    // Create's SawFilterSlot: on top of an upward saw, 3 px beside the blade (choose the cut)
    common::filterslot::registerSlot(std::string(kSaw), common::filterslot::SlotSpec{
        "filter",
        [](Block const& b, int face) -> std::optional<core::common::Vec> {
            if (face != static_cast<int>(Face::Up)) return std::nullopt;
            if (world::intState(b, kFacing, 1) != static_cast<int>(Face::Up)) return std::nullopt;
            bool along = world::intState(b, kAlongFirst, 0) != 0;
            double o   = 3.0 / 16.0;
            return along ? core::common::Vec{.5 + o, 12.5 / 16.0, .5} : core::common::Vec{.5, 12.5 / 16.0, .5 + o};
        },
        .4,
        [](GlobalPos const& pos) { log().info("saw at {}: filter changed", pos.key()); },
    });
}

void uninstall() { reset(); }

} // namespace lv::feature::crafter::machines

// ---- kinetic specs -----------------------------------------------------------------------------------------
namespace {
namespace k = lv::core::kinetics;
using lv::feature::crafter::kAlongFirst;
using lv::feature::crafter::kFacing;

k::Face facingOf(k::BlockState const& s, int fallback) { return static_cast<k::Face>(std::clamp(s.get(kFacing, fallback), 0, 5)); }

k::KineticSpec crafterSpec() {
    k::KineticSpec s;
    s.states       = {kFacing};
    s.rotationAxis = [](k::BlockState const& st) { return std::optional<k::Axis>(k::axisOf(facingOf(st, 2))); };
    s.hasShaftTowards = [](k::BlockState const& st, k::Face f) { return k::axisOf(f) == k::axisOf(facingOf(st, 2)); };
    s.cog             = [](k::BlockState const&) { return k::CogType::Small; };
    s.impact          = 2;
    return s;
}

k::KineticSpec sawSpec() {
    k::KineticSpec s;
    s.states       = {kFacing, kAlongFirst};
    auto axis      = [](k::BlockState const& st) {
        return lv::core::crafter::directionalAxis(facingOf(st, 1), st.get(kAlongFirst, 0) != 0);
    };
    s.rotationAxis    = [axis](k::BlockState const& st) { return std::optional<k::Axis>(axis(st)); };
    s.hasShaftTowards = [axis](k::BlockState const& st, k::Face f) { return k::axisOf(f) == axis(st); };
    s.impact          = 4;
    s.rotorState      = lv::feature::crafter::kRotor;
    s.rotorModel      = "geometry.levitate.mechanical_saw";
    s.rotorTexture    = "levitate_mechanical_saw";
    return s;
}

k::KineticSpec drillSpec() {
    k::KineticSpec s;
    s.states          = {kFacing};
    s.rotationAxis    = [](k::BlockState const& st) { return std::optional<k::Axis>(k::axisOf(facingOf(st, 1))); };
    s.hasShaftTowards = [](k::BlockState const& st, k::Face f) { return f == k::opposite(facingOf(st, 1)); };
    s.impact          = 4;
    s.rotorState      = lv::feature::crafter::kRotor;
    s.rotorModel      = "geometry.levitate.mechanical_drill";
    s.rotorTexture    = "levitate_mechanical_drill";
    s.parts           = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        auto     f = facingOf(*v.state, 1);
        k::RotorPart p;
        p.bone = k::isPositive(f) ? "rotor" : "rotor_neg";
        p.axis = k::axisOf(f);
        out.push_back(p);
    };
    return s;
}
} // namespace

LV_REGISTER_KINETIC_BLOCK("levitate:mechanical_crafter", crafterSpec());
LV_REGISTER_KINETIC_BLOCK("levitate:mechanical_saw", sawSpec());
LV_REGISTER_KINETIC_BLOCK("levitate:mechanical_drill", drillSpec());
