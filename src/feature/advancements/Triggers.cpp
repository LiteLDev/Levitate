// Advancement triggers that this unit fires by observing the world and the shared contracts
// (U45): the root, inventory criteria, block placements (owners, Create's AdvancementBehaviour),
// kinetic speeds, bearings, contraptions, glue, redstone and clicks. Triggers inside other units'
// processing (presses, mixers, spouts, ...) need a call to `advancements::awardOwner` there
// (SPEC §3.6.45 lists them).

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/advancements/Advancements.h"
#include "feature/advancements/Advancements.h"
#include "feature/advancements/Internal.h"
#include "feature/bearings/Bearings.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pipes/Pipes.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::advancements {

namespace ca    = core::advancements;
namespace ia    = common::interactions;
namespace items = common::items;
using SharedTypes::Legacy::ArmorSlot;

namespace {

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    return dimension ? &dimension->getBlockSourceFromMainChunkSource() : nullptr;
}

GlobalPos offset(GlobalPos p, int face) {
    static constexpr std::array<std::array<int, 3>, 6> d = {{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    if (face < 0 || face > 5) return p;
    auto const& o = d[static_cast<std::size_t>(face)];
    return {p.dim, p.x + o[0], p.y + o[1], p.z + o[2]};
}

std::uint64_t gTick = 0;
std::vector<kinetics::ListenerId>& listeners() {
    static std::vector<kinetics::ListenerId> v;
    return v;
}
std::vector<ia::HandlerId>& handlers() {
    static std::vector<ia::HandlerId> v;
    return v;
}

// ---- clicks: placements, casings, belts, pipes, signals ---------------------------------------------------------

struct PendingClick {
    std::string   uuid;
    std::string   itemId;
    GlobalPos     clicked;
    int           face = 0;
    std::string   before[2]; ///< clicked, clicked + face
    std::uint64_t due = 0;
};
std::vector<PendingClick>& pending() {
    static std::vector<PendingClick> v;
    return v;
}

Player* byUuid(std::string const& uuid);

bool isCogwheel(std::string const& id) { return id == "levitate:cogwheel" || id == "levitate:large_cogwheel"; }

/// Create's CogWheelBlock#setPlacedBy: a small and a large cogwheel meshing diagonally.
void checkCogs(Player& player, GlobalPos const& pos, std::string const& id) {
    auto* r = regionOf(pos.dim);
    if (!r) return;
    int  axis    = game::intState(r->getBlock(game::blockPos(pos)), "levitate:axis").value_or(1);
    bool isLarge = id == "levitate:large_cogwheel";
    for (int a1 = 0; a1 < 3; ++a1) {
        if (a1 == axis) continue;
        for (int a2 = 0; a2 < 3; ++a2) {
            if (a2 == a1 || a2 == axis) continue;
            for (int o1 : {-1, 1})
                for (int o2 : {-1, 1}) {
                    int d[3] = {0, 0, 0};
                    d[a1] += o1;
                    d[a2] += o2;
                    BlockPos    at{pos.x + d[0], pos.y + d[1], pos.z + d[2]};
                    auto const& other = r->getBlock(at);
                    std::string oid   = game::blockId(other);
                    if (!isCogwheel(oid)) continue;
                    if (game::intState(other, "levitate:axis").value_or(1) != axis) continue;
                    if ((oid == "levitate:large_cogwheel") == isLarge) continue;
                    award(player, "shifting_gears");
                    return;
                }
        }
    }
}

void evaluateBlock(GlobalPos const& pos, std::string const& id);

void resolveClick(PendingClick const& c) {
    auto* player = byUuid(c.uuid);
    if (!player) return;
    GlobalPos const at[2] = {c.clicked, offset(c.clicked, c.face)};
    for (int i = 0; i < 2; ++i) {
        std::string now = blockIdAt(at[i]);
        if (now.empty() || now == c.before[i]) continue;
        // a new block: Create's setPlacedBy (owner of the block entity's advancements)
        if (!blockAdvancements(now).empty()) {
            setOwner(at[i], *player);
            evaluateBlock(at[i], now);
        }
        if (isCogwheel(now)) checkCogs(*player, at[i], now);
        if (i == 1 && now == "levitate:track_signal" && c.itemId == "levitate:track_signal") award(*player, "track_signal");
        if (i == 0) {
            // Create's ManualApplicationRecipe: an item applied to a block in the world
            if (now == "levitate:andesite_casing") award(*player, "andesite_casing");
            else if (now == "levitate:copper_casing") award(*player, "copper_casing");
            else if (now == "levitate:brass_casing") award(*player, "brass_casing");
            else if (now == "levitate:railway_casing") award(*player, "train_casing_00");
            // Create's BeltConnectorItem: the second shaft connected
            if (now == "levitate:belt" && c.itemId == "levitate:belt_connector") award(*player, "belt");
            // Create's FluidPipeBlock#onWrenched: a pipe with flow turned into a glass pipe
            if (now == "levitate:glass_fluid_pipe" && c.before[0] == "levitate:fluid_pipe" && c.itemId == ia::kWrenchId
                && !pipes::fluidIn(at[0]).empty())
                award(*player, "glass_pipe");
        }
    }
}

void installClicks() {
    // an observer before every other handler: it never takes the click
    handlers().push_back(ia::onUseOn({}, [](ia::UseOnContext const& c) {
        // (repeats while the button is held are checked again: awards and owners are idempotent)
        // Create's HandCrankBlock#use: turning a crank while starving
        if (c.blockId == "levitate:hand_crank") {
            auto ref = c.player.getAttribute(Player::HUNGER());
            if (auto const* inst = static_cast<AttributeInstance const*>(ref.mPtr); inst && inst->mCurrentValue <= 0.f) award(c.player, "hand_crank_000");
        }
        PendingClick p;
        p.uuid      = c.player.getUuid().asString();
        p.itemId    = c.itemId;
        p.clicked   = c.pos;
        p.face      = c.face;
        p.before[0] = c.blockId;
        p.before[1] = blockIdAt(offset(c.pos, c.face));
        p.due       = gTick + 2; // after the units' own deferred placements
        pending().push_back(std::move(p));
        return ia::Result::Pass;
    }, 5000));
}

// ---- block conditions ------------------------------------------------------------------------------------------------

bool lavaAround(GlobalPos const& pos, int radius) {
    auto* r = regionOf(pos.dim);
    if (!r) return false;
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dz = -radius; dz <= radius; ++dz) {
                auto id = game::blockIdAt(*r, BlockPos{pos.x + dx, pos.y + dy, pos.z + dz});
                if (id == "minecraft:lava" || id == "minecraft:flowing_lava") return true;
            }
    return false;
}

/// Speed-driven awards of a block (Create's onSpeedChanged / tick checks).
void onSpeed(GlobalPos const& pos, std::string const& id, float prev, float speed, std::optional<GlobalPos> const& source) {
    if (ownerOf(pos).empty()) return;
    float a = std::abs(speed);
    if (id == "levitate:encased_fan") {
        if (speed != 0) awardOwner(pos, "encased_fan");
    } else if (id == "levitate:mechanical_pump") {
        if (std::abs(prev) != a && speed != 0) awardOwner(pos, "mechanical_pump_0");
    } else if (id == "levitate:mechanical_crafter") {
        if (speed != 0) {
            awardOwner(pos, "mechanical_crafter");
            if (a < 5) awardOwner(pos, "crafter_lazy_000");
        }
    } else if (id == "levitate:rotation_speed_controller") {
        auto above = blockIdAt({pos.dim, pos.x, pos.y + 1, pos.z});
        if (speed != 0 && above.find("large_cogwheel") != std::string::npos) awardOwner(pos, "speed_controller");
    } else if (id == "levitate:water_wheel" || id == "levitate:large_water_wheel") {
        if (speed != 0 && source && *source == pos) awardOwner(pos, lavaAround(pos, id == "levitate:water_wheel" ? 1 : 2) ? "lava_wheel_00000" : "water_wheel");
    } else if (id == "levitate:crushing_wheel") {
        if (speed == 0) return;
        for (int f = 0; f < 6; ++f) {
            if (blockIdAt(offset(pos, f)) != "levitate:crushing_wheel_controller") continue;
            awardOwner(pos, "crushing_wheel");
            if (a > 256 - 1) awardOwner(pos, "crusher_maxed_0000");
            break;
        }
    } else if (id == "levitate:deployer") {
        if (speed != 0) awardOwner(pos, "deployer");
    } else if (id == "levitate:copper_backtank" || id == "levitate:netherite_backtank") {
        if (speed != 0) awardOwner(pos, "backtank");
    }
}

void evaluateBlock(GlobalPos const& pos, std::string const& id) {
    if (kinetics::specOf(id)) {
        float s = kinetics::speedAt(pos);
        // a placed generator is its own source
        std::optional<GlobalPos> source;
        if (id == "levitate:water_wheel" || id == "levitate:large_water_wheel") source = pos;
        onSpeed(pos, id, 0, s, source);
    }
}

void installSpeeds() {
    for (char const* id : {"levitate:encased_fan", "levitate:mechanical_pump", "levitate:mechanical_crafter", "levitate:rotation_speed_controller",
                           "levitate:water_wheel", "levitate:large_water_wheel", "levitate:crushing_wheel", "levitate:deployer",
                           "levitate:copper_backtank", "levitate:netherite_backtank"}) {
        listeners().push_back(kinetics::onSpeedChanged(id, [](kinetics::SpeedChange const& c) {
            if (c.removed) return;
            onSpeed(c.pos, c.blockId, c.prevSpeed, c.speed, c.source);
        }));
    }
}

// ---- periodic checks ---------------------------------------------------------------------------------------------------

Player* byUuid(std::string const& uuid) {
    auto level = ll::service::getLevel();
    return level ? level->getPlayer(mce::UUID::fromString(uuid)) : nullptr;
}

/// The block a player looks at within `reach` (a voxel walk from the eyes).
std::optional<GlobalPos> lookedAt(Player& p, double reach) {
    auto* r = regionOf(static_cast<int>(p.getDimensionId()));
    if (!r) return std::nullopt;
    Vec3 eye = p.getEyePos(), dir = p.getViewVector(1.f);
    for (double t = 0; t <= reach; t += 0.05) {
        BlockPos b{static_cast<int>(std::floor(eye.x + dir.x * t)), static_cast<int>(std::floor(eye.y + dir.y * t)), static_cast<int>(std::floor(eye.z + dir.z * t))};
        if (!r->getBlock(b).isAir()) return game::globalPos(*r, b);
    }
    return std::nullopt;
}

void checkOwnedBlocks() {
    for (auto const& [pos, uuid] : ownedBlocks()) {
        auto* owner = byUuid(uuid);
        if (!owner) continue;
        auto id = blockIdAt(pos);
        if (id.empty()) continue;
        if (id == "levitate:steam_whistle") {
            // Create's WhistleBlockEntity#tick: powered
            if (auto* r = regionOf(pos.dim); r && common::redstone::power(*r, game::blockPos(pos)) > 0) awardOwner(pos, "steam_whistle");
        } else if (id == "levitate:stressometer") {
            // Create's StressGaugeBlockEntity#onObserved: the owner looks at it through goggles
            if (items::idOf(owner->getArmor(ArmorSlot::Head)) != "levitate:goggles") continue;
            auto at = lookedAt(*owner, 5.0);
            if (!at || *at != pos) continue;
            awardOwner(pos, "stressometer");
            if (auto net = kinetics::networkAt(pos); net && net->capacity > 0 && std::abs(net->stress / net->capacity - 1.0) < 1e-5)
                awardOwner(pos, "stressometer_maxed");
        } else if (id == "levitate:windmill_bearing" || id == "levitate:clockwork_bearing") {
            auto st = bearings::bearingState(pos);
            if (!st.value("running", false)) continue;
            if (id == "levitate:clockwork_bearing") awardOwner(pos, "clockwork_bearing");
            else {
                awardOwner(pos, "windmill");
                if (st.value("sails", 0) >= 16 * 8) awardOwner(pos, "windmill_maxed");
            }
        } else if (id == "levitate:steam_engine") {
            // Create's SteamEngineBlockEntity: the engine turns its shaft (efficiency > 0)
            for (int f = 0; f < 6; ++f) {
                auto s = offset(pos, f);
                if (blockIdAt(s) == "levitate:powered_shaft" && kinetics::speedAt(s) != 0) {
                    awardOwner(pos, "steam_engine");
                    break;
                }
            }
        }
    }
}

/// Create's Contraption#containsBlockBreakers (drills, saws, ploughs, rollers, harvesters).
bool hasBreakers(contraption::Contraption const& c) {
    return std::ranges::any_of(c.blocks(), [](contraption::CarriedBlock const& b) {
        return b.id == "levitate:mechanical_drill" || b.id == "levitate:mechanical_saw" || b.id == "levitate:mechanical_plough"
            || b.id == "levitate:mechanical_roller" || b.id == "levitate:mechanical_harvester";
    });
}

std::set<contraption::Id>& seenContraptions() {
    static std::set<contraption::Id> s;
    return s;
}
bool gContraptionsPrimed = false;

/// Create awards contraption_actors when a structure with breakers is assembled (new contraptions
/// only; the ones that exist when the world loads were assembled before).
void checkContraptions() {
    std::set<contraption::Id> live;
    for (auto id : contraption::all()) {
        live.insert(id);
        if (seenContraptions().contains(id)) continue;
        seenContraptions().insert(id);
        if (!gContraptionsPrimed) continue;
        auto* c = contraption::find(id);
        if (!c || !hasBreakers(*c)) continue;
        awardOwner(c->anchor(), "contraption_actors");
    }
    std::erase_if(seenContraptions(), [&](contraption::Id id) { return !live.contains(id); });
    gContraptionsPrimed = true;
}

std::map<int, std::size_t>& glueCounts() {
    static std::map<int, std::size_t> m;
    return m;
}

/// Create's SuperGlueSelectionPacket: the player who made a new glue region (holding super glue).
void checkGlue() {
    auto level = ll::service::getLevel();
    if (!level) return;
    for (int dim = 0; dim < 3; ++dim) {
        auto boxes = contraption::glueRegions(dim);
        auto known = glueCounts().find(dim);
        if (known != glueCounts().end() && boxes.size() > known->second) {
            auto const& b = boxes.back();
            Player*     best = nullptr;
            double      bestD = 24 * 24;
            level->forEachPlayer([&](Player& p) {
                if (p.isClientSide() || static_cast<int>(p.getDimensionId()) != dim) return true;
                if (items::idOf(p.getSelectedItem()) != "levitate:super_glue" && items::idOf(p.getOffhandSlot()) != "levitate:super_glue") return true;
                auto   at = p.getPosition();
                double dx = at.x - (b.min.x + b.max.x) / 2.0, dy = at.y - (b.min.y + b.max.y) / 2.0, dz = at.z - (b.min.z + b.max.z) / 2.0;
                double d  = dx * dx + dy * dy + dz * dz;
                if (d < bestD) bestD = d, best = &p;
                return true;
            });
            if (best) award(*best, "super_glue");
        }
        glueCounts()[dim] = boxes.size();
    }
}


void checkPlayers() {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([](Player& p) {
        if (p.isClientSide()) return true;
        // Create's root: awarded for free
        award(p, "root");
        // InventoryChangeTrigger criteria (whenIconCollected / whenItemCollected)
        std::set<std::string> have;
        auto&                 inv = p.getInventory();
        for (int i = 0; i < inv.getContainerSize(); ++i) {
            auto const& s = inv.getItem(i);
            if (!items::isEmpty(s)) have.insert(items::idOf(s));
        }
        for (auto slot : {ArmorSlot::Head, ArmorSlot::Torso, ArmorSlot::Legs, ArmorSlot::Feet})
            if (!items::isEmpty(p.getArmor(slot))) have.insert(items::idOf(p.getArmor(slot)));
        if (!items::isEmpty(p.getOffhandSlot())) have.insert(items::idOf(p.getOffhandSlot()));
        for (auto const& d : ca::all()) {
            if (d.collect[0].empty() || has(p, d.id)) continue;
            bool all = std::ranges::all_of(d.collect, [&](std::string_view c) { return c.empty() || have.contains(std::string(c)); });
            if (all) award(p, d.id);
        }
        // Create's ExtendoGripItem#holdingExtendoGripIncreasesRange
        bool main = items::idOf(p.getSelectedItem()) == "levitate:extendo_grip";
        bool off  = items::idOf(p.getOffhandSlot()) == "levitate:extendo_grip";
        if (main || off) award(p, "extendo_grip");
        if (main && off) award(p, "extendo_grip_dual");
        return true;
    });
}

} // namespace

std::string blockIdAt(GlobalPos const& pos) {
    auto* r = regionOf(pos.dim);
    if (!r) return {};
    BlockPos bp{pos.x, pos.y, pos.z};
    if (pos.y < r->getMinHeight() || pos.y >= r->getMaxHeight() || !r->hasChunksAt(bp, 0, false)) return {};
    return game::blockIdAt(*r, bp);
}

void installTriggers() {
    installClicks();
    installSpeeds();
}

void uninstallTriggers() {
    for (auto id : handlers()) ia::remove(id);
    handlers().clear();
    for (auto id : listeners()) kinetics::removeListener(id);
    listeners().clear();
}

void tickTriggers() {
    ++gTick;
    if (!pending().empty()) {
        std::vector<PendingClick> due;
        std::erase_if(pending(), [&](PendingClick& c) {
            if (c.due > gTick) return false;
            due.push_back(std::move(c));
            return true;
        });
        for (auto const& c : due) resolveClick(c);
    }
    if (gTick % 20 == 0) {
        checkPlayers();
        checkOwnedBlocks();
        checkContraptions();
    }
    if (gTick % 10 == 5) checkGlue();
}

void resetTriggers() {
    pending().clear();
    seenContraptions().clear();
    gContraptionsPrimed = false;
    glueCounts().clear();
    gTick = 0;
}

} // namespace lv::feature::advancements
