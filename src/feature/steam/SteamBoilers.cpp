// Steam unit: boilers (Create's BoilerData), steam engines (SteamEngineBlockEntity), powered
// shafts (PoweredShaftBlockEntity) and steam whistles (WhistleBlockEntity), ticked once per
// server tick for the indexed blocks of loaded chunks.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FluidRegistry.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::steam {

namespace blockdata = common::blockdata;

namespace {

constexpr char kDbIndex[] = "lv:steam:index";

struct ShaftState {
    std::optional<GlobalPos> engine;
    float                    efficiency   = 0;
    int                      direction    = 1;
    int                      initialTicks = 3;
    bool                     applied      = false;
};
struct WhistleState {
    bool sounding  = false;
    int  lastPitch = -1;
};
struct EngineState {
    std::uint64_t nextChuff = 0;
};

std::map<GlobalPos, ShaftState>&   shaftStates() {
    static std::map<GlobalPos, ShaftState> m;
    return m;
}
std::map<GlobalPos, WhistleState>& whistleStates() {
    static std::map<GlobalPos, WhistleState> m;
    return m;
}
std::map<GlobalPos, EngineState>&  engineStates() {
    static std::map<GlobalPos, EngineState> m;
    return m;
}
std::map<GlobalPos, int>& loggedLevels() {
    static std::map<GlobalPos, int> m;
    return m;
}
std::set<GlobalPos>& converting() {
    static std::set<GlobalPos> s;
    return s;
}
bool          gIndexDirty = false;
std::uint64_t gTick       = 0;

// ---- engine puffs (Create's BoilerData sound pools) ------------------------------------------------

/// One SoundPool(4, 2) per boiler side: puffs queued by its engines are merged over 2 ticks and at
/// most 4 of them play, so a big boiler doesn't sound like hundreds of engines.
struct PuffPool {
    std::vector<GlobalPos> queued;
    int                    ticks   = 0;
    int                    engines = 0;
};
std::map<std::pair<GlobalPos, int>, PuffPool>& puffPools() {
    static std::map<std::pair<GlobalPos, int>, PuffPool> m;
    return m;
}
std::mt19937& puffRng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

void queuePuff(GlobalPos const& controller, cs::Dir side, GlobalPos const& engine, int engines) {
    auto& pool   = puffPools()[{controller, static_cast<int>(side)}];
    pool.engines = engines;
    pool.queued.push_back(engine);
}

/// Create's BoilerData sound: candle extinguish at 3 / max(2, engines / 6), pitch 1.18 − rand · .25,
/// and the steam sample at a sixteenth of that volume (pitch .8), at the engine's block corner.
void playPuffAt(GlobalPos const& p, int engines) {
    std::uniform_real_distribution<float> d(0.f, 1.f);
    float volume = 3.f / static_cast<float>(std::max(2, engines / 6));
    float pitch  = 1.18f - d(puffRng()) * .25f;
    playSound(p.dim, p.x, p.y, p.z, "extinguish.candle", volume, pitch);
    playSound(p.dim, p.x, p.y, p.z, "levitate.steam.engine", volume / 16.f, .8f);
}

void playPuffs() {
    constexpr int kMaxConcurrent = 4, kMergeTicks = 2;
    for (auto it = puffPools().begin(); it != puffPools().end();) {
        auto& pool = it->second;
        if (pool.queued.empty()) {
            it = puffPools().erase(it);
            continue;
        }
        if (++pool.ticks < kMergeTicks) {
            ++it;
            continue;
        }
        pool.ticks = 0;
        auto& q    = pool.queued;
        if (static_cast<int>(q.size()) <= kMaxConcurrent) {
            for (auto const& p : q) playPuffAt(p, pool.engines);
        } else {
            for (int i = 0; i < kMaxConcurrent; ++i) {
                std::uniform_int_distribution<std::size_t> pick(0, q.size() - 1);
                auto                                       at = pick(puffRng());
                playPuffAt(q[at], pool.engines);
                q.erase(q.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
        q.clear();
        ++it;
    }
}

std::string at(GlobalPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

std::string number(double v) {
    // Create's LangNumberFormat: grouping, at most two decimals
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    auto        dot  = s.find('.');
    std::string head = s.substr(0, dot), tail = dot == std::string::npos ? "" : s.substr(dot);
    bool        neg  = !head.empty() && head[0] == '-';
    if (neg) head.erase(0, 1);
    std::string grouped;
    for (size_t i = 0; i < head.size(); ++i) {
        if (i > 0 && (head.size() - i) % 3 == 0) grouped += ',';
        grouped += head[i];
    }
    return (neg ? "-" : "") + grouped + tail;
}

char const* colourCode(cs::Colour c) {
    switch (c) {
    case cs::Colour::DarkGreen:
        return "§2";
    case cs::Colour::Green:
        return "§a";
    case cs::Colour::DarkRed:
        return "§4";
    case cs::Colour::DarkGray:
        return "§8";
    case cs::Colour::Gray:
        return "§7";
    case cs::Colour::Aqua:
        return "§b";
    case cs::Colour::Blue:
        return "§9";
    default:
        return "§6";
    }
}

template <class... Args>
std::string text(Player const* viewer, std::string_view key, std::string_view fallback, Args const&... args) {
    return viewer ? trFor(*viewer, key, fallback, args...) : tr(key, fallback, args...);
}

std::optional<int> shaftAxis(Block const& block) { return intState(block, "levitate:axis"); }

/// `levitate:size` holds 0 medium (the default), 1 small, 2 large.
cs::WhistleSize sizeOfState(int v) { return v == 1 ? cs::WhistleSize::Small : v == 2 ? cs::WhistleSize::Large : cs::WhistleSize::Medium; }
int stateOfSize(cs::WhistleSize s) { return s == cs::WhistleSize::Small ? 1 : s == cs::WhistleSize::Large ? 2 : 0; }

cs::WhistleSize whistleSize(Block const& block) { return sizeOfState(intState(block, "levitate:size").value_or(0)); }

std::optional<cs::ExtensionShape> extensionShapeAt(BlockSource& region, GlobalPos const& p) {
    if (idAt(region, p) != kExtension) return std::nullopt;
    auto const& b = region.getBlock(game::blockPos(p));
    return static_cast<cs::ExtensionShape>(std::clamp(intState(b, "levitate:shape").value_or(0), 0, 2));
}

void markIndexDirty() { gIndexDirty = true; }

// ---- powered shafts --------------------------------------------------------------------------------

void applyShaft(GlobalPos const& shaftPos, ShaftState& ss) {
    double engineCapacity = kinetics::stressCapacityOf(kEngine);
    bool   powered        = ss.engine.has_value();
    float  speed          = powered ? cs::poweredShaftSpeed(ss.direction, ss.efficiency, engineCapacity) : 0.f;
    double capacity       = powered && ss.efficiency > 0 ? cs::poweredShaftCapacity(ss.efficiency, engineCapacity) : 0.0;
    kinetics::setStressCapacity(shaftPos, capacity);
    kinetics::setGeneratedSpeed(shaftPos, speed);
    bool wasApplied = ss.applied;
    ss.applied      = true;
    if (wasApplied || speed != 0)
        log().info("steam: powered shaft at {} turns at {} RPM ({} su)", at(shaftPos), number(speed), number(capacity * std::abs(speed)));
}

/// Create's PoweredShaftBlockEntity.update.
void shaftUpdate(GlobalPos const& shaftPos, GlobalPos const& enginePos, int direction, float efficiency) {
    auto& ss       = shaftStates()[shaftPos];
    float prev     = ss.efficiency;
    int   prevDir  = ss.direction;
    ss.engine      = enginePos;
    ss.efficiency  = efficiency;
    if (std::abs(efficiency - prev) < 1e-6f && prevDir == direction && ss.applied) return;
    ss.direction = direction;
    applyShaft(shaftPos, ss);
}

/// Create's PoweredShaftBlockEntity.remove.
void shaftRemove(GlobalPos const& shaftPos, GlobalPos const& enginePos) {
    auto it = shaftStates().find(shaftPos);
    if (it == shaftStates().end() || !it->second.engine || !(*it->second.engine == enginePos)) return;
    it->second.engine.reset();
    it->second.efficiency = 0;
    it->second.direction  = 0;
    applyShaft(shaftPos, it->second);
}

/// Create's PoweredShaftBlock.stillValid: an engine two blocks away points at the shaft.
/// Unloaded neighbourhoods count as valid (the engine may be in a chunk that isn't loaded yet).
bool shaftStillValid(BlockSource& region, GlobalPos const& shaft, int axis) {
    for (auto d : kDirs) {
        if (static_cast<int>(cs::axisOf(d)) == axis) continue;
        GlobalPos engine = offset(shaft, d, 2);
        if (!loaded(region, engine)) return true;
        if (idAt(region, engine) != kEngine) continue;
        auto shape = engineShape(region.getBlock(game::blockPos(engine)));
        if (!(offset(engine, shape.facing, 2) == shaft)) continue;
        if (static_cast<int>(cs::axisOf(shape.facing)) != axis) return true;
    }
    return false;
}

void convertShaft(BlockSource& region, GlobalPos const& pos, bool toPowered, int axis) {
    auto const* block = game::blockWith(toPowered ? kPoweredShaft : kShaft, {{"levitate:axis", axis}});
    if (!block) return;
    converting().insert(pos);
    game::setBlock(region, game::blockPos(pos), *block);
    if (toPowered) {
        index().shafts.insert(pos);
        shaftStates()[pos] = ShaftState{};
    } else {
        index().shafts.erase(pos);
        shaftStates().erase(pos);
    }
    markIndexDirty();
    log().info("steam: shaft at {} {}", at(pos), toPowered ? "became a powered shaft" : "is a plain shaft again");
}

// ---- boilers ---------------------------------------------------------------------------------------

/// Create's BoilerData.evaluate: engines and whistles attached to any part of the structure.
void evaluate(GlobalPos const& ctrl, BoilerRuntime& rt) {
    rt.evaluated = true;
    rt.sinceEval = 0;
    auto* region = regionOf(ctrl.dim);
    auto  info   = tankAt(ctrl);
    if (!region || !info || !(info->controller == ctrl)) {
        rt.boiler.setAttachments(0, 0);
        return;
    }
    int           engines = 0, whistles = 0;
    std::set<int> pitches; // Create's whistlePitches (pipe_organ)
    for (int y = 0; y < info->height; ++y) {
        for (int x = 0; x < info->width; ++x) {
            for (int z = 0; z < info->width; ++z) {
                GlobalPos p{ctrl.dim, ctrl.x + x, ctrl.y + y, ctrl.z + z};
                if (!isTankId(idAt(*region, p))) continue;
                for (auto d : kDirs) {
                    GlobalPos n  = offset(p, d);
                    auto      id = idAt(*region, n);
                    if (id == kEngine) {
                        if (engineShape(region->getBlock(game::blockPos(n))).facing == d) ++engines;
                    } else if (id == kWhistle) {
                        if (cs::opposite(whistleAttached(region->getBlock(game::blockPos(n)))) == d) {
                            ++whistles;
                            pitches.insert(whistlePitchAt(n) + 100 * static_cast<int>(whistleSize(region->getBlock(game::blockPos(n)))));
                        }
                    }
                }
            }
        }
    }
    bool wasActive = rt.boiler.isActive();
    rt.boiler.setAttachments(engines, whistles);
    if (pitches.size() >= 12) advancements::awardOwner(ctrl, "pipe_organ");
    if (wasActive != rt.boiler.isActive()) {
        if (rt.boiler.isActive()) setWindows(ctrl, false); // Create: a boiler closes its windows
        log().info("steam: tank at {} {} a boiler ({} engines, {} whistles)", at(ctrl), rt.boiler.isActive() ? "is now" : "is no longer", engines,
                   whistles);
        markTankForSync(ctrl);
    }
}

std::vector<int> heatScores(BlockSource& region, TankInfo const& info) {
    std::vector<int> scores;
    for (int x = 0; x < info.width; ++x) {
        for (int z = 0; z < info.width; ++z) {
            BlockPos below{info.controller.x + x, info.controller.y - 1, info.controller.z + z};
            if (!region.hasChunksAt(below, 0, false)) continue;
            scores.push_back(cs::heatScore(common::heat::level(region, below)));
        }
    }
    return scores;
}

/// Create's BoilerData.updateOcclusion: a side's gauge (in the middle of the side, on the bottom
/// layer) is hidden when something with a collision box stands right in front of it. Bits in
/// Create's 2D order (south, west, north, east).
int gaugeOcclusion(BlockSource& region, TankInfo const& info) {
    static constexpr int dx[4]{0, -1, 0, 1}, dz[4]{1, 0, -1, 0};
    int mask = 0, w = info.width;
    // the middle of a side: one block for odd widths, the two middle ones for even widths
    std::vector<int> along = w % 2 ? std::vector<int>{w / 2} : std::vector<int>{w / 2 - 1, w / 2};
    for (int i = 0; i < 4; ++i) {
        for (int t : along) {
            int x = dx[i] == 0 ? t : dx[i] > 0 ? w : -1;
            int z = dz[i] == 0 ? t : dz[i] > 0 ? w : -1;
            BlockPos p{info.controller.x + x, info.controller.y, info.controller.z + z};
            if (!region.hasChunksAt(p, 0, false)) continue;
            if (!game::replaceable(region, p)) mask |= 1 << i;
        }
    }
    return mask;
}

GlobalPos attachedTankOfEngine(GlobalPos const& engine, Block const& block) {
    return offset(engine, cs::opposite(engineShape(block).facing));
}

std::vector<std::string> boilerLines(BoilerRuntime const& rt, int size, Player const* viewer) {
    std::vector<std::string> out;
    auto const&              b = rt.boiler;
    auto                     d = b.display(size);
    std::string              levelText;
    if (d.passive) levelText = text(viewer, "create.boiler.passive", "Passive");
    else if (d.level == 0) levelText = text(viewer, "create.boiler.idle", "Idle");
    else if (d.level == cs::kMaxBoilerLevel) levelText = text(viewer, "create.boiler.max_lvl", "Max");
    else levelText = text(viewer, "create.boiler.lvl", "Lvl {0}", d.level);
    out.push_back(text(viewer, "create.boiler.status", "Boiler Status:  {0}", std::string("§a") + levelText + "§r"));
    auto line = [&](char const* label, char const* fallback, char const* dots, char const* dotsFallback, int level) {
        std::string s = "  §7" + text(viewer, std::string("create.boiler.") + label, fallback) + "§8"
                      + text(viewer, std::string("create.boiler.") + dots, dotsFallback);
        for (auto const& [c, n] : cs::boilerBars(d, level)) s += colourCode(c) + std::string(static_cast<size_t>(n), '|');
        out.push_back(s);
    };
    line("size", "Size", "size_dots", "....... ", d.maxHeatForSize);
    line("water", "Water", "water_dots", "... ", d.maxHeatForWater);
    line("heat", "Heat", "heat_dots", "...... ", d.heat);
    if (b.attachedEngines == 0) return out;
    out.emplace_back("");
    auto mb = text(viewer, "create.generic.unit.millibuckets", "mB");
    if (b.attachedEngines > 0 && d.maxHeatForSize > 0 && d.maxHeatForWater == 0 && d.heat > 0) {
        out.push_back("§7" + text(viewer, "create.boiler.water_input_rate", "Water input rate"));
        out.push_back("  §9" + number(b.waterSupply) + mb + "§7 / §8"
                      + text(viewer, "create.boiler.per_tick", "{0} per Tick", std::to_string(cs::kWaterSupplyPerLevel) + mb));
        return out;
    }
    double su = cs::boilerCapacityShown(b, size, kinetics::stressCapacityOf(kEngine));
    out.push_back("§7" + text(viewer, "create.tooltip.capacityProvided", "Kinetic Stress Capacity:"));
    std::string via = b.attachedEngines == 1 ? text(viewer, "create.boiler.via_one_engine", "via 1 engine")
                                             : text(viewer, "create.boiler.via_engines", "via {0} engines", b.attachedEngines);
    out.push_back("  §b" + number(su) + text(viewer, "create.generic.unit.stress", "su") + " §8" + via);
    return out;
}

std::string fluidName(std::string const& fluid, Player const* viewer) {
    return common::fluidtypes::displayName(fluid, viewer ? localeOf(*viewer) : std::string{});
}

} // namespace

// ---- shared state ----------------------------------------------------------------------------------

std::map<GlobalPos, BoilerRuntime>& boilers() {
    static std::map<GlobalPos, BoilerRuntime> m;
    return m;
}

Index& index() {
    static Index i;
    return i;
}

std::uint64_t currentTick() { return gTick; }

BoilerRuntime* boilerOf(GlobalPos const& controller) {
    auto& rt = boilers()[controller];
    if (!rt.evaluated) evaluate(controller, rt);
    return &rt;
}

void markBoilerDirty(GlobalPos const& controller) {
    if (auto it = boilers().find(controller); it != boilers().end()) it->second.evaluated = false;
}

void feedBoiler(GlobalPos const& controller, int mb) {
    if (auto* rt = boilerOf(controller)) rt->boiler.addSupply(mb);
}

bool boilerActive(GlobalPos const& controller) {
    auto* rt = boilerOf(controller);
    return rt && rt->boiler.isActive();
}

EngineShape engineShape(Block const& block) {
    EngineShape s;
    auto        face = strState(block, "minecraft:block_face");
    s.facing         = dirFromName(face, cs::Dir::Up);
    s.horizontal     = dirFromName(strState(block, "minecraft:cardinal_direction"), cs::Dir::South);
    if (cs::axisOf(s.facing) != cs::Axis::Y) s.horizontal = s.facing;
    return s;
}

cs::Dir whistleAttached(Block const& block) {
    auto face = dirFromName(strState(block, "minecraft:block_face"), cs::Dir::Up);
    return cs::axisOf(face) == cs::Axis::Y ? cs::Dir::Down : cs::opposite(face);
}

bool engineCounterClockwise(GlobalPos const& engine) {
    auto const* doc = blockdata::get(engine);
    if (!doc || !doc->contains(kDataKey)) return false;
    return (*doc)[kDataKey].value("ccw", false);
}

void setEngineCounterClockwise(GlobalPos const& engine, bool ccw) {
    blockdata::edit(engine)[kDataKey]["ccw"] = ccw;
}

// ---- index -----------------------------------------------------------------------------------------

void saveIndex() {
    if (!game::db::available()) return;
    auto list = [](std::set<GlobalPos> const& s) {
        Json a = Json::array();
        for (auto const& p : s) a.push_back({p.dim, p.x, p.y, p.z});
        return a;
    };
    game::db::save(kDbIndex, Json{{"engines", list(index().engines)}, {"whistles", list(index().whistles)}, {"shafts", list(index().shafts)}});
    gIndexDirty = false;
}

void loadIndex() {
    index() = Index{};
    auto j  = game::db::load(kDbIndex);
    if (!j || !j->is_object()) return;
    auto read = [&](char const* key, std::set<GlobalPos>& out) {
        if (!j->contains(key) || !(*j)[key].is_array()) return;
        for (auto const& e : (*j)[key])
            if (e.is_array() && e.size() == 4) out.insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    };
    read("engines", index().engines);
    read("whistles", index().whistles);
    read("shafts", index().shafts);
}

void resetRuntime() {
    boilers().clear();
    shaftStates().clear();
    whistleStates().clear();
    resetWhistleTones();
    engineStates().clear();
    puffPools().clear();
    loggedLevels().clear();
    converting().clear();
    index() = Index{};
    gIndexDirty = false;
}

// ---- block events ----------------------------------------------------------------------------------

void onEngineAdded(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || idAt(*region, pos) != kEngine) return;
    auto const& block = region->getBlock(game::blockPos(pos));
    GlobalPos   tank  = attachedTankOfEngine(pos, block);
    if (!isTankId(idAt(*region, tank))) {
        // Create's SteamEngineBlock.canSurvive: only on fluid tanks
        log().info("steam: steam engine at {} has no tank to stand on and breaks", at(pos));
        popBlock(*region, pos);
        return;
    }
    index().engines.insert(pos);
    markIndexDirty();
    if (auto info = tankAt(tank)) markBoilerDirty(info->controller);
    log().info("steam: steam engine placed at {} facing {}", at(pos), dirName(engineShape(block).facing));
}

void onEngineRemoved(GlobalPos const& pos) {
    if (index().engines.erase(pos)) markIndexDirty();
    engineStates().erase(pos);
    // its shaft stops (the shaft may lie in either direction: the block is gone)
    for (auto& [shaftPos, ss] : shaftStates())
        if (ss.engine && *ss.engine == pos) shaftRemove(shaftPos, pos);
    for (auto d : kDirs)
        if (auto info = tankAt(offset(pos, d))) markBoilerDirty(info->controller);
    log().info("steam: steam engine at {} removed", at(pos));
}

void onWhistleAdded(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || idAt(*region, pos) != kWhistle) return;
    auto const& block = region->getBlock(game::blockPos(pos));
    GlobalPos   tank  = offset(pos, whistleAttached(block));
    if (!isTankId(idAt(*region, tank))) {
        log().info("steam: steam whistle at {} has no tank to hang on and breaks", at(pos));
        popBlock(*region, pos);
        return;
    }
    index().whistles.insert(pos);
    markIndexDirty();
    setStates(*region, pos, {{"levitate:powered", redstonePowered(*region, pos) ? 1 : 0}});
    if (auto info = tankAt(tank)) markBoilerDirty(info->controller);
    log().info("steam: steam whistle placed at {}", at(pos));
}

void onWhistleRemoved(GlobalPos const& pos) {
    if (index().whistles.erase(pos)) markIndexDirty();
    whistleStates().erase(pos);
    whistleQuiet(pos);
    // extensions above can't stand without it (they drop nothing, as Create's)
    if (auto* region = regionOf(pos.dim)) {
        for (int i = 1; i <= 32; ++i) {
            GlobalPos p = offset(pos, cs::Dir::Up, i);
            if (idAt(*region, p) != kExtension) break;
            removeBlock(*region, p);
        }
    }
    for (auto d : kDirs)
        if (auto info = tankAt(offset(pos, d))) markBoilerDirty(info->controller);
    log().info("steam: steam whistle at {} removed", at(pos));
}

void onExtensionChanged(GlobalPos const& pos, bool added) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    GlobalPos below = offset(pos, cs::Dir::Down);
    auto      belowId = idAt(*region, below);
    if (added) {
        // Create's WhistleExtenderBlock.canSurvive: on a whistle or another extension
        if (belowId != kWhistle && belowId != kExtension) {
            removeBlock(*region, pos);
            return;
        }
        auto size = intState(region->getBlock(game::blockPos(below)), "levitate:size").value_or(0);
        auto shape = cs::connectedShape(extensionShapeAt(*region, pos).value_or(cs::ExtensionShape::Single), idAt(*region, offset(pos, cs::Dir::Up)) == kExtension);
        setStates(*region, pos, {{"levitate:size", size}, {"levitate:shape", static_cast<int>(shape)}});
    } else {
        // everything above falls apart
        for (int i = 1; i <= 32; ++i) {
            GlobalPos p = offset(pos, cs::Dir::Up, i);
            if (idAt(*region, p) != kExtension) break;
            removeBlock(*region, p);
        }
    }
    if (belowId == kExtension) {
        auto shape = extensionShapeAt(*region, below);
        if (shape && *shape != cs::ExtensionShape::Single) {
            auto next = cs::connectedShape(*shape, added);
            setStates(*region, below, {{"levitate:shape", static_cast<int>(next)}});
        }
    }
}

void onPoweredShaftRemoved(GlobalPos const& pos) {
    if (converting().erase(pos)) return;
    if (index().shafts.erase(pos)) markIndexDirty();
    shaftStates().erase(pos);
}

void onPoweredShaftAdded(GlobalPos const& pos) {
    converting().erase(pos);
    if (index().shafts.insert(pos).second) markIndexDirty();
    shaftStates().try_emplace(pos);
}

bool whistleSounding(GlobalPos const& pos) {
    auto it = whistleStates().find(pos);
    return it != whistleStates().end() && it->second.sounding;
}

void dropAttachmentsOf(GlobalPos const& removedTank) {
    auto* region = regionOf(removedTank.dim);
    if (!region) return;
    for (auto d : kDirs) {
        GlobalPos n  = offset(removedTank, d);
        auto      id = idAt(*region, n);
        if (id == kEngine) {
            if (engineShape(region->getBlock(game::blockPos(n))).facing == d) {
                log().info("steam: steam engine at {} lost its tank and breaks", at(n));
                popBlock(*region, n);
            }
        } else if (id == kWhistle) {
            if (cs::opposite(whistleAttached(region->getBlock(game::blockPos(n)))) == d) {
                log().info("steam: steam whistle at {} lost its tank and breaks", at(n));
                popBlock(*region, n);
            }
        }
    }
}

// ---- whistles ----------------------------------------------------------------------------------------

int whistlePitchAt(GlobalPos const& whistle) {
    auto* region = regionOf(whistle.dim);
    if (!region) return 0;
    return cs::whistlePitch([&](int i) { return extensionShapeAt(*region, offset(whistle, cs::Dir::Up, i)); });
}

bool useOnWhistle(Player& player, GlobalPos const& pos, ItemStack const& held, bool sneaking, double hitY) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    auto id     = idAt(*region, pos);
    auto heldId = held.isNull() ? std::string{} : common::items::idOf(held);
    // the whistle at the bottom of the column
    GlobalPos base = pos;
    if (id == kExtension) {
        for (int i = 0; i < 64 && idAt(*region, base) == kExtension; ++i) base = offset(base, cs::Dir::Down);
        if (idAt(*region, base) != kWhistle) return false;
    } else if (id != kWhistle) {
        return false;
    }
    auto const& baseBlock = region->getBlock(game::blockPos(base));
    if (heldId == kWhistle) {
        // Create's WhistleBlock.incrementSize (the held whistle is not used up)
        auto action = cs::incrementSize(
            [&](int i) { return extensionShapeAt(*region, offset(base, cs::Dir::Up, i)); },
            [&](int i) { return game::replaceable(*region, game::blockPos(offset(base, cs::Dir::Up, i))); }
        );
        GlobalPos target = offset(base, cs::Dir::Up, action.offset);
        if (action.kind == cs::IncrementAction::Widen) {
            setStates(*region, target, {{"levitate:shape", static_cast<int>(cs::ExtensionShape::Double)}});
        } else if (action.kind == cs::IncrementAction::Place) {
            auto size = intState(baseBlock, "levitate:size").value_or(0);
            if (auto const* b = game::blockWith(kExtension, {{"levitate:size", size}, {"levitate:shape", 0}})) game::setBlock(*region, game::blockPos(target), *b);
        }
        if (action.kind != cs::IncrementAction::None) {
            // Create: the note block xylophone at a quarter volume and the whistle's (copper) hit sound
            playSound(pos.dim, target.x + .5, target.y + .5, target.z + .5, "note.xylophone", .25f, action.soundPitch);
            playSound(pos.dim, target.x + .5, target.y + .5, target.z + .5, "hit.copper", 1.f, action.soundPitch);
            log().info("steam: {} changed the whistle at {}: pitch {}", player.getRealName(), at(base), whistlePitchAt(base));
        }
        return true;
    }
    if (heldId == kWrench) {
        if (sneaking && id == kExtension) {
            // Create's WhistleExtenderBlock.onSneakWrenched: the upper half of a full extension halves
            // it (the extensions above lose their support); the lower half or a half extension is
            // the wrench's own dismantling (U8)
            auto shape = extensionShapeAt(*region, pos).value_or(cs::ExtensionShape::Single);
            if (hitY < .5 || shape == cs::ExtensionShape::Single) return false;
            for (int i = 1; i <= 32; ++i) {
                GlobalPos p = offset(pos, cs::Dir::Up, i);
                if (idAt(*region, p) != kExtension) break;
                removeBlock(*region, p);
            }
            setStates(*region, pos, {{"levitate:shape", static_cast<int>(cs::ExtensionShape::Single)}});
            goggles::wrench::playRemoveSound(pos);
            log().info("steam: {} changed the whistle at {}: pitch {}", player.getRealName(), at(base), whistlePitchAt(base));
            return true;
        }
        if (sneaking) return false; // dismantling is the wrench's own (U8)
        // Create's WhistleBlock.onWrenched: the next size, for the whole column
        auto next = static_cast<cs::WhistleSize>((static_cast<int>(whistleSize(baseBlock)) + 1) % 3);
        int  size = stateOfSize(next);
        setStates(*region, base, {{"levitate:size", size}});
        for (int i = 1; i <= 32; ++i) {
            GlobalPos p = offset(base, cs::Dir::Up, i);
            if (idAt(*region, p) != kExtension) break;
            setStates(*region, p, {{"levitate:size", size}});
        }
        playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "block.itemframe.rotate_item", 1.f, 1.f);
        static constexpr char const* names[]{"small", "medium", "large"};
        log().info("steam: whistle at {} is now {}", at(base), names[static_cast<int>(next)]);
        return true;
    }
    return false;
}

// ---- tick ----------------------------------------------------------------------------------------------

void tickSteam() {
    ++gTick;

    // powered shafts: count down, revert when no engine points at them any more
    for (auto pos : std::vector<GlobalPos>(index().shafts.begin(), index().shafts.end())) {
        auto* region = regionOf(pos.dim);
        if (!region || !loaded(*region, pos)) continue;
        if (idAt(*region, pos) != kPoweredShaft) {
            index().shafts.erase(pos);
            shaftStates().erase(pos);
            markIndexDirty();
            continue;
        }
        auto& ss = shaftStates()[pos];
        if (ss.initialTicks > 0) --ss.initialTicks;
        int axis = shaftAxis(region->getBlock(game::blockPos(pos))).value_or(0);
        if (!shaftStillValid(*region, pos, axis) && game::blockWith(kShaft)) convertShaft(*region, pos, false, axis);
    }

    // boilers in use: heat and water supply
    std::set<GlobalPos> active;
    auto addActive = [&](GlobalPos const& tankPos) {
        if (auto info = tankAt(tankPos)) active.insert(info->controller);
    };
    for (auto const& e : index().engines) {
        auto* region = regionOf(e.dim);
        if (!region || !loaded(*region, e) || idAt(*region, e) != kEngine) continue;
        addActive(attachedTankOfEngine(e, region->getBlock(game::blockPos(e))));
    }
    for (auto const& w : index().whistles) {
        auto* region = regionOf(w.dim);
        if (!region || !loaded(*region, w) || idAt(*region, w) != kWhistle) continue;
        addActive(offset(w, whistleAttached(region->getBlock(game::blockPos(w)))));
    }
    for (auto const& ctrl : active) {
        auto* region = regionOf(ctrl.dim);
        auto  info   = tankAt(ctrl);
        if (!region || !info) continue;
        auto* rt = boilerOf(ctrl);
        if (++rt->sinceEval >= 100) evaluate(ctrl, *rt); // catches attachments changed without an event
        if (!rt->boiler.isActive()) continue;
        if (rt->boiler.updateTemperature(heatScores(*region, *info))) markTankForSync(ctrl);
        bool sampled = rt->boiler.tick(static_cast<int>(std::min<std::int64_t>(info->capacity, INT32_MAX)), info->creative);
        if (sampled) markTankForSync(ctrl);
        int size  = info->width * info->width * info->height;
        if (sampled && rt->boiler.actualHeat(size) == cs::kMaxBoilerLevel) advancements::awardOwner(ctrl, "steam_engine_maxed");
        int level = rt->boiler.display(size).passive ? -1 : rt->boiler.actualHeat(size);
        // the gauges (Create's BoilerData.read: gauge chases this target; updateOcclusion)
        int   forSize = rt->boiler.maxHeatForSize(size);
        float gauge   = level < 0 ? 1.f / 8.f : forSize == 0 ? 0.f : static_cast<float>(level) / static_cast<float>(forSize);
        int   occluded = gaugeOcclusion(*region, *info);
        if (gauge != rt->gauge || occluded != rt->occluded) {
            rt->gauge    = gauge;
            rt->occluded = occluded;
            markTankForSync(ctrl);
        }
        auto [last, fresh] = loggedLevels().try_emplace(ctrl, level);
        if (fresh || last->second != level) {
            last->second = level;
            log().info("steam: boiler at {} level {} (size {}, water {} mB/t, heat {}{})", at(ctrl), level < 0 ? std::string("passive") : std::to_string(level),
                       rt->boiler.maxHeatForSize(size), number(rt->boiler.waterSupply), rt->boiler.activeHeat, rt->boiler.passiveHeat ? ", passive" : "");
        }
    }
    for (auto it = loggedLevels().begin(); it != loggedLevels().end();) {
        if (!active.contains(it->first)) it = loggedLevels().erase(it);
        else ++it;
    }

    // steam engines (Create's SteamEngineBlockEntity.tick)
    for (auto pos : std::vector<GlobalPos>(index().engines.begin(), index().engines.end())) {
        auto* region = regionOf(pos.dim);
        if (!region || !loaded(*region, pos)) continue;
        if (idAt(*region, pos) != kEngine) {
            onEngineRemoved(pos);
            continue;
        }
        auto const& block    = region->getBlock(game::blockPos(pos));
        auto        shape    = engineShape(block);
        GlobalPos   tankPos  = offset(pos, cs::opposite(shape.facing));
        GlobalPos   shaftPos = offset(pos, shape.facing, 2);
        // a plain shaft in front becomes the engine's powered shaft (Create's pickCorrectShaftType)
        auto shaftId = idAt(*region, shaftPos);
        if (shaftId == kShaft) {
            int axis = shaftAxis(region->getBlock(game::blockPos(shaftPos))).value_or(0);
            if (axis != static_cast<int>(cs::axisOf(shape.facing)) && game::blockWith(kPoweredShaft)) {
                convertShaft(*region, shaftPos, true, axis);
                shaftId = kPoweredShaft;
            }
        }
        auto tank       = tankAt(tankPos);
        bool shaftValid = false;
        int  axis       = 0;
        if (shaftId == kPoweredShaft) {
            axis       = shaftAxis(region->getBlock(game::blockPos(shaftPos))).value_or(0);
            auto& ss   = shaftStates()[shaftPos];
            shaftValid = axis != static_cast<int>(cs::axisOf(shape.facing)) && ss.initialTicks == 0 && (!ss.engine || *ss.engine == pos);
        }
        // Create's SteamEngineBlockEntity.isValid: only a regular fluid tank drives the engine (an
        // engine on a creative fluid tank stands still)
        if (!tank || !shaftValid || tank->creative) {
            auto it = shaftStates().find(shaftPos);
            if (shaftId == kPoweredShaft && it != shaftStates().end() && it->second.engine && *it->second.engine == pos && it->second.efficiency != 0
                && loaded(*region, tankPos))
                shaftUpdate(shaftPos, pos, 0, 0);
            continue;
        }
        auto* rt         = boilerOf(tank->controller);
        int   size       = tank->width * tank->width * tank->height;
        float efficiency = std::clamp(rt->boiler.engineEfficiency(size), 0.f, 1.f);
        bool  ccw        = engineCounterClockwise(pos);
        int   conveyed   = cs::conveyedDirection(efficiency, static_cast<cs::Axis>(axis),
                                                 cs::axisOf(shape.facing) == cs::Axis::Y ? shape.horizontal : shape.facing, ccw);
        if (auto node = kinetics::nodeAt(shaftPos)) {
            float shaftSpeed = node->theoreticalSpeed;
            if (node->source && shaftSpeed != 0 && conveyed != 0 && (shaftSpeed > 0) != (conveyed > 0)) {
                setEngineCounterClockwise(pos, !ccw);
                conveyed = -conveyed;
            }
        }
        shaftUpdate(shaftPos, pos, conveyed, efficiency);

        // the engine's puffs: twice per turn of the shaft (Create's steam sound pool)
        float speed = kinetics::speedAt(shaftPos);
        auto& es    = engineStates()[pos];
        if (speed != 0 && efficiency > 0 && gTick >= es.nextChuff) {
            es.nextChuff = gTick + static_cast<std::uint64_t>(std::max(4.f, 600.f / std::abs(speed)));
            queuePuff(tank->controller, shape.facing, pos, rt->boiler.attachedEngines);
            // a steam jet out of the cylinder's front, along the facing and a little up
            auto   f  = offset(pos, shape.facing);
            double nx = f.x - pos.x, ny = f.y - pos.y, nz = f.z - pos.z;
            steamJet(pos.dim, pos.x + .5 + nx * .55, pos.y + .5 + ny * .55, pos.z + .5 + nz * .55, nx * .06, ny * .06 + .02, nz * .06);
        }
    }

    // steam whistles
    for (auto pos : std::vector<GlobalPos>(index().whistles.begin(), index().whistles.end())) {
        auto* region = regionOf(pos.dim);
        if (!region || !loaded(*region, pos)) {
            // an unloaded whistle stops (Create: its block entity no longer keeps the sound alive)
            if (auto it = whistleStates().find(pos); it != whistleStates().end() && it->second.sounding) {
                it->second.sounding  = false;
                it->second.lastPitch = -1;
                whistleQuiet(pos);
            }
            continue;
        }
        if (idAt(*region, pos) != kWhistle) {
            onWhistleRemoved(pos);
            continue;
        }
        auto const& block = region->getBlock(game::blockPos(pos));
        bool        powered = intState(block, "levitate:powered").value_or(0) != 0;
        if (gTick % 2 == 0) {
            bool now = redstonePowered(*region, pos);
            if (now != powered) {
                setStates(*region, pos, {{"levitate:powered", now ? 1 : 0}});
                powered = now;
            }
        }
        bool sounding = false;
        if (powered) {
            auto tank = tankAt(offset(pos, whistleAttached(block)));
            sounding  = tank && boilerOf(tank->controller)->boiler.hasSteam();
        }
        auto& ws    = whistleStates()[pos];
        auto  size  = whistleSize(block);
        int   pitch = sounding ? whistlePitchAt(pos) : 0;
        float f     = cs::whistleSoundPitch(pitch);
        static constexpr char const* sizeNames[]{"small", "medium", "large"};
        static constexpr char const* toneSounds[]{"levitate.steam.whistle_high", "levitate.steam.whistle_medium", "levitate.steam.whistle_low"};
        bool started = sounding && (!ws.sounding || ws.lastPitch != cs::pitchId(pitch, size));
        if (started) {
            if (!ws.sounding) whistleChiff(pos,size == cs::WhistleSize::Small ? f + .75f : f);
            log().info("steam: whistle at {} sounds ({} whistle, pitch {})", at(pos), sizeNames[static_cast<int>(size)], pitch);
            ws.lastPitch = cs::pitchId(pitch, size);
        }
        if (!sounding && ws.sounding) {
            log().info("steam: whistle at {} is quiet", at(pos));
            whistleQuiet(pos);
        }
        bool newSound = sounding && !ws.sounding;
        ws.sounding   = sounding;
        if (!sounding) {
            ws.lastPitch = -1;
            continue;
        }
        // one looping instance per whistle; its pitch follows the column (Create's tickAudio)
        whistleTone(pos, toneSounds[static_cast<int>(size)], f);
        if (gTick % 8 == 0 || newSound) {
            // steam out of the mouth, away from the tank (Create's SteamJetParticle): from the
            // mouth's lip, up and out of it
            // the mouth faces away from the tank (wall) or back towards whoever placed it (floor)
            auto   facing = cs::opposite(cs::axisOf(whistleAttached(block)) == cs::Axis::Y
                                             ? dirFromName(strState(block, "minecraft:cardinal_direction"), cs::Dir::North)
                                             : whistleAttached(block));
            auto   out = offset(pos, facing);
            double nx = out.x - pos.x, nz = out.z - pos.z;
            double reach = .45 * .75 + static_cast<int>(size) / 16.0;
            double mx = nx * 1.5, my = 1.0, mz = nz * 1.5, len = std::sqrt(mx * mx + my * my + mz * mz);
            steamJet(pos.dim, pos.x + .5 + nx * reach, pos.y + .5 + .45 - .4, pos.z + .5 + nz * reach, mx / len * .1, my / len * .1, mz / len * .1);
        }
    }
    tickWhistleTones();
    playPuffs();

    if (gIndexDirty) saveIndex();
}

// ---- value box -----------------------------------------------------------------------------------------

void registerEngineValueBox() {
    namespace vb = common::valuebox;
    vb::BoxSpec box;
    box.id            = "direction";
    box.label         = "create.contraptions.windmill.rotation_direction";
    box.labelFallback = "Rotation Direction";
    box.board         = [](Block const&) { return core::common::optionBoard(2, tr("create.contraptions.windmill.rotation_direction", "Rotation Direction")); };
    box.centre        = [](Block const& block, int face) -> std::optional<core::common::Vec> {
        auto shape = engineShape(block);
        if (static_cast<int>(cs::axisOf(static_cast<cs::Dir>(face))) == static_cast<int>(cs::axisOf(shape.facing))) return std::nullopt;
        return core::common::faceLocalPoint(face, 8, 8, 0.5);
    };
    box.get = [](GlobalPos const& p) { return core::common::ValueSettings{0, engineCounterClockwise(p) ? 1 : 0}; };
    box.set = [](GlobalPos const& p, core::common::ValueSettings s, Player& editor) {
        setEngineCounterClockwise(p, s.value == 1);
        log().info("steam: {} set the steam engine at {} to {}", editor.getRealName(), at(p), s.value == 1 ? "counter-clockwise" : "clockwise");
    };
    box.format = [](Block const&, core::common::ValueSettings s) {
        return s.value == 1 ? tr("create.generic.counter_clockwise", "Counter-Clockwise") : tr("create.generic.clockwise", "Clockwise");
    };
    vb::registerBox(kEngine, std::move(box));
}

// ---- goggles / command ----------------------------------------------------------------------------------

std::vector<std::string> describe(GlobalPos const& pos, Player const* viewer) {
    std::vector<std::string> out;
    auto*                    region = regionOf(pos.dim);
    if (!region) return out;
    auto      id      = idAt(*region, pos);
    GlobalPos tankPos = pos;
    if (id == kEngine) tankPos = attachedTankOfEngine(pos, region->getBlock(game::blockPos(pos)));
    else if (id == kWhistle) tankPos = offset(pos, whistleAttached(region->getBlock(game::blockPos(pos))));
    auto info = tankAt(tankPos);
    if (!info) return out;
    auto* rt = boilerOf(info->controller);
    if (rt->boiler.isActive()) return boilerLines(*rt, info->width * info->width * info->height, viewer);
    // Create's IHaveGoggleInformation.containedFluidTooltip
    auto mb = text(viewer, "create.generic.unit.millibuckets", "mB");
    out.push_back(text(viewer, "create.gui.goggles.fluid_container", "Fluid Container Info:"));
    if (!info->fluid.empty() && info->amount > 0) {
        out.push_back("  §7" + fluidName(info->fluid, viewer));
        out.push_back("  §6" + number(static_cast<double>(info->amount)) + mb + "§7 / §8" + number(static_cast<double>(info->capacity)) + mb);
    } else {
        out.push_back("  §7" + text(viewer, "create.gui.goggles.fluid_container.capacity", "Capacity: ") + "§6"
                      + number(static_cast<double>(info->capacity)) + mb);
    }
    return out;
}

} // namespace lv::feature::steam
