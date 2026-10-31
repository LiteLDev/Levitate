// Chain conveyor (SPEC §2.21, §3.6.25): Create 6.0.10's ChainConveyorBlock(Entity). A kinetic
// block (vertical axis) that connects to other chain conveyors with chains (right-click two of
// them with chains), carries packages around its loop and along its chains, routes them by
// address towards the frogports that catch them, and lets players ride the chains with a wrench.

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "core/packages/Address.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::packages {

namespace items     = common::items;
namespace blockdata = common::blockdata;
namespace k         = lv::kinetics;

namespace {

std::map<GlobalPos, ConveyorState>& states() {
    static std::map<GlobalPos, ConveyorState> m;
    return m;
}

struct Selection {
    GlobalPos pos;
};
std::unordered_map<std::int64_t, Selection>& selections() {
    static std::unordered_map<std::int64_t, Selection> m;
    return m;
}

struct Rider {
    GlobalPos                 conveyor;
    float                     chainPosition = 0;
    std::optional<cp::Offset> connection;
    bool                      flipped    = false;
    int                       catchingUp = 20;
};
std::unordered_map<std::int64_t, Rider>& riders() {
    static std::unordered_map<std::int64_t, Rider> m;
    return m;
}

int gNetId = 0;

Json offsetJson(cp::Offset o) { return Json::array({o.x, o.y, o.z}); }
std::optional<cp::Offset> offsetFrom(Json const& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    return cp::Offset{j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}
Json packageJson(ChainPackage const& p) { return {{"pos", p.chainPosition}, {"item", items::toJson(p.item)}}; }
ChainPackage packageFrom(Json const& j) {
    ChainPackage p;
    p.chainPosition = j.value("pos", 0.f);
    p.item          = items::fromJson(j.value("item", Json()));
    p.netId         = nextNetId();
    return p;
}

float speedOf(GlobalPos const& pos) { return k::speedAt(pos); }

bool canAcceptMorePackages(ConveyorState const& s) {
    // Create counts the travelling *lists* (one per connection that carried packages)
    return int(s.looping.size() + s.travelling.size()) < config().chainConveyorCapacity;
}
bool canAcceptMoreFromOtherConveyor(ConveyorState const& s) { return int(s.looping.size()) < config().chainConveyorCapacity; }

void dropPackage(GlobalPos const& pos, ChainPackage const& box) {
    cp::V3 at = box.world.length() > 0 ? box.world - cp::V3{0, 0.5, 0} : centre(pos);
    dropItem(pos.dim, at, box.item);
}

/// Create's exportToPort: hands a box to the frogport at `portOffset`.
bool exportToPort(GlobalPos const& pos, ConveyorState& s, ChainPackage const& box, cp::Offset portOffset) {
    auto portPos = offset(pos, portOffset);
    if (kindOf(idAt(portPos)) != Kind::Frogport) {
        s.loopPorts.erase(portOffset);
        s.travelPorts.erase(portOffset);
        return false;
    }
    auto& p = port(portPos);
    if (p.animating || isBackedUp(p)) return false;
    startFrogAnimation(portPos, box.item, false);
    return true;
}

void notifyPortToAnticipate(GlobalPos const& pos, cp::Offset portOffset) {
    auto portPos = offset(pos, portOffset);
    if (kindOf(idAt(portPos)) == Kind::Frogport) anticipate(portPos);
}

void removeInvalidConnections(GlobalPos const& pos, ConveyorState& s) {
    bool changed = false;
    for (auto it = s.connections.begin(); it != s.connections.end();) {
        auto target = offset(pos, *it);
        if (!isLoaded(target) || (hasConveyor(target) && conveyor(target).connections.contains(it->negated()))) {
            ++it;
            continue;
        }
        if (auto t = s.travelling.find(*it); t != s.travelling.end()) {
            for (auto const& b : t->second) dropPackage(pos, b);
            s.travelling.erase(t);
        }
        it      = s.connections.erase(it);
        changed = true;
    }
    if (changed) {
        s.statsValid = false;
        saveConveyor(pos);
        k::refresh(pos);
    }
}

void updateBoxWorldPositions(GlobalPos const& pos, ConveyorState& s) {
    prepareStats(pos, s);
    for (auto& [target, list] : s.travelling) {
        auto st = s.stats.find(target);
        if (st == s.stats.end()) continue;
        for (auto& b : list) b.world = cp::travelPosition(st->second, b.chainPosition);
    }
    for (auto& b : s.looping) b.world = cp::loopPosition(corner(pos), b.chainPosition);
}

std::vector<cp::V3> chainPoints(GlobalPos const& pos, ConveyorState& s, cp::Offset connection, int count) {
    prepareStats(pos, s);
    auto it = s.stats.find(connection);
    if (it == s.stats.end()) return {};
    return cp::pointsAlongChains(it->second, centre(pos), count);
}

/// Create's chainDestroyed: chain items along the chain (or at the conveyor).
void chainDestroyed(GlobalPos const& pos, ConveyorState& s, cp::Offset connection, bool spawnDrops) {
    int cost = cp::chainCost(connection);
    for (auto const& p : chainPoints(pos, s, connection, int(std::lround(connection.length() * 8))))
        if (std::rand() % 8 == 0) particle(pos.dim, p, "minecraft:basic_smoke_particle");
    if (!spawnDrops || chainItemIds().empty()) return;
    auto chain = items::make(chainItemIds().front());
    if (!chain) return;
    auto points = chainPoints(pos, s, connection, cost);
    if (!points.empty()) {
        for (auto const& p : points) dropItem(pos.dim, p, *chain);
        return;
    }
    while (cost > 0) {
        dropItem(pos.dim, centre(pos), items::withCount(*chain, std::min(cost, 64)));
        cost -= 64;
    }
}

/// Create's removeConnectionTo (the other end's view): drops the packages travelling to `target`.
bool removeConnectionTo(GlobalPos const& pos, GlobalPos const& target) {
    if (!hasConveyor(pos)) return false;
    auto& s     = conveyor(pos);
    auto  local = diff(pos, target);
    if (!s.connections.contains(local)) return false;
    s.connections.erase(local);
    s.stats.erase(local);
    if (auto t = s.travelling.find(local); t != s.travelling.end()) {
        for (auto const& b : t->second) dropPackage(pos, b);
        s.travelling.erase(t);
    }
    s.statsValid = false;
    saveConveyor(pos);
    k::refresh(pos);
    return true;
}

void giveChains(Player& player, int count) {
    if (chainItemIds().empty()) return;
    auto chain = items::make(chainItemIds().front());
    if (!chain) return;
    while (count > 0) {
        giveOrDrop(player, items::withCount(*chain, std::min(count, 64)));
        count -= 64;
    }
}

/// Ray / AABB slab test: distance along the ray, or nullopt.
std::optional<double> rayBox(cp::V3 from, cp::V3 dir, cp::V3 min, cp::V3 max, double range) {
    double tmin = 0, tmax = range;
    double o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z}, lo[3] = {min.x, min.y, min.z}, hi[3] = {max.x, max.y, max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9) {
            if (o[i] < lo[i] || o[i] > hi[i]) return std::nullopt;
            continue;
        }
        double t1 = (lo[i] - o[i]) / d[i], t2 = (hi[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return std::nullopt;
    }
    return tmin;
}

std::vector<GlobalPos> conveyorsNear(int dim, cp::V3 at, double radius) {
    std::vector<GlobalPos> out;
    for (auto const& p : tracked(Kind::ChainConveyor)) {
        if (p.dim != dim) continue;
        if (centre(p).distanceTo(at) > radius) continue;
        if (!isLoaded(p) || !hasConveyor(p)) continue;
        out.push_back(p);
    }
    return out;
}

} // namespace

// ---- state ---------------------------------------------------------------------------------------

int nextNetId() { return ++gNetId; }

bool hasConveyor(GlobalPos const& pos) { return idAt(pos) == kChainConveyorId; }

std::optional<std::vector<ItemStack>> loopingPackages(GlobalPos const& pos) {
    if (!hasConveyor(pos)) return std::nullopt;
    std::vector<ItemStack> out;
    for (auto const& box : conveyor(pos).looping) out.push_back(box.item);
    return out;
}

ConveyorState& conveyor(GlobalPos const& pos) {
    auto& m  = states();
    auto  it = m.find(pos);
    if (it != m.end()) return it->second;
    ConveyorState s;
    if (auto const* doc = blockdata::get(pos); doc && doc->contains("packages")) {
        auto const& j = (*doc)["packages"];
        for (auto const& c : j.value("connections", Json::array()))
            if (auto o = offsetFrom(c)) s.connections.insert(*o);
        for (auto const& p : j.value("loop", Json::array())) s.looping.push_back(packageFrom(p));
        for (auto const& t : j.value("travel", Json::array())) {
            auto to = offsetFrom(t.value("to", Json()));
            if (!to) continue;
            auto& list = s.travelling[*to];
            for (auto const& p : t.value("packages", Json::array())) list.push_back(packageFrom(p));
        }
        s.reversed     = j.value("reversed", false);
        s.tickReversed = s.reversed;
    }
    return m.emplace(pos, std::move(s)).first->second;
}

void saveConveyor(GlobalPos const& pos) {
    auto it = states().find(pos);
    if (it == states().end()) return;
    auto const& s = it->second;
    Json        conns = Json::array(), loop = Json::array(), travel = Json::array();
    for (auto const& c : s.connections) conns.push_back(offsetJson(c));
    for (auto const& p : s.looping) loop.push_back(packageJson(p));
    for (auto const& [to, list] : s.travelling) {
        Json pk = Json::array();
        for (auto const& p : list) pk.push_back(packageJson(p));
        travel.push_back({{"to", offsetJson(to)}, {"packages", pk}});
    }
    blockdata::edit(pos)["packages"] = {
        {"connections", conns     },
        {"loop",        loop      },
        {"travel",      travel    },
        {"reversed",    s.reversed},
    };
}

void forgetConveyor(GlobalPos const& pos) { states().erase(pos); }

void resetConveyors() {
    states().clear();
    selections().clear();
    riders().clear();
}

void prepareStats(GlobalPos const& pos, ConveyorState& s) {
    float speed = speedOf(pos);
    if (s.reversed != (speed < 0) && speed != 0) {
        s.reversed   = speed < 0;
        s.statsValid = false;
    }
    if (s.statsValid) return;
    s.stats.clear();
    for (auto const& c : s.connections) s.stats[c] = cp::connectionStats(corner(pos), c, speed < 0);
    s.statsValid = true;
}

cp::V3 packagePosition(GlobalPos const& pos, ConveyorState& s, float chainPosition, std::optional<cp::Offset> connection) {
    if (!connection) return cp::loopPosition(corner(pos), chainPosition);
    prepareStats(pos, s);
    auto it = s.stats.find(*connection);
    if (it == s.stats.end()) return {};
    return cp::travelPosition(it->second, chainPosition);
}

bool canAcceptPackagesFor(GlobalPos const& pos, std::optional<cp::Offset> connection) {
    auto& s = conveyor(pos);
    if (!connection && !canAcceptMorePackages(s)) return false;
    if (connection) {
        auto other = offset(pos, *connection);
        if (!hasConveyor(other) || !canAcceptMorePackages(conveyor(other))) return false;
    }
    return true;
}

bool addLoopingPackage(GlobalPos const& pos, ChainPackage box) {
    auto& s = conveyor(pos);
    box.world = cp::loopPosition(corner(pos), box.chainPosition);
    s.looping.push_back(std::move(box));
    saveConveyor(pos);
    return true;
}

bool addTravellingPackage(GlobalPos const& pos, ChainPackage box, cp::Offset connection) {
    auto& s = conveyor(pos);
    if (!s.connections.contains(connection)) return false;
    box.world = packagePosition(pos, s, box.chainPosition, connection);
    s.travelling[connection].push_back(std::move(box));
    saveConveyor(pos);
    return true;
}

// ---- ticking -------------------------------------------------------------------------------------

void tickConveyor(GlobalPos const& pos, bool) {
    auto& s = conveyor(pos);
    if (s.checkInvalid) {
        s.checkInvalid = false;
        removeInvalidConnections(pos, s);
    }
    float rpm             = speedOf(pos);
    float distance        = cp::distancePerTick(rpm);
    float degrees         = cp::degreesPerTick(rpm);
    prepareStats(pos, s);
    // compare with this conveyor's own last tick: other code may have run prepareStats already
    bool reversedBefore = s.tickReversed;
    s.tickReversed      = s.reversed;

    s.routing.tick();
    if (s.routing.shouldAdvertise()) {
        for (auto const& c : s.connections) {
            auto other = offset(pos, c);
            if (isLoaded(other) && hasConveyor(other)) s.routing.advertiseTo(c, conveyor(other).routing);
        }
        s.routing.changed    = false;
        s.routing.lastUpdate = 0;
    }

    if (rpm == 0) {
        updateBoxWorldPositions(pos, s);
        return;
    }
    bool changed = false;

    if (reversedBefore != s.reversed) {
        // boxes on the chains continue on the other conveyor's chain, which now leads away
        for (auto& [offsetTo, list] : s.travelling) {
            auto other = offset(pos, offsetTo);
            if (!hasConveyor(other)) continue;
            for (auto it = list.begin(); it != list.end();) {
                if (it->justFlipped) {
                    ++it;
                    continue;
                }
                ChainPackage box = *it;
                box.justFlipped  = true;
                float length     = float(offsetTo.length() - 22 / 16.0);
                box.chainPosition = length - box.chainPosition;
                addTravellingPackage(other, box, offsetTo.negated());
                it      = list.erase(it);
                changed = true;
            }
        }
    }

    auto const travelPorts = s.travelPorts; // exporting may change the maps
    auto const loopPorts   = s.loopPorts;
    for (auto& [target, list] : s.travelling) {
        auto st = s.stats.find(target);
        if (st == s.stats.end()) continue;
        auto const& stats = st->second;
        for (auto it = list.begin(); it != list.end();) {
            auto& box       = *it;
            box.justFlipped = false;
            float prev      = box.chainPosition;
            box.chainPosition = std::min(stats.chainLength, box.chainPosition + distance);
            float anticipatePosition = std::min(stats.chainLength, box.chainPosition + distance * 4);
            bool  removed = false;
            for (auto const& [portOffset, portInfo] : travelPorts) {
                if (prev > portInfo.chainPosition) continue;
                if (!portInfo.connection || *portInfo.connection != target) continue;
                bool notYet = box.chainPosition < portInfo.chainPosition;
                if (notYet && anticipatePosition < portInfo.chainPosition) continue;
                if (!matchAddress(addressOf(box.item), portInfo.filter)) continue;
                if (notYet) {
                    notifyPortToAnticipate(pos, portOffset);
                    continue;
                }
                if (!exportToPort(pos, s, box, portOffset)) continue;
                removed = true;
                break;
            }
            if (removed) {
                it      = list.erase(it);
                changed = true;
                continue;
            }
            if (box.chainPosition < stats.chainLength) {
                ++it;
                continue;
            }
            // onto the loop of the next conveyor
            auto other = offset(pos, target);
            if (hasConveyor(other)) {
                ChainPackage moved  = box;
                moved.chainPosition = cp::wrapAngle(stats.tangentAngle + 180 + 2 * 35 * (s.reversed ? -1 : 1));
                addLoopingPackage(other, moved);
                it      = list.erase(it);
                changed = true;
                continue;
            }
            ++it;
        }
    }

    for (auto it = s.looping.begin(); it != s.looping.end();) {
        auto& box       = *it;
        box.justFlipped = false;
        float prev      = box.chainPosition;
        box.chainPosition = cp::wrapAngle(box.chainPosition + degrees);
        float anticipatePosition = cp::wrapAngle(box.chainPosition + degrees * 4);
        bool  removed = false;
        for (auto const& [portOffset, portInfo] : loopPorts) {
            bool notYet = !cp::loopThresholdCrossed(box.chainPosition, prev, portInfo.chainPosition, s.reversed);
            if (notYet && !cp::loopThresholdCrossed(anticipatePosition, prev, portInfo.chainPosition, s.reversed)) continue;
            if (!matchAddress(addressOf(box.item), portInfo.filter)) continue;
            if (notYet) {
                notifyPortToAnticipate(pos, portOffset);
                continue;
            }
            if (!exportToPort(pos, s, box, portOffset)) continue;
            removed = true;
            break;
        }
        if (removed) {
            it      = s.looping.erase(it);
            changed = true;
            continue;
        }
        bool left = false;
        for (auto const& c : s.connections) {
            auto other = offset(pos, c);
            if (hasConveyor(other) && !canAcceptMoreFromOtherConveyor(conveyor(other))) continue;
            auto st = s.stats.find(c);
            if (st == s.stats.end()) continue;
            if (!cp::loopThresholdCrossed(box.chainPosition, prev, st->second.tangentAngle, s.reversed)) continue;
            if (s.routing.exitFor(addressOf(box.item)) != c) continue;
            ChainPackage moved  = box;
            moved.chainPosition = 0;
            s.travelling[c].push_back(moved);
            left = true;
            break;
        }
        if (left) {
            it      = s.looping.erase(it);
            changed = true;
            continue;
        }
        ++it;
    }
    updateBoxWorldPositions(pos, s);
    if (changed) {
        saveConveyor(pos);
    }
}

// ---- connections -----------------------------------------------------------------------------------

bool connectConveyors(Player* player, GlobalPos const& a, GlobalPos const& b) {
    if (!hasConveyor(a) || !hasConveyor(b) || a == b) return false;
    auto& sa = conveyor(a);
    auto& sb = conveyor(b);
    bool  added = sa.connections.insert(diff(a, b)).second;
    added |= sb.connections.insert(diff(b, a)).second;
    sa.statsValid = sb.statsValid = false;
    saveConveyor(a);
    saveConveyor(b);
    k::refresh(a);
    k::refresh(b);
    log().info("packages: chain conveyors at {} and {} connected{}", a.key(), b.key(), player ? " by " + player->getRealName() : "");
    return added;
}

void disconnectConveyors(GlobalPos const& a, GlobalPos const& b, bool spawnDrops, Player* refundTo) {
    if (!hasConveyor(a)) return;
    auto& s     = conveyor(a);
    auto  local = diff(a, b);
    if (!s.connections.contains(local)) return;
    if (refundTo && !refundTo->isCreative()) giveChains(*refundTo, cp::chainCost(local));
    chainDestroyed(a, s, local, spawnDrops);
    playSound(a.dim, centre(a), "dig.chain");
    removeConnectionTo(a, b);
    removeConnectionTo(b, a);
    log().info("packages: chain between {} and {} removed", a.key(), b.key());
}

void onConveyorRemoved(GlobalPos const& pos, Json const& data, bool creativeBreak) {
    // rebuild the removed conveyor's view from its saved data
    ConveyorState s;
    for (auto const& c : data.value("connections", Json::array()))
        if (auto o = offsetFrom(c)) s.connections.insert(*o);
    for (auto const& p : data.value("loop", Json::array())) s.looping.push_back(packageFrom(p));
    for (auto const& t : data.value("travel", Json::array()))
        if (auto to = offsetFrom(t.value("to", Json())))
            for (auto const& p : t.value("packages", Json::array())) s.travelling[*to].push_back(packageFrom(p));
    states().erase(pos);
    for (auto const& c : s.connections) s.stats[c] = cp::connectionStats(corner(pos), c, s.reversed);
    s.statsValid = true;
    for (auto const& c : s.connections) {
        chainDestroyed(pos, s, c, !creativeBreak);
        auto other = offset(pos, c);
        if (isLoaded(other)) removeConnectionTo(other, pos);
    }
    for (auto& b : s.looping) {
        b.world = cp::loopPosition(corner(pos), b.chainPosition);
        dropPackage(pos, b);
    }
    for (auto& [to, list] : s.travelling)
        for (auto& b : list) {
            b.world = cp::travelPosition(s.stats[to], b.chainPosition);
            dropPackage(pos, b);
        }
    broadcast(pos, "packages.conveyor", {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}, {"removed", true}});
    log().info("packages: chain conveyor at {} removed ({} connection(s))", pos.key(), s.connections.size());
}

bool conveyorPlacementBlocked(GlobalPos const& pos) {
    // Create: no two chain conveyors next to each other (3x3 around, same height)
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z)
            if ((x != 0 || z != 0) && hasConveyor({pos.dim, pos.x + x, pos.y, pos.z + z})) return true;
    return false;
}

void clearChainSelection(Player& player, bool message) {
    if (selections().erase(player.getOrCreateUniqueID().rawID) && message)
        actionBar(player, trFor(player, "create.chain_conveyor.selection_cleared", "Selection Cleared"));
}

bool useConveyorWithChain(Player& player, GlobalPos const& pos) {
    auto key  = player.getOrCreateUniqueID().rawID;
    auto fail = [&](char const* keySuffix, char const* fallback) {
        actionBar(player, "§c" + trFor(player, std::string("create.chain_conveyor.") + keySuffix, fallback));
        log().info("packages: chain connection refused for {}: {}", player.getRealName(), keySuffix);
        playSound(pos.dim, centre(pos), "levitate.packages.deny");
        return false;
    };
    if (int(conveyor(pos).connections.size()) >= config().maxChainConveyorConnections)
        return fail("cannot_add_more_connections", "Cannot add more connections to this Conveyor");
    auto it = selections().find(key);
    if (it == selections().end() || it->second.pos.dim != pos.dim || !hasConveyor(it->second.pos)) {
        if (player.isSneaking()) return false;
        selections()[key] = {pos};
        log().info("packages: {} selected the chain conveyor at {}", player.getRealName(), pos.key());
        actionBar(player, trFor(player, "create.chain_conveyor.select_second", "Select a second conveyor to connect"));
        return true;
    }
    auto first = it->second.pos;
    selections().erase(it);
    if (player.isSneaking()) {
        actionBar(player, trFor(player, "create.chain_conveyor.selection_cleared", "Selection Cleared"));
        return false;
    }
    if (first == pos) return false;
    auto d = diff(first, pos);
    if (auto err = cp::validateConnection(d, config().maxChainConveyorLength)) {
        static std::map<std::string, char const*> const fallbacks{
            {"too_far",                   "Too far apart"            },
            {"too_close",                 "Too close together"       },
            {"cannot_connect_vertically", "Cannot connect vertically"},
            {"too_steep",                 "Slope too steep"          },
        };
        return fail(err->c_str(), fallbacks.at(*err));
    }
    auto& target = conveyor(pos);
    if (int(target.connections.size()) >= config().maxChainConveyorConnections)
        return fail("cannot_add_more_connections", "Cannot add more connections to this Conveyor");
    if (target.connections.contains(d.negated())) return fail("already_connected", "Conveyors are already connected");
    if (!player.isCreative()) {
        int cost = cp::chainCost(d);
        if (!takeItems(player, chainItemIds(), cost, true)) return fail("not_enough_chains", "Not holding enough chains");
        takeItems(player, chainItemIds(), cost, false);
    }
    connectConveyors(&player, first, pos);
    playSound(pos.dim, centre(pos), "place.chain", 1.f, 0.8f);
    return true;
}

void refundChainsOnWrench(Player& player, GlobalPos const& pos) {
    // Create's onSneakWrenched: the chains go back into the inventory, the block drops none
    auto& s = conveyor(pos);
    if (!player.isCreative())
        for (auto const& c : s.connections) giveChains(player, cp::chainCost(c));
    for (auto const& c : std::set<cp::Offset>(s.connections)) {
        auto other = offset(pos, c);
        removeConnectionTo(other, pos);
        removeConnectionTo(pos, other); // drops the boxes on this end's chain too
    }
}

// ---- picking ---------------------------------------------------------------------------------------

std::optional<ChainPick> pickChain(Player& player, bool dismantling, double maxDistance) {
    cp::V3 from = eyePos(player), dir = lookVec(player).normalized();
    std::optional<ChainPick> best;
    double                   bestT = maxDistance;
    for (auto const& pos : conveyorsNear(player.getDimensionId(), from, maxDistance + 40)) {
        auto& s = conveyor(pos);
        prepareStats(pos, s);
        // the loop around the wheel (Create's ChainConveyorBB)
        if (!dismantling) {
            cp::V3 c = corner(pos) + cp::V3{0.5, 0, 0.5};
            if (auto t = rayBox(from, dir, c + cp::V3{-1.25, 0, -1.25}, c + cp::V3{1.25, 1, 1.25}, bestT); t && *t < bestT) {
                cp::V3 hit = from + dir * *t;
                float  ang = cp::wrapAngle(float(std::atan2(hit.x - c.x, hit.z - c.z) * 180.0 / 3.14159265358979));
                bestT      = *t;
                best       = ChainPick{pos, ang, std::nullopt, cp::loopPosition(corner(pos), ang), *t};
            }
        }
        // the chains (Create's ChainConveyorOBB)
        for (auto const& c : s.connections) {
            auto st = s.stats.find(c);
            if (st == s.stats.end()) continue;
            auto const& stats = st->second;
            cp::V3      s2    = cp::mirroredStart(stats, centre(pos));
            cp::V3      d     = stats.end - stats.start;
            for (auto const& seg : {std::pair{stats.start, stats.end}, std::pair{s2, s2 + d}, std::pair{(stats.start + s2) * 0.5, (stats.start + s2) * 0.5 + d}}) {
                auto hit = cp::raySegment(from, dir, bestT, seg.first, seg.second, 0.3);
                if (!hit || hit->t >= bestT) continue;
                bestT          = hit->t;
                float chainPos = float(hit->along * stats.chainLength);
                best           = ChainPick{pos, chainPos, c, cp::travelPosition(stats, chainPos), hit->t};
            }
        }
    }
    return best;
}

std::optional<PackagePick> pickPackage(Player& player, double maxDistance) {
    cp::V3 from = eyePos(player), dir = lookVec(player).normalized();
    std::optional<PackagePick> best;
    double                     bestT = maxDistance;
    for (auto const& pos : conveyorsNear(player.getDimensionId(), from, maxDistance + 40)) {
        auto& s     = conveyor(pos);
        auto  check = [&](ChainPackage const& b, std::optional<cp::Offset> conn) {
            cp::V3 target = b.world - cp::V3{0, 9 / 16.0, 0};
            cp::V3 min = target + cp::V3{-0.45, -0.25 - 0.45, -0.45}, max = target + cp::V3{0.45, 0.25 + 0.45, 0.45};
            if (auto t = rayBox(from, dir, min, max, bestT); t && *t < bestT) {
                bestT = *t;
                best  = PackagePick{pos, conn, b.netId, *t};
            }
        };
        for (auto const& b : s.looping) check(b, std::nullopt);
        for (auto const& [c, list] : s.travelling)
            for (auto const& b : list) check(b, c);
    }
    return best;
}

bool takePackageFromChain(Player& player, PackagePick const& pick) {
    if (!hasConveyor(pick.conveyor)) return false;
    auto& s    = conveyor(pick.conveyor);
    auto  take = [&](std::vector<ChainPackage>& list) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->netId != pick.netId) continue;
            auto box = it->item;
            list.erase(it);
            giveOrDrop(player, box);
            saveConveyor(pick.conveyor);
            log().info("packages: {} took a package off the chain conveyor at {}", player.getRealName(), pick.conveyor.key());
            return true;
        }
        return false;
    };
    if (!pick.connection) return take(s.looping);
    auto it = s.travelling.find(*pick.connection);
    return it != s.travelling.end() && take(it->second);
}

bool putPackageOnChain(Player& player, ChainPick const& pick) {
    auto held = player.getSelectedItem();
    if (!isPackage(held) || !hasConveyor(pick.conveyor)) return false;
    if (!canAcceptPackagesFor(pick.conveyor, pick.connection)) return false;
    ChainPackage box;
    box.chainPosition = pick.chainPosition;
    box.item          = items::withCount(held, 1);
    box.netId         = nextNetId();
    bool ok           = pick.connection ? addTravellingPackage(pick.conveyor, box, *pick.connection) : addLoopingPackage(pick.conveyor, box);
    if (!ok) return false;
    if (!player.isCreative()) {
        player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
        player.sendInventory(false);
    }
    log().info("packages: {} hung a package for '{}' on the chain conveyor at {}", player.getRealName(), addressOf(box.item), pick.conveyor.key());
    return true;
}

// ---- riding ----------------------------------------------------------------------------------------

namespace {
void sendMotion(Player& player, Vec3 const& motion) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
    if (!packet) return;
    auto& pkt      = static_cast<SetActorMotionPacket&>(*packet);
    pkt.mRuntimeId = player.getRuntimeID();
    pkt.mMotion    = motion;
    pkt.sendTo(player);
}
} // namespace

void startRiding(Player& player, ChainPick const& pick) {
    Rider r;
    r.conveyor      = pick.conveyor;
    r.chainPosition = pick.chainPosition;
    r.connection    = pick.connection;
    r.flipped       = speedOf(pick.conveyor) < 0;
    riders()[player.getOrCreateUniqueID().rawID] = r;
    actionBar(player, trFor(player, "lv.packages.chain_riding", "Sneak to dismount"));
    auto feet = player.getFeetPos();
    playSound(player.getDimensionId(), {feet.x, feet.y, feet.z}, "hit.chain", 1.f, 0.5f);
    log().info("packages: {} started riding the chain conveyor at {}", player.getRealName(), pick.conveyor.key());
}

void tickRiders() {
    for (auto it = riders().begin(); it != riders().end();) {
        auto*  player = playerById(it->first);
        auto&  r      = it->second;
        auto   stop   = [&](char const* why) {
            if (player) {
                auto feet = player->getFeetPos();
                playSound(player->getDimensionId(), {feet.x, feet.y, feet.z}, "hit.chain", 0.75f, 0.35f);
                log().info("packages: {} stopped riding ({})", player->getRealName(), why);
            }
            it = riders().erase(it);
        };
        if (!player) {
            it = riders().erase(it);
            continue;
        }
        if (items::idOf(player->getSelectedItem()) != kWrenchId) {
            stop("not holding a wrench");
            continue;
        }
        if (player->isSneaking() || !hasConveyor(r.conveyor) || int(player->getDimensionId()) != r.conveyor.dim) {
            stop("dismounted");
            continue;
        }
        auto* s = &conveyor(r.conveyor);
        if (r.connection && !s->connections.contains(*r.connection)) {
            stop("chain removed");
            continue;
        }
        prepareStats(r.conveyor, *s);
        // Create's updateTargetPosition
        float rpm = speedOf(r.conveyor);
        if (r.connection) {
            auto const& stats = s->stats[*r.connection];
            if (r.flipped != (rpm < 0)) {
                r.flipped       = rpm < 0;
                r.conveyor      = offset(r.conveyor, *r.connection);
                r.chainPosition = stats.chainLength - r.chainPosition;
                r.connection    = r.connection->negated();
            } else {
                r.chainPosition = std::min(stats.chainLength, r.chainPosition + cp::distancePerTick(rpm));
                if (r.chainPosition >= stats.chainLength && hasConveyor(offset(r.conveyor, *r.connection))) {
                    r.chainPosition = cp::wrapAngle(stats.tangentAngle + 180 + 2 * 35 * (s->reversed ? -1 : 1));
                    r.conveyor      = offset(r.conveyor, *r.connection);
                    r.connection.reset();
                }
            }
        } else {
            float prev      = r.chainPosition;
            r.chainPosition = cp::wrapAngle(r.chainPosition + cp::degreesPerTick(rpm));
            // leave the loop towards the chain the player looks along
            cp::V3                    look = lookVec(*player).normalized();
            std::optional<cp::Offset> nearest;
            double                    bestDiff = std::numeric_limits<double>::max();
            for (auto const& c : s->connections) {
                cp::V3 n{double(c.x), double(c.y), double(c.z)};
                double dd = (n.normalized() - look).length();
                if (dd * dd > bestDiff) continue;
                nearest  = c;
                bestDiff = dd * dd;
            }
            if (nearest && cp::loopThresholdCrossed(r.chainPosition, prev, s->stats[*nearest].tangentAngle, s->reversed)) {
                r.chainPosition = 0;
                r.connection    = nearest;
            }
        }
        s = &conveyor(r.conveyor);
        prepareStats(r.conveyor, *s);
        cp::V3 target;
        if (r.connection) {
            auto const& stats = s->stats[*r.connection];
            target            = cp::travelPosition(stats, r.chainPosition);
        } else {
            target = corner(r.conveyor) + cp::V3{0.5, 0, 0.5} + cp::rotateY({0, 0.25, 1}, r.chainPosition);
        }
        // Create pulls the top of the player (+0.5) to the chain
        auto   feet = player->getFeetPos();
        cp::V3 top{feet.x, feet.y + 1.8 + 0.5, feet.z};
        cp::V3 d = target - top;
        if (r.catchingUp > 0) r.catchingUp--;
        if (r.catchingUp == 0 && (d.length() > 3 || d.y < -1)) {
            stop("fell off");
            continue;
        }
        // Create's ChainConveyorRidingHandler: motion = motion * 0.75 + diff * 0.25, no fall damage;
        // the motion goes to the rider's client like vanilla knockback (SetActorMotion), so every
        // client (with or without the mod) moves smoothly and the server's movement agrees
        Vec3 const& was = player->getPosDelta();
        Vec3        motion{float(was.x * 0.75 + d.x * 0.25), float(was.y * 0.75 + d.y * 0.25), float(was.z * 0.75 + d.z * 0.25)};
        const_cast<Vec3&>(player->getPosDelta()) = motion;
        sendMotion(*player, motion);
        player->setFallDistance(0);
        ++it;
    }
}

// ---- kinetics --------------------------------------------------------------------------------------

void installConveyorKinetics() {
    k::KineticSpec spec;
    spec.rotationAxis    = [](k::BlockState const&) { return std::optional<k::Axis>(k::Axis::Y); };
    spec.hasShaftTowards = [](k::BlockState const&, k::Face f) { return f == k::Face::Up || f == k::Face::Down; };
    spec.impact          = 1;
    spec.extraNeighbours = [](k::NodeView const& self, std::vector<GlobalPos>& out) {
        if (!hasConveyor(self.pos)) return;
        for (auto const& c : conveyor(self.pos).connections) out.push_back(offset(self.pos, c));
    };
    spec.propagate = [](k::NodeView const& from, k::NodeView const& to, k::Offset, bool, bool) -> float {
        if (!hasConveyor(from.pos)) return 0;
        if (!conveyor(from.pos).connections.contains(diff(from.pos, to.pos))) return 0;
        return hasConveyor(to.pos) ? 1.f : 0.f;
    };
    spec.customConnection = [](k::NodeView const& self, k::NodeView const& other) {
        return hasConveyor(self.pos) && conveyor(self.pos).connections.contains(diff(self.pos, other.pos));
    };
    spec.rotorState   = kRotor;
    spec.rotorModel   = "geometry.levitate.chain_conveyor";
    spec.rotorTexture = "levitate_chain_conveyor";
    k::registerBlock(kChainConveyorId, std::move(spec));
}

} // namespace lv::feature::packages
