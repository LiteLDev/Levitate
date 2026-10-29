// Mechanical crafter (Create 6.0.10's MechanicalCrafterBlock / MechanicalCrafterBlockEntity /
// RecipeGridHandler / ConnectedInputHandler), see docs/SPEC.md §3.6.16.
//
// Per-crafter state (phase, countdown, slot, grouped items, cover, connected input) lives in
// BlockData["crafter"]; the in-memory copy is the working state. Block states: levitate:facing
// (2..5, the front), levitate:pointing (0 up, 1 left, 2 down, 3 right: Create's Pointing),
// levitate:covered (shown on the front).

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

#include "nlohmann/json.hpp"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/crafter/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::crafter::crafterlogic {

namespace k   = core::kinetics;
namespace ci  = common::items;
namespace bd  = common::blockdata;
using Json    = nlohmann::json;
using cr::Cell;
using cr::Phase;
using cr::Pointing;

namespace {

constexpr char kDbIndex[] = "lv:crafter:crafters";
constexpr int  kLazyRate  = 20; // Create: setLazyTickRate(20)

struct Info {
    Face     facing   = Face::North;
    Pointing pointing = Pointing::Up;
};

std::map<GlobalPos, State>& states() {
    static std::map<GlobalPos, State> m;
    return m;
}
std::set<GlobalPos>& trackedSet() {
    static std::set<GlobalPos> s;
    return s;
}
/// A crafter being removed: its old block state still counts during its removal (Create's onRemove
/// runs with the block entity's cached state).
std::optional<std::pair<GlobalPos, Info>>& ghost() {
    static std::optional<std::pair<GlobalPos, Info>> g;
    return g;
}

void persistIndex() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : trackedSet()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbIndex, j);
}

Info infoOf(Block const& block) {
    Info i;
    i.facing   = static_cast<Face>(std::clamp(world::intState(block, kFacing, 2), 2, 5));
    i.pointing = static_cast<Pointing>(std::clamp(world::intState(block, kPointing, 0), 0, 3));
    return i;
}

std::optional<Info> infoAt(GlobalPos const& pos) {
    if (auto const& g = ghost(); g && g->first == pos) return g->second;
    auto const* b = world::blockAt(pos);
    if (!b || b->getTypeName() != kCrafter) return std::nullopt;
    return infoOf(*b);
}

Face targetOf(Info const& i) { return cr::targetDirection(i.facing, i.pointing); }

std::optional<GlobalPos> targetingCrafter(GlobalPos const& pos) {
    auto info = infoAt(pos);
    if (!info) return std::nullopt;
    auto t     = world::offset(pos, targetOf(*info));
    auto tinfo = infoAt(t);
    if (!tinfo || tinfo->facing != info->facing) return std::nullopt;
    return t;
}

std::vector<GlobalPos> precedingCrafters(GlobalPos const& pos) {
    std::vector<GlobalPos> out;
    auto                   info = infoAt(pos);
    if (!info) return out;
    Face pointing = targetOf(*info);
    for (Face f : k::kFaces) {
        if (k::axisOf(f) == k::axisOf(info->facing)) continue;
        if (f == pointing) continue;
        auto n     = world::offset(pos, f);
        auto ninfo = infoAt(n);
        if (!ninfo) continue;
        if (targetOf(*ninfo) != k::opposite(f)) continue;
        if (ninfo->facing != info->facing) continue;
        out.push_back(n);
    }
    return out;
}

ItemStack emptyStack() { return ci::empty(); }
bool      empty(ItemStack const& s) { return ci::isEmpty(s); }

Json toJson(State const& s, std::optional<Info> info) {
    Json j;
    j["phase"]     = cr::phaseName(s.phase);
    j["countDown"] = s.countDown;
    j["cover"]     = s.covered;
    j["slot"]      = empty(s.slot) ? Json() : ci::toJson(s.slot);
    Json grid      = Json::array();
    for (auto const& [cell, stack] : s.grid) grid.push_back({cell.first, cell.second, empty(stack) ? Json() : ci::toJson(stack)});
    j["grid"]       = grid;
    j["controller"] = s.isController;
    Json input      = Json::array();
    for (auto const& o : s.input) input.push_back({o.x, o.y, o.z});
    j["input"] = input;
    if (info) {
        j["facing"]   = static_cast<int>(info->facing);
        j["pointing"] = static_cast<int>(info->pointing);
    }
    return j;
}

State fromJson(Json const& j) {
    State s;
    if (!j.is_object()) return s;
    s.phase     = cr::phaseFromName(j.value("phase", std::string("IDLE")));
    s.countDown = j.value("countDown", 0);
    s.covered   = j.value("cover", false);
    if (auto it = j.find("slot"); it != j.end() && !it->is_null()) s.slot = ci::fromJson(*it);
    if (auto it = j.find("grid"); it != j.end() && it->is_array()) {
        for (auto const& e : *it) {
            if (!e.is_array() || e.size() != 3) continue;
            s.grid[{e[0].get<int>(), e[1].get<int>()}] = e[2].is_null() ? emptyStack() : ci::fromJson(e[2]);
        }
    }
    s.isController = j.value("controller", true);
    if (auto it = j.find("input"); it != j.end() && it->is_array() && !it->empty()) {
        s.input.clear();
        for (auto const& e : *it)
            if (e.is_array() && e.size() == 3) s.input.push_back({e[0].get<int>(), e[1].get<int>(), e[2].get<int>()});
    }
    if (s.input.empty()) {
        s.isController = true;
        s.input        = {{0, 0, 0}};
    }
    return s;
}

float speedOf(GlobalPos const& pos) { return kinetics::speedAt(pos); }

using Test = std::function<bool(GlobalPos const&, State const&)>;

/// Create's getAllCraftersOfChainIf (nullopt for loops / failed tests).
std::optional<std::vector<GlobalPos>> chainIf(GlobalPos const& root, Test const& test, bool poweredStart) {
    std::vector<GlobalPos>                                  crafters;
    std::deque<std::pair<GlobalPos, std::optional<GlobalPos>>> frontier{{root, std::nullopt}};
    std::set<GlobalPos>                                     visited;
    bool                                                    anyEmpty = false, allEmpty = true;
    while (!frontier.empty()) {
        auto [current, last] = frontier.front();
        frontier.pop_front();
        if (visited.contains(current)) return std::nullopt;
        auto* s = at(current);
        if (!s) return std::nullopt;
        if (!test(current, *s)) anyEmpty = true;
        else allEmpty = false;
        crafters.push_back(current);
        visited.insert(current);
        if (crafters.size() > 4096) return std::nullopt;
        auto target = targetingCrafter(current);
        if (target && target != last) frontier.push_back({*target, current});
        for (auto const& p : precedingCrafters(current))
            if (p != last) frontier.push_back({p, current});
    }
    if ((anyEmpty && !poweredStart) || allEmpty) return std::nullopt;
    return crafters;
}

void playClick(GlobalPos const& pos, float pitch) {
    // Create's crafter_click: wooden button click (.25) + wooden trapdoor open (.125)
    world::playSound(pos, "random.click", .25f, pitch);
    world::playSound(pos, "random.door_open", .125f, pitch);
}

void eject(GlobalPos const& pos) {
    auto* s = at(pos);
    if (!s) return;
    auto   info = infoAt(pos);
    double ox = 0, oy = 0, oz = 0;
    if (info) {
        auto o = k::offsetOf(info->facing);
        ox     = o.x * .75;
        oz     = o.z * .75;
    }
    double x = pos.x + .5 + ox, y = pos.y + .5 + oy, z = pos.z + .5 + oz;
    for (auto const& [cell, stack] : s->grid)
        if (!empty(stack)) world::dropItem(pos.dim, x, y, z, stack);
    if (!empty(s->slot)) world::dropItem(pos.dim, x, y, z, s->slot);
    s->phase = Phase::Idle;
    s->grid.clear();
    s->slot = emptyStack();
    sendData(pos);
}

void begin(GlobalPos const& pos) {
    auto* s = at(pos);
    if (!s) return;
    s->phase = Phase::Accepting;
    s->grid.clear();
    s->grid[{0, 0}] = s->slot;
    s->slot         = emptyStack();
    if (precedingCrafters(pos).empty()) {
        s->phase     = Phase::Assembling;
        s->countDown = 1;
    }
    sendData(pos);
}

void continueIfAllPrecedingFinished(GlobalPos const& pos) {
    auto* s = at(pos);
    if (!s) return;
    for (auto const& p : precedingCrafters(pos)) {
        auto* ps = at(p);
        if (!ps || ps->phase != Phase::Waiting) return;
    }
    s->phase     = Phase::Assembling;
    s->countDown = 1;
}

bool onlyEmptyItems(State const& s) {
    for (auto const& [c, st] : s.grid)
        if (!empty(st)) return false;
    return true;
}

/// Crafting remainders (Create keeps `getCraftingRemainingItem` of every ingredient).
std::optional<std::string> remainderOf(ItemStack const& stack) {
    auto id = ci::idOf(stack);
    if (id == "minecraft:milk_bucket" || id == "minecraft:water_bucket" || id == "minecraft:lava_bucket"
        || id == "minecraft:powder_snow_bucket")
        return "minecraft:bucket";
    if (id == "minecraft:honey_bottle" || id == "minecraft:dragon_breath") return "minecraft:glass_bottle";
    return std::nullopt;
}

bool isTargetingBelt(GlobalPos const& pos) {
    auto info = infoAt(pos);
    if (!info) return false;
    auto id = world::idAt(world::offset(pos, targetOf(*info)));
    return id == "levitate:belt";
}

void tryInsert(GlobalPos const& pos) {
    auto* s    = at(pos);
    auto  info = infoAt(pos);
    if (!s || !info) return;
    auto* region = world::loadedRegion(pos);
    if (!region) return;
    Face dir    = targetOf(*info);
    auto target = world::offset(pos, dir);
    int  face   = static_cast<int>(k::opposite(dir));
    if (!world::loadedRegion(target) || !ci::has(*region, game::blockPos(target), face)) {
        ejectWholeGrid(pos);
        return;
    }
    bool              changedPhase = s->phase != Phase::Inserting;
    std::vector<Cell> inserted;
    // copy: inserting may call back into crafters (a crafter fed by a crafter)
    auto grid = s->grid;
    for (auto& [cell, stack] : grid) {
        if (empty(stack)) {
            inserted.push_back(cell);
            continue;
        }
        auto remainder = common::items::insert(*region, game::blockPos(target), face, stack, false);
        if (!empty(remainder)) {
            stack = remainder;
            continue;
        }
        inserted.push_back(cell);
    }
    s = at(pos);
    if (!s) return;
    s->grid = grid;
    for (auto const& c : inserted) s->grid.erase(c);
    if (s->grid.empty()) {
        ejectWholeGrid(pos);
    } else {
        s->phase = Phase::Inserting;
        if (!inserted.empty() || changedPhase) sendData(pos);
    }
}

void tickOne(GlobalPos const& pos, State& st, std::uint64_t tick) {
    State* s = &st;
    if (s->phase == Phase::Accepting) return;
    float speed = speedOf(pos);
    int   step  = cr::countDownSpeed(speed);

    bool power = world::powered(pos);
    if (s->wasPowered != power) {
        s->wasPowered = power;
        if (power) {
            log().info("crafter at {} powered: starting an incomplete craft", pos.key());
            checkCompletedRecipe(pos, true);
            s = at(pos);
            if (!s) return;
        }
    }

    if (s->phase == Phase::Assembling) {
        s->countDown -= step;
        if (s->countDown < 0) {
            s->countDown = 0;
            if (targetingCrafter(pos)) {
                s->phase     = Phase::Exporting;
                s->countDown = onlyEmptyItems(*s) ? 0 : 1000;
                sendData(pos);
                return;
            }
            auto result = tryToApplyRecipe(s->grid);
            if (result) {
                std::vector<ItemStack> containers;
                for (auto const& [c, stack] : s->grid)
                    if (auto r = remainderOf(stack))
                        if (auto made = ci::make(*r)) containers.push_back(*made);
                s->grid.clear();
                s->grid[{0, 0}] = *result;
                for (std::size_t i = 0; i < containers.size(); ++i) {
                    // merged LEFT: the container at (i, 0) lands at (i + 1, 0)
                    s->grid[{static_cast<int>(i) + 1, 0}] = containers[i];
                }
                s->phase     = Phase::Crafting;
                s->countDown = 2000;
                log().info("crafter at {} crafted {} x{}", pos.key(), ci::idOf(*result), ci::countOf(*result));
                sendData(pos);
                return;
            }
            log().info("crafter at {}: no recipe matches, ejecting the grid", pos.key());
            ejectWholeGrid(pos);
            return;
        }
    }

    if (s->phase == Phase::Exporting) {
        s->countDown -= step;
        if (s->countDown < 0) {
            s->countDown = 0;
            auto target  = targetingCrafter(pos);
            auto info    = infoAt(pos);
            if (!target || !info) {
                ejectWholeGrid(pos);
                return;
            }
            auto* ts = at(*target);
            if (!ts) {
                ejectWholeGrid(pos);
                return;
            }
            bool wasEmpty = onlyEmptyItems(*s);
            auto off      = cr::mergeOffset(info->pointing);
            for (auto const& [cell, stack] : s->grid) ts->grid[{cell.first + off.first, cell.second + off.second}] = stack;
            s->grid.clear();
            if (!wasEmpty) playClick(pos, static_cast<float>(ts->grid.size()) / 16.f + .5f);
            s->phase     = Phase::Waiting;
            s->countDown = 0;
            sendData(pos);
            continueIfAllPrecedingFinished(*target);
            sendData(*target);
            return;
        }
    }

    if (s->phase == Phase::Crafting) {
        int prev = s->countDown;
        s->countDown -= step;
        auto info = infoAt(pos);
        if (info && tick % 4 == 0 && s->countDown > 1000 && step > 0) {
            // Create spawns crit particles in front of the crafter while crafting (client side)
            auto o = k::offsetOf(info->facing);
            world::particle(pos.dim, pos.x + .5 + o.x * .65, pos.y + .5, pos.z + .5 + o.z * .65, "minecraft:critical_hit_emitter");
        }
        if (s->countDown < 1000 && prev >= 1000) {
            playClick(pos, 2.f);
            world::playSound(pos, "random.break", .5f, .75f); // Create's crafter_craft: item break
        }
        if (s->countDown < 0) {
            s->countDown = 0;
            tryInsert(pos);
            return;
        }
    }

    if (s->phase == Phase::Inserting) {
        if (isTargetingBelt(pos)) tryInsert(pos);
        return;
    }
}

void lazyTick(GlobalPos const& pos) {
    auto* s = at(pos);
    if (!s) return;
    if (s->phase == Phase::Idle && !empty(s->slot)) checkCompletedRecipe(pos, false);
    s = at(pos);
    if (s && s->phase == Phase::Inserting) tryInsert(pos);
}

/// Members of a connected input group, in Create's insertion order (top row first, left to right).
std::vector<GlobalPos> inputMembers(GlobalPos const& controller, State const& cs) {
    auto info = infoAt(controller);
    if (!info) return {};
    std::vector<GlobalPos> out;
    for (auto const& o : cr::inventoryOrder(cs.input, info->facing)) {
        GlobalPos p = controller;
        p.x += o.x;
        p.y += o.y;
        p.z += o.z;
        if (at(p)) out.push_back(p);
    }
    return out;
}

std::optional<GlobalPos> controllerOf(GlobalPos const& pos) {
    auto* s = at(pos);
    if (!s) return std::nullopt;
    if (s->isController) return pos;
    GlobalPos c = pos;
    c.x += s->input[0].x;
    c.y += s->input[0].y;
    c.z += s->input[0].z;
    auto* cs = at(c);
    if (!cs || cs == s || !cs->isController) return std::nullopt;
    return c;
}

void connectivityChanged(GlobalPos const& pos) {
    save(pos);
    sendData(pos);
}

void initAndAddAll(GlobalPos const& crafterPos, std::vector<GlobalPos> const& positions) {
    auto* c = at(crafterPos);
    if (!c) return;
    c->isController = true;
    c->input.clear();
    for (auto const& p : positions) {
        auto* s = at(p);
        if (!s) continue;
        if (!(p == crafterPos)) {
            s->isController = false;
            s->input        = {{crafterPos.x - p.x, crafterPos.y - p.y, crafterPos.z - p.z}};
            save(p);
        }
        c = at(crafterPos);
        c->input.push_back({p.x - crafterPos.x, p.y - crafterPos.y, p.z - crafterPos.z});
    }
    if (c->input.empty() || std::none_of(c->input.begin(), c->input.end(), [](k::Offset const& o) { return o == k::Offset{}; })) {
        // the crafter always belongs to its own group (Create re-adds it through `positions`)
        if (std::none_of(positions.begin(), positions.end(), [&](GlobalPos const& p) { return p == crafterPos; })) c->input.insert(c->input.begin(), k::Offset{});
    }
}

} // namespace

// ---- state access ------------------------------------------------------------------------------

State* at(GlobalPos const& pos) {
    auto& m  = states();
    auto  it = m.find(pos);
    if (ghost() && ghost()->first == pos) return it == m.end() ? nullptr : &it->second;
    if (!infoAt(pos)) {
        if (it != m.end()) m.erase(it);
        return nullptr;
    }
    if (it != m.end()) {
        if (bd::has(pos)) return &it->second;
        m.erase(it); // the data went away (moved by a contraption): start fresh below
    }
    State s;
    if (auto const* d = bd::get(pos); d && d->contains("crafter")) s = fromJson((*d)["crafter"]);
    auto& ref = m[pos] = std::move(s);
    if (!bd::has(pos) || !bd::get(pos)->contains("crafter")) bd::edit(pos)["crafter"] = toJson(ref, infoAt(pos));
    track(pos);
    return &ref;
}

void save(GlobalPos const& pos) {
    auto it = states().find(pos);
    if (it == states().end()) return;
    if (ghost() && ghost()->first == pos) return; // being removed
    bd::edit(pos)["crafter"] = toJson(it->second, infoAt(pos));
}

void track(GlobalPos const& pos) {
    if (trackedSet().insert(pos).second) persistIndex();
}

std::vector<GlobalPos> tracked() { return {trackedSet().begin(), trackedSet().end()}; }

void sendData(GlobalPos const& pos) {
    save(pos);
    auto* s = [&]() -> State* {
        auto it = states().find(pos);
        return it == states().end() ? nullptr : &it->second;
    }();
    if (!s) return;
    Json msg{{"pos", {pos.dim, pos.x, pos.y, pos.z}}, {"phase", cr::phaseName(s->phase)}, {"countDown", s->countDown},
             {"covered", s->covered}, {"speed", speedOf(pos)}};
    msg["slot"] = empty(s->slot) ? "" : ci::idOf(s->slot);
    Json grid   = Json::array();
    for (auto const& [c, st] : s->grid) grid.push_back({c.first, c.second, empty(st) ? "" : ci::idOf(st)});
    msg["grid"] = grid;
    if (auto info = infoAt(pos); info) {
        msg["facing"]   = static_cast<int>(info->facing);
        msg["pointing"] = static_cast<int>(info->pointing);
        msg["target"]   = targetingCrafter(pos).has_value();
    }
    // every companion in the dimension within 64 blocks
    world::forEachCompanionNear(pos, 64, [&](Player& p) { net::send(p, "crafter.state", msg); });
}

// ---- recipe start -------------------------------------------------------------------------------

void checkCompletedRecipe(GlobalPos const& pos, bool poweredStart) {
    if (speedOf(pos) == 0) return;
    Test test = poweredStart ? Test([](GlobalPos const&, State const& s) { return !empty(s.slot); })
                             : Test([](GlobalPos const&, State const& s) { return !empty(s.slot) || s.covered; });
    auto chain = chainIf(pos, test, poweredStart);
    if (!chain) return;
    log().info("crafter at {}: crafting started with {} crafters", pos.key(), chain->size());
    for (auto const& p : *chain) begin(p);
}

void ejectWholeGrid(GlobalPos const& pos) {
    auto chain = chainIf(pos, [](GlobalPos const&, State const&) { return true; }, false);
    if (!chain) return;
    for (auto const& p : *chain) eject(p);
}

// ---- item capability ------------------------------------------------------------------------------

ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    if (empty(stack)) return stack;
    auto ctrl = controllerOf(pos);
    if (!ctrl) return stack;
    auto* cs = at(*ctrl);
    if (!cs) return stack;
    ItemStack rest = stack;
    for (auto const& member : inputMembers(*ctrl, *cs)) {
        if (empty(rest)) break;
        auto* s = at(member);
        if (!s || s->phase != Phase::Idle || s->covered || !empty(s->slot)) continue;
        if (!simulate) {
            s->slot = ci::withCount(rest, 1);
            world::playSound(member, "block.itemframe.add_item", .25f, .5f);
            sendData(member);
            checkCompletedRecipe(member, false); // whenContentsChanged
        }
        rest = ci::withCount(rest, ci::countOf(rest) - 1);
    }
    return rest;
}

// ---- interaction ----------------------------------------------------------------------------------

bool use(Player& player, GlobalPos const& pos, int face, double hx, double hy, double hz) {
    ItemStack held = player.getSelectedItem();
    bool      ok   = use(player, pos, face, hx, hy, hz, held);
    if (ci::countOf(held) != ci::countOf(player.getSelectedItem()) || ci::idOf(held) != ci::idOf(player.getSelectedItem())) {
        player.setSelectedItem(held);
        player.sendInventory(false);
    }
    return ok;
}

bool use(Player& player, GlobalPos const& pos, int face, double, double, double, ItemStack& held) {
    auto info = infoAt(pos);
    auto* s   = at(pos);
    if (!info || !s) return false;
    std::string heldId  = ci::idOf(held);
    if (heldId == kArm) return false;
    bool isHand  = ci::isEmpty(held);
    bool wrenched = heldId == kWrench;
    if (face != static_cast<int>(info->facing)) return false;

    if (s->phase != Phase::Idle && !wrenched) {
        ejectWholeGrid(pos);
        log().info("crafter at {}: grid ejected by {}", pos.key(), player.getRealName());
        return true;
    }
    if (s->phase == Phase::Idle && !isHand && !wrenched) {
        if (heldId == kSlotCover) {
            if (s->covered || !empty(s->slot)) return false;
            s->covered = true;
            world::setStates(pos, {{kCovered, 1}});
            sendData(pos);
            if (!player.isCreative()) {
                held = ci::withCount(held, ci::countOf(held) - 1);
                player.sendInventory(false);
            }
            log().info("crafter at {}: slot covered by {}", pos.key(), player.getRealName());
            return true;
        }
        auto insertedId = heldId;
        auto remainder  = insert(pos, held, false);
        if (ci::countOf(remainder) != ci::countOf(held)) {
            held = remainder;
            player.sendInventory(false);
            log().info("crafter at {}: {} inserted {}", pos.key(), player.getRealName(), insertedId);
        }
        return true;
    }
    s = at(pos);
    if (!s) return false;
    if (empty(s->slot)) {
        if (s->covered && !wrenched) {
            s->covered = false;
            world::setStates(pos, {{kCovered, 0}});
            sendData(pos);
            if (!player.isCreative())
                if (auto cover = ci::make(kSlotCover)) world::give(player, *cover);
            log().info("crafter at {}: slot cover removed by {}", pos.key(), player.getRealName());
            return true;
        }
        return false;
    }
    if (!isHand && !ci::sameItemSameData(held, s->slot)) return false;
    ItemStack inSlot = s->slot;
    s->slot          = emptyStack();
    sendData(pos);
    world::give(player, inSlot);
    log().info("crafter at {}: {} took {}", pos.key(), player.getRealName(), ci::idOf(inSlot));
    return true;
}

void orientPlaced(GlobalPos const& pos, Face clickedFace, GlobalPos const& clickedPos, Face lookFacing, bool sneaking) {
    auto other = infoAt(clickedPos);
    Info info;
    if (!other || sneaking) {
        info.facing = cr::horizontalOpposite(lookFacing);
        if (info.facing != clickedFace) info.pointing = cr::pointingFromFacing(clickedFace, info.facing);
    } else {
        info.facing   = other->facing;
        info.pointing = cr::pointingFromFacing(clickedFace, other->facing);
    }
    world::setStates(pos, {{kFacing, static_cast<int>(info.facing)}, {kPointing, static_cast<int>(info.pointing)}, {kCovered, 0}});
    at(pos);
    log().info("crafter placed at {} facing {} pointing {}", pos.key(), k::faceName(info.facing), cr::xRotation(info.pointing));
}

bool shouldConnect(GlobalPos const& pos, Face face, Face direction) {
    auto ref = infoAt(pos);
    if (!ref) return false;
    if (k::axisOf(direction) == k::axisOf(ref->facing)) return false;
    if (face == ref->facing) return false;
    auto n = infoAt(world::offset(pos, direction));
    if (!n) return false;
    return n->facing == ref->facing;
}

void toggleConnection(GlobalPos const& pos, GlobalPos const& pos2) {
    auto* c1 = at(pos);
    auto* c2 = at(pos2);
    if (!c1 || !c2) return;
    auto controllerOfState = [](GlobalPos const& p, State const& s) {
        GlobalPos c = p;
        c.x += s.input[0].x;
        c.y += s.input[0].y;
        c.z += s.input[0].z;
        return s.isController ? p : c;
    };
    GlobalPos ctrl1 = controllerOfState(pos, *c1);
    GlobalPos ctrl2 = controllerOfState(pos2, *c2);

    if (ctrl1 == ctrl2) {
        // split the group: what stays connected to pos2 goes with it, the rest with pos
        auto* controller = at(ctrl1);
        if (!controller) return;
        std::set<GlobalPos> positions;
        for (auto const& o : controller->input) positions.insert({ctrl1.dim, ctrl1.x + o.x, ctrl1.y + o.y, ctrl1.z + o.z});
        std::deque<GlobalPos>  frontier{pos2};
        std::vector<GlobalPos> splitGroup;
        positions.erase(pos2);
        positions.erase(pos);
        while (!frontier.empty()) {
            auto cur = frontier.front();
            frontier.pop_front();
            for (Face f : k::kFaces) {
                auto next = world::offset(cur, f);
                if (!positions.erase(next)) continue;
                splitGroup.push_back(next);
                frontier.push_back(next);
            }
        }
        // Create re-adds the two crafters themselves through the groups' own members
        std::vector<GlobalPos> rest(positions.begin(), positions.end());
        rest.insert(rest.begin(), pos);
        splitGroup.insert(splitGroup.begin(), pos2);
        initAndAddAll(pos, rest);
        initAndAddAll(pos2, splitGroup);
        connectivityChanged(pos);
        connectivityChanged(pos2);
        log().info("crafter inputs at {} and {} disconnected", pos.key(), pos2.key());
        return;
    }

    auto* k1 = at(ctrl1);
    auto* k2 = at(ctrl2);
    if (!k1 || !k2) return;
    // connectControllers(ctrl1, ctrl2): every member of group 2 now points at ctrl1
    auto members2 = k2->input;
    for (auto const& o : members2) {
        if (o == k::Offset{}) continue;
        GlobalPos p{ctrl2.dim, ctrl2.x + o.x, ctrl2.y + o.y, ctrl2.z + o.z};
        auto*     s = at(p);
        if (!s) continue;
        s->isController = false;
        s->input        = {{ctrl1.x - p.x, ctrl1.y - p.y, ctrl1.z - p.z}};
        at(ctrl1)->input.push_back({p.x - ctrl1.x, p.y - ctrl1.y, p.z - ctrl1.z});
        save(p);
    }
    k2               = at(ctrl2);
    k2->isController = false;
    k2->input        = {{ctrl1.x - ctrl2.x, ctrl1.y - ctrl2.y, ctrl1.z - ctrl2.z}};
    at(ctrl1)->input.push_back({ctrl2.x - ctrl1.x, ctrl2.y - ctrl1.y, ctrl2.z - ctrl1.z});
    connectivityChanged(ctrl1);
    connectivityChanged(ctrl2);
    log().info("crafter inputs at {} and {} connected", pos.key(), pos2.key());
}

// ---- ticking ----------------------------------------------------------------------------------------

void tickAll(std::uint64_t tick) {
    std::vector<GlobalPos> gone;
    for (auto const& pos : std::vector<GlobalPos>(trackedSet().begin(), trackedSet().end())) {
        if (!world::loadedRegion(pos)) continue;
        auto* s = at(pos);
        if (!s) {
            gone.push_back(pos);
            continue;
        }
        tickOne(pos, *s, tick);
        auto phase = static_cast<std::uint64_t>(pos.x * 7 + pos.z * 13 + pos.y);
        if ((tick + phase) % kLazyRate == 0) lazyTick(pos);
        // players coming into range (or back from another dimension) get the state again
        if ((tick + phase) % 100 == 0) sendData(pos);
    }
    if (!gone.empty()) {
        for (auto const& p : gone) trackedSet().erase(p);
        persistIndex();
    }
}

void reset() {
    states().clear();
    trackedSet().clear();
    ghost().reset();
}

namespace {
/// Create's onRemove: drop the cover, eject the grid of the chain, untie the connected input.
void onRemoved(GlobalPos const& pos, Json const& doc) {
    if (!doc.contains("crafter")) return;
    auto const& j = doc["crafter"];
    Info        info;
    info.facing   = static_cast<Face>(std::clamp(j.value("facing", 2), 2, 5));
    info.pointing = static_cast<Pointing>(std::clamp(j.value("pointing", 0), 0, 3));
    auto it       = states().find(pos);
    if (it == states().end()) states()[pos] = fromJson(j);
    State& s = states()[pos];
    ghost()  = std::make_pair(pos, info);
    if (s.covered)
        if (auto cover = ci::make(kSlotCover)) world::dropItem(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, *cover);
    ejectWholeGrid(pos);
    for (Face f : k::kFaces) {
        if (k::axisOf(f) == k::axisOf(info.facing)) continue;
        auto  other = world::offset(pos, f);
        auto* os    = at(other);
        auto* ts    = at(pos);
        if (!os || !ts) continue;
        GlobalPos c1{pos.dim, pos.x + ts->input[0].x, pos.y + ts->input[0].y, pos.z + ts->input[0].z};
        GlobalPos c2{pos.dim, other.x + os->input[0].x, other.y + os->input[0].y, other.z + os->input[0].z};
        if (ts->isController) c1 = pos;
        if (os->isController) c2 = other;
        if (!(c1 == c2)) continue;
        toggleConnection(pos, other);
    }
    ghost().reset();
    states().erase(pos);
    if (trackedSet().erase(pos)) persistIndex();
    log().info("crafter removed at {}", pos.key());
}
} // namespace

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
        if (previous != kCrafter) return;
        try {
            onRemoved(pos, doc);
        } catch (std::exception const& e) {
            ghost().reset();
            log().error("crafter removal at {} failed: {}", pos.key(), e.what());
        }
    });
    common::items::registerBlock(std::string(kCrafter), common::items::Handler{
        [](common::Access const& a) {
            auto pos  = game::globalPos(a.region, a.pos);
            auto ctrl = controllerOf(pos);
            if (!ctrl) return 0;
            return static_cast<int>(inputMembers(*ctrl, *at(*ctrl)).size());
        },
        [](common::Access const& a, int slot) {
            auto pos  = game::globalPos(a.region, a.pos);
            auto ctrl = controllerOf(pos);
            if (!ctrl) return emptyStack();
            auto members = inputMembers(*ctrl, *at(*ctrl));
            if (slot < 0 || slot >= static_cast<int>(members.size())) return emptyStack();
            return at(members[static_cast<std::size_t>(slot)])->slot;
        },
        [](common::Access const& a, ItemStack const& stack, bool simulate) {
            return insert(game::globalPos(a.region, a.pos), stack, simulate);
        },
        [](common::Access const&, common::items::ItemPredicate const&, int, bool) { return emptyStack(); }, // forbidExtraction
    });
    net::onCompanionJoined([](Player& p) {
        for (auto const& pos : tracked()) {
            if (pos.dim != static_cast<int>(p.getDimensionId())) continue;
            if (!world::loadedRegion(pos) || !at(pos)) continue;
            sendData(pos);
        }
    });
}

void uninstall() { reset(); }

} // namespace lv::feature::crafter::crafterlogic
