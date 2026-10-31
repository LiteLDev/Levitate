// Mechanical arm (Create's ArmBlock / ArmBlockEntity / ArmInteractionPoint(Type)s /
// ArmInteractionPointHandler), SPEC §3.6.15.
//
// Holding the arm item, right-clicking a block that is an interaction point selects it as an input
// ("Take items from"), clicking it again makes it an output ("Deposit items to"); placing the arm
// keeps the points within range. While turning, the arm moves to an input that has something an
// output accepts, takes it, moves to that output and puts it in. Points are reached through the
// item capability (U3) of the block, so any unit's depot, belt, basin, funnel, chute, crafter, saw,
// millstone, packager, crushing wheel or blaze burner works without naming it; jukeboxes,
// composters, campfires and respawn anchors are vanilla points.
//
// BlockData "arm": {"points": [[dx, dy, dz, take], ...] (relative to the arm), "held", "phase",
// "chased", "progress", "lastIn", "lastOut", "mode"}.

#include <algorithm>
#include <map>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/RespawnAnchorBlock.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/CampfireBlockActor.h"

#include "core/assembly/Arm.h"
#include "core/common/ValueSettings.h"
#include "feature/advancements/Advancements.h"
#include "feature/assembly/AssemblyInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::assembly::arm {

namespace bd    = common::blockdata;
namespace items = common::items;
using ca::ArmPhase;
using ca::ArmSelectionMode;

namespace {

constexpr char kDbIndex[] = "lv:assembly:arms";
constexpr char kDataKey[] = "arm";
constexpr char kCarrier[] = "assembly:arm";

// ---- interaction point types (Create's ArmInteractionPointType registry) ------------------------

/// Create's BeltFunnelBlock.Shape.PUSHING in a funnel's `levitate:shape` (U12's kBeltPushing in
/// feature/funnels/FunnelsInternal.h, a header internal to that unit).
constexpr int kBeltFunnelPushing = 3;

enum class PointType {
    Basin,
    Belt,
    BlazeBurner,
    Chute,
    Crafter,
    CrushingWheels,
    Deployer,
    Depot,
    Funnel,
    Millstone,
    Packager,
    Saw,
    Campfire,
    Composter,
    Jukebox,
    RespawnAnchor,
};

std::optional<PointType> typeOf(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return std::nullopt;
    auto const&        block = region.getBlock(pos);
    std::string const& id    = block.getTypeName();
    static std::map<std::string, PointType, std::less<>> const ids{
        {"levitate:basin",                     PointType::Basin         },
        {"levitate:belt",                      PointType::Belt          },
        {"levitate:blaze_burner",              PointType::BlazeBurner   },
        {"levitate:chute",                     PointType::Chute         },
        {"levitate:smart_chute",               PointType::Chute         },
        {"levitate:mechanical_crafter",        PointType::Crafter       },
        {"levitate:crushing_wheel_controller", PointType::CrushingWheels},
        {kDeployerId,                          PointType::Deployer      },
        {"levitate:depot",                     PointType::Depot         },
        {"levitate:weighted_ejector",          PointType::Depot         },
        {"levitate:track_station",             PointType::Depot         },
        {"levitate:andesite_funnel",           PointType::Funnel        },
        {"levitate:brass_funnel",              PointType::Funnel        },
        {"levitate:millstone",                 PointType::Millstone     },
        {"levitate:packager",                  PointType::Packager      },
        {"levitate:repackager",                PointType::Packager      },
        {"levitate:mechanical_saw",            PointType::Saw           },
        {"minecraft:campfire",                 PointType::Campfire      },
        {"minecraft:soul_campfire",            PointType::Campfire      },
        {"minecraft:composter",                PointType::Composter     },
        {"minecraft:jukebox",                  PointType::Jukebox       },
        {"minecraft:respawn_anchor",           PointType::RespawnAnchor },
    };
    auto it = ids.find(id);
    if (it == ids.end()) return std::nullopt;
    // Create's SawType: only saws facing up that turn
    if (it->second == PointType::Saw) {
        if (detail::intState(block, "levitate:facing").value_or(1) != 1) return std::nullopt;
        if (kinetics::speedAt(game::globalPos(region, pos)) == 0) return std::nullopt;
    }
    // Create's FunnelType: not an extracting funnel, not a belt funnel pushing onto its belt
    if (it->second == PointType::Funnel) {
        if (detail::intState(block, "levitate:extracting").value_or(0) != 0) return std::nullopt;
        if (detail::intState(block, "levitate:shape").value_or(0) == kBeltFunnelPushing) return std::nullopt;
    }
    return it->second;
}

/// Where the claw reaches (world coordinates) and from which side (rendering only).
std::pair<core::assembly::Vec3f, Face> interactionTarget(PointType type, BlockPos const& p) {
    double y = 1.0;
    switch (type) {
    case PointType::Depot:
        y = 14 / 16.0;
        break;
    case PointType::Belt:
        y = 14 / 16.0;
        break;
    case PointType::Basin:
    case PointType::Millstone:
    case PointType::Funnel:
    case PointType::Chute:
        y = 0.5 + 6 / 16.0;
        break;
    case PointType::Packager: // Create's ArmInteractionPoint: the centre of the block
        y = 0.5;
        break;
    default:
        break;
    }
    return {
        {p.x + 0.5, p.y + y, p.z + 0.5},
        Face::Down
    };
}

int insertFace(PointType t) { return t == PointType::Composter ? 1 : -1; }
int extractFace(PointType t) { return t == PointType::Composter ? 0 : -1; }

// Create's ArmInteractionPoint.insert / extract / getSlotCount through the item capability, or the
// vanilla block behaviour for points without an inventory.
ItemStack insertInto(BlockSource& region, BlockPos const& pos, PointType type, ItemStack const& stack, bool simulate) {
    if (items::isEmpty(stack)) return stack;
    if (type == PointType::RespawnAnchor) {
        if (items::idOf(stack) != "minecraft:glowstone") return stack;
        int charge = detail::intState(region.getBlock(pos), "respawn_anchor_charge").value_or(4);
        if (charge >= 4) return stack;
        if (!simulate) RespawnAnchorBlock::_bumpCharge(region, pos, nullptr, 1);
        return items::withCount(stack, items::countOf(stack) - 1);
    }
    if (type == PointType::Campfire) {
        auto level = ll::service::getLevel();
        if (!level) return stack;
        auto cooked = level->getRecipes().getFurnaceRecipeResult(stack, HashedString("campfire"));
        if (cooked.isNull()) return stack;
        auto* actor = region.getBlockEntity(pos);
        if (!actor) return stack;
        auto& campfire = static_cast<CampfireBlockActor&>(*actor);
        bool free = false;
        auto const* cooking = reinterpret_cast<ItemInstance const*>(&campfire.mCookingItem);
        for (int i = 0; i < 4; ++i)
            if (cooking[i].isNull()) free = true;
        if (!free) return stack;
        if (!simulate && !campfire.setItem(region, ItemInstance(items::withCount(stack, 1)))) return stack;
        return items::withCount(stack, items::countOf(stack) - 1);
    }
    return items::insert(region, pos, insertFace(type), stack, simulate);
}

int slotCountOf(BlockSource& region, BlockPos const& pos, PointType type) {
    if (type == PointType::RespawnAnchor || type == PointType::Campfire) return 0;
    return items::slotCount(region, pos, extractFace(type));
}

ItemStack slotOf(BlockSource& region, BlockPos const& pos, PointType type, int slot) {
    common::Access at{region, pos, extractFace(type)};
    auto           h = items::find(at);
    if (!h || !h->getSlot) return items::empty();
    return h->getSlot(at, slot);
}

ItemStack extractFrom(BlockSource& region, BlockPos const& pos, PointType type, int slot, int amount, bool simulate) {
    auto kind = slotOf(region, pos, type, slot);
    if (items::isEmpty(kind)) return items::empty();
    return items::extract(region, pos, extractFace(type), [kind](ItemStack const& s) { return items::sameItemSameData(s, kind); }, amount, simulate);
}

// ---- runtime ------------------------------------------------------------------------------------

struct Point {
    int  dx = 0, dy = 0, dz = 0;
    bool take = true;
};

struct Runtime {
    std::vector<Point>  points;
    ItemStack           held;
    ArmPhase            phase    = ArmPhase::SearchInputs;
    int                 chased   = -1;
    float               progress = 0;
    int                 lastIn = -1, lastOut = -1;
    ArmSelectionMode    mode   = ArmSelectionMode::RoundRobin;
    bool                redstone = false;
    ca::ArmAngles       angles   = ca::armRestAngles(); ///< pose of the last target (animation)
};

struct Selection {
    int                                 dim = 0;
    std::vector<std::pair<BlockPos, bool>> points; ///< world pos, take
};

struct State {
    std::map<GlobalPos, Runtime>    runtimes;
    std::set<GlobalPos>             index;
    bool                            indexDirty = false;
    std::map<std::string, Selection> selections; ///< by player UUID
    kinetics::ListenerId            speedListener = -1;
};
State& st() {
    static State s;
    return s;
}

Json toJson(Runtime const& r) {
    Json pts = Json::array();
    for (auto const& p : r.points) pts.push_back({p.dx, p.dy, p.dz, p.take ? 1 : 0});
    Json j{
        {"points",   pts                        },
        {"phase",    static_cast<int>(r.phase)  },
        {"chased",   r.chased                   },
        {"progress", r.progress                 },
        {"lastIn",   r.lastIn                   },
        {"lastOut",  r.lastOut                  },
        {"mode",     static_cast<int>(r.mode)   },
    };
    if (!items::isEmpty(r.held)) j["held"] = items::toJson(r.held);
    return j;
}

Runtime fromJson(Json const& j) {
    Runtime r;
    if (!j.is_object()) return r;
    if (auto it = j.find("points"); it != j.end() && it->is_array()) {
        for (auto const& e : *it)
            if (e.is_array() && e.size() == 4) r.points.push_back({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>() != 0});
    }
    r.phase    = static_cast<ArmPhase>(std::clamp(j.value("phase", 0), 0, 4));
    r.chased   = j.value("chased", -1);
    r.progress = j.value("progress", 0.f);
    r.lastIn   = j.value("lastIn", -1);
    r.lastOut  = j.value("lastOut", -1);
    r.mode     = static_cast<ArmSelectionMode>(std::clamp(j.value("mode", 0), 0, 2));
    if (auto it = j.find("held"); it != j.end()) r.held = items::fromJson(*it);
    return r;
}

void persistIndex() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : st().index) j.push_back(detail::posJson(p));
    game::db::save(kDbIndex, j);
    st().indexDirty = false;
}

void track(GlobalPos const& pos) {
    if (st().index.insert(pos).second) st().indexDirty = true;
}

Runtime& rt(GlobalPos const& pos) {
    auto& m = st().runtimes;
    if (auto it = m.find(pos); it != m.end()) return it->second;
    Runtime r;
    if (auto const* d = bd::get(pos))
        if (auto it = d->find(kDataKey); it != d->end()) r = fromJson(*it);
    track(pos);
    return m.emplace(pos, std::move(r)).first->second;
}

void save(GlobalPos const& pos, Runtime const& r) { bd::edit(pos)[kDataKey] = toJson(r); }

BlockPos worldOf(GlobalPos const& arm, Point const& p) { return BlockPos{arm.x + p.dx, arm.y + p.dy, arm.z + p.dz}; }

std::vector<Point> listOf(Runtime const& r, bool take) {
    std::vector<Point> out;
    for (auto const& p : r.points)
        if (p.take == take) out.push_back(p);
    return out;
}

bool ceilingOf(BlockSource& region, GlobalPos const& pos) {
    return detail::intState(region.getBlock(game::blockPos(pos)), "levitate:ceiling").value_or(0) != 0;
}

/// Vanilla clients: the held item as a client-only item entity under the claw of the block model
/// (Create's ArmRenderer draws it between the moving claws; the client mod does that).
struct VanillaView {
    VanillaView() {
        common::vanillaitems::registerSource([](std::vector<common::vanillaitems::Shown>& out) {
            for (auto const& [pos, r] : st().runtimes) {
                if (items::isEmpty(r.held)) continue;
                auto* region = detail::regionOf(pos.dim);
                if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) continue;
                if (region->getBlock(game::blockPos(pos)).getTypeName() != kArmId) continue;
                // the model's head hangs over the north edge, 16..20 px up (flipped on a ceiling)
                float y = ceilingOf(*region, pos) ? .05f : .7f;
                out.push_back({pos, 0, items::withCount(r.held, 1), Vec3{pos.x + .5f, pos.y + y, pos.z + .5f - 7 / 16.f}});
            }
        });
    }
} const gVanillaView;

/// The point currently chased (Create's getTargetedInteractionPoint).
std::optional<Point> targeted(Runtime const& r) {
    if (r.chased < 0) return std::nullopt;
    if (r.phase == ArmPhase::MoveToInput) {
        auto in = listOf(r, true);
        if (r.chased < static_cast<int>(in.size())) return in[static_cast<std::size_t>(r.chased)];
    }
    if (r.phase == ArmPhase::MoveToOutput) {
        auto out = listOf(r, false);
        if (r.chased < static_cast<int>(out.size())) return out[static_cast<std::size_t>(r.chased)];
    }
    return std::nullopt;
}

void syncState(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    bool          ceiling = ceilingOf(region, pos);
    ca::ArmAngles from    = r.angles;
    ca::ArmAngles to      = ca::armRestAngles();
    bool          has     = false;
    if (auto t = targeted(r)) {
        auto wp = worldOf(pos, *t);
        if (auto type = typeOf(region, wp)) {
            auto [target, claw] = interactionTarget(*type, wp);
            to  = ca::armAnglesFor({pos.x, pos.y, pos.z}, target, claw, ceiling);
            has = true;
        }
    }
    if (has) r.angles = to;
    auto angles = [](ca::ArmAngles const& a) { return Json::array({a.base, a.lower, a.upper, a.head}); };
    sync::arm(
        pos,
        {{"phase", static_cast<int>(r.phase)},
         {"progress", r.progress},
         {"step", ca::armProgressPerTick(kinetics::speedAt(pos))},
         {"from", angles(from)},
         {"to", angles(to)},
         {"target", has},
         {"ceiling", ceiling},
         {"held", items::isEmpty(r.held) ? "" : items::idOf(r.held)},
         {"heldItem", items::isEmpty(r.held) ? Json() : items::toJson(r.held)}}
    );
}

// ---- Create's ArmBlockEntity logic ----------------------------------------------------------------

bool pointValid(BlockSource& region, GlobalPos const& arm, Point const& p) { return typeOf(region, worldOf(arm, p)).has_value(); }

ItemStack simulateInsertion(BlockSource& region, GlobalPos const& arm, Runtime const& r, ItemStack stack) {
    for (auto const& p : listOf(r, false)) {
        auto wp = worldOf(arm, p);
        if (auto t = typeOf(region, wp)) stack = insertInto(region, wp, *t, stack, true);
        if (items::isEmpty(stack)) break;
    }
    return stack;
}

int distributableAmount(BlockSource& region, GlobalPos const& arm, Runtime const& r, Point const& p, int slot) {
    auto wp   = worldOf(arm, p);
    auto type = typeOf(region, wp);
    if (!type) return 0;
    auto stack = extractFrom(region, wp, *type, slot, 64, true);
    if (items::isEmpty(stack)) return 0;
    auto rest = simulateInsertion(region, arm, r, stack);
    if (items::sameItem(stack, rest)) return items::countOf(stack) - items::countOf(rest);
    return items::countOf(stack);
}

void selectIndex(GlobalPos const& pos, Runtime& r, BlockSource& region, bool input, int index) {
    r.phase    = input ? ArmPhase::MoveToInput : ArmPhase::MoveToOutput;
    r.chased   = index;
    r.progress = 0;
    if (input) r.lastIn = index;
    else r.lastOut = index;
    save(pos, r);
    syncState(pos, r, region);
}

void searchForItem(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    if (r.redstone) return;
    auto inputs = listOf(r, true);
    int  count  = static_cast<int>(inputs.size());
    auto scan   = ca::armScanRange(r.mode, r.lastIn, count);
    bool found  = false;
    for (int i = scan.begin; i < scan.end && !found; ++i) {
        auto const& p = inputs[static_cast<std::size_t>(i)];
        if (!pointValid(region, pos, p)) continue;
        auto wp    = worldOf(pos, p);
        int  slots = slotCountOf(region, wp, *typeOf(region, wp));
        for (int s = 0; s < slots; ++s) {
            if (distributableAmount(region, pos, r, p, s) == 0) continue;
            selectIndex(pos, r, region, true, i);
            found = true;
            break;
        }
    }
    r.lastIn = ca::armLastIndexAfter(r.mode, r.lastIn, found, count);
}

void searchForDestination(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    auto outputs = listOf(r, false);
    int  count   = static_cast<int>(outputs.size());
    auto scan    = ca::armScanRange(r.mode, r.lastOut, count);
    bool found   = false;
    for (int i = scan.begin; i < scan.end; ++i) {
        auto const& p = outputs[static_cast<std::size_t>(i)];
        if (!pointValid(region, pos, p)) continue;
        auto wp   = worldOf(pos, p);
        auto rest = insertInto(region, wp, *typeOf(region, wp), r.held, true);
        if (items::sameItemSameData(rest, r.held) && items::countOf(rest) == items::countOf(r.held)) continue;
        selectIndex(pos, r, region, false, i);
        found = true;
        break;
    }
    r.lastOut = ca::armLastIndexAfter(r.mode, r.lastOut, found, count);
}

void collectItem(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    if (auto p = targeted(r); p && pointValid(region, pos, *p)) {
        auto wp    = worldOf(pos, *p);
        auto type  = *typeOf(region, wp);
        int  slots = slotCountOf(region, wp, type);
        for (int s = 0; s < slots; ++s) {
            int amount = distributableAmount(region, pos, r, *p, s);
            if (amount == 0) continue;
            ItemStack prev = r.held;
            r.held         = extractFrom(region, wp, type, s, amount, false);
            r.phase        = ArmPhase::SearchOutputs;
            r.progress     = 0;
            r.chased       = -1;
            save(pos, r);
            syncState(pos, r, region);
            if (!items::sameItem(r.held, prev))
                detail::playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.pop", .125f, .5f + detail::currentTick() % 4 * .0625f);
            if (int more = 0; detail::logAction("arm.take@" + pos.key() + "#" + items::idOf(r.held), more))
                log().info("arm at {} took {} x{}{}", pos.key(), items::idOf(r.held), items::countOf(r.held), detail::similar(more));
            return;
        }
    }
    r.phase    = ArmPhase::SearchInputs;
    r.progress = 0;
    r.chased   = -1;
    save(pos, r);
    syncState(pos, r, region);
}

void depositItem(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    if (auto p = targeted(r); p && pointValid(region, pos, *p)) {
        auto wp     = worldOf(pos, *p);
        auto before = items::countOf(r.held);
        auto id     = items::idOf(r.held);
        auto type   = *typeOf(region, wp);
        r.held      = insertInto(region, wp, type, r.held, false);
        if (type == PointType::Jukebox && items::isEmpty(r.held)) advancements::awardOwner(pos, "musical_arm");
        if (int more = 0; items::countOf(r.held) != before && detail::logAction("arm.deposit@" + pos.key() + "#" + id, more))
            log().info("arm at {} deposited {} x{}{}", pos.key(), id, before - items::countOf(r.held), detail::similar(more));
    }
    r.phase    = items::isEmpty(r.held) ? ArmPhase::SearchInputs : ArmPhase::SearchOutputs;
    r.progress = 0;
    r.chased   = -1;
    save(pos, r);
    syncState(pos, r, region);
    advancements::awardOwner(pos, "mechanical_arm");
}

bool hasMusic(GlobalPos const& pos, Runtime const& r, BlockSource& region) {
    for (auto const& p : r.points) {
        auto wp = worldOf(pos, p);
        if (typeOf(region, wp) != PointType::Jukebox) continue;
        if (!items::isEmpty(slotOf(region, wp, PointType::Jukebox, 0))) return true;
    }
    return false;
}

void lazyTick(GlobalPos const& pos, Runtime& r, BlockSource& region) {
    if (r.progress < .5f) return;
    if (r.phase == ArmPhase::SearchInputs || r.phase == ArmPhase::Dancing) {
        bool music = hasMusic(pos, r, region);
        if (music != (r.phase == ArmPhase::Dancing)) {
            r.phase = music ? ArmPhase::Dancing : ArmPhase::SearchInputs;
            save(pos, r);
            syncState(pos, r, region);
        }
    }
    if (r.phase == ArmPhase::SearchOutputs) searchForDestination(pos, r, region);
}

void tickOne(GlobalPos const& pos) {
    auto* region = detail::regionOf(pos.dim);
    if (!region) return;
    auto bp = game::blockPos(pos);
    if (!region->hasChunksAt(bp, 0, false)) return;
    if (region->getBlock(bp).getTypeName() != kArmId) {
        st().runtimes.erase(pos);
        if (st().index.erase(pos)) st().indexDirty = true;
        return;
    }
    auto& r        = rt(pos);
    r.redstone     = detail::powered(*region, bp);
    bool before    = r.progress >= 1;
    r.progress     = std::min(1.f, r.progress + ca::armProgressPerTick(kinetics::speedAt(pos)));
    bool reached   = !before && r.progress >= 1;
    if (r.progress >= 1) {
        if (r.phase == ArmPhase::MoveToInput) collectItem(pos, r, *region);
        else if (r.phase == ArmPhase::MoveToOutput) depositItem(pos, r, *region);
        else if (r.phase == ArmPhase::SearchInputs || r.phase == ArmPhase::Dancing) searchForItem(pos, r, *region);
    }
    if (reached || (detail::currentTick() + static_cast<std::uint64_t>(pos.x + pos.z)) % 10 == 0) lazyTick(pos, r, *region);
}

std::string blockName(BlockSource& region, BlockPos const& pos) {
    auto id = game::blockIdAt(region, pos);
    if (auto s = items::make(id)) return items::displayName(*s);
    return id;
}

std::string uuidOf(Player const& p) { return p.getUuid().asString(); }

void showSelection(Player& player, Selection const& sel) {
    for (auto const& [p, take] : sel.points) {
        detail::particle(sel.dim, p.x + .5, p.y + 1.05, p.z + .5, take ? "minecraft:blue_flame_particle" : "minecraft:basic_flame_particle");
    }
    (void)player;
}

// ---- value box: Create's SelectionModeValueBox ---------------------------------------------------

void registerValueBox() {
    common::valuebox::BoxSpec box;
    box.id            = "selection_mode";
    box.label         = "create.logistics.when_multiple_outputs_available";
    box.labelFallback = "Distribution Method";
    box.scale         = 0.3;
    box.board         = [](Block const&) { return core::common::optionBoard(3, ""); };
    box.centre        = [](Block const& block, int face) -> std::optional<core::common::Vec> {
        if (face < 2 || face > 5) return std::nullopt;
        bool ceiling = detail::intState(block, "levitate:ceiling").value_or(0) != 0;
        return core::common::faceLocalPoint(face, 8, ceiling ? 13 : 3, 0.5);
    };
    box.get = [](GlobalPos const& pos) { return core::common::ValueSettings{0, static_cast<int>(rt(pos).mode)}; };
    box.set = [](GlobalPos const& pos, core::common::ValueSettings s, Player&) { setSelectionMode(pos, static_cast<ArmSelectionMode>(std::clamp(s.value, 0, 2))); };
    box.format = [](Block const&, core::common::ValueSettings s) -> std::string {
        switch (s.value) {
        case 1:
            return tr("create.mechanical_arm.selection_mode.forced_round_robin", "Forced Round Robin");
        case 2:
            return tr("create.mechanical_arm.selection_mode.prefer_first", "Prefer First Target");
        default:
            return tr("create.mechanical_arm.selection_mode.round_robin", "Round Robin");
        }
    };
    common::valuebox::registerBox(kArmId, std::move(box));
}

void registerCarrier() {
    contraption::registerDataCarrier({
        kCarrier,
        [](BlockSource& region, GlobalPos const& pos) -> std::optional<Json> {
            if (game::blockIdAt(region, game::blockPos(pos)) != kArmId) return std::nullopt;
            st().runtimes.erase(pos);
            if (st().index.erase(pos)) st().indexDirty = true;
            auto doc = bd::take(pos);
            return doc ? std::optional<Json>(*doc) : std::optional<Json>(Json::object());
        },
        [](BlockSource&, GlobalPos const& pos, Json const& doc) {
            if (doc.is_object() && !doc.empty()) bd::put(pos, doc);
            track(pos);
        },
        [](Json& doc, contraption::cc::Axis axis, int quarter) {
            // points are relative to the arm: turn them with it (Create's StructureTransform)
            if (!doc.contains(kDataKey)) return;
            auto r = fromJson(doc[kDataKey]);
            for (auto& p : r.points) {
                auto q = contraption::cc::rotate(contraption::cc::Pos{p.dx, p.dy, p.dz}, axis, quarter);
                p.dx   = q.x;
                p.dy   = q.y;
                p.dz   = q.z;
            }
            doc[kDataKey] = toJson(r);
        },
    });
}

} // namespace

// ---- public API ---------------------------------------------------------------------------------

bool isPoint(BlockSource& region, BlockPos const& pos) { return typeOf(region, pos).has_value(); }

bool onSelect(Player& player, BlockPos const& pos) {
    if (player.isSpectator()) return false;
    auto& region = player.getDimensionBlockSource();
    auto  type   = typeOf(region, pos);
    if (!type) return false;
    int   dim = static_cast<int>(player.getDimensionId());
    auto& sel = st().selections[uuidOf(player)];
    if (sel.dim != dim) sel = Selection{dim, {}};
    auto it = std::find_if(sel.points.begin(), sel.points.end(), [&](auto const& e) { return e.first == pos; });
    bool take;
    if (it == sel.points.end()) {
        sel.points.emplace_back(pos, true);
        take = true;
    } else {
        it->second = !it->second;
        take       = it->second;
    }
    auto name = blockName(region, pos);
    detail::actionBar(
        player,
        take ? "§b" + trFor(player, "create.mechanical_arm.extract_from", "Take items from {0}", name)
             : "§6" + trFor(player, "create.mechanical_arm.deposit_to", "Deposit items to {0}", name)
    );
    showSelection(player, sel);
    log().info("arm selection of {}: {} {} {} {}", player.getRealName(), pos.x, pos.y, pos.z, take ? "input" : "output");
    return true;
}

bool onDeselect(Player& player, BlockPos const& pos) {
    auto it = st().selections.find(uuidOf(player));
    if (it == st().selections.end()) return false;
    auto& pts = it->second.points;
    auto  n   = std::erase_if(pts, [&](auto const& e) { return e.first == pos; });
    return n > 0;
}

void onPlaced(BlockSource& region, BlockPos const& pos, Player* placer, Face clickedFace) {
    if (game::blockIdAt(region, pos) != kArmId) return;
    bool ceiling = clickedFace == Face::Down;
    if (auto const* b = detail::withStates(region.getBlock(pos), {{"levitate:ceiling", ceiling ? 1 : 0}})) game::setBlock(region, pos, *b);
    auto gp = game::globalPos(region, pos);
    st().runtimes.erase(gp);
    auto& r = rt(gp);
    r       = Runtime{};
    if (placer) {
        auto it = st().selections.find(uuidOf(*placer));
        if (it != st().selections.end() && it->second.dim == gp.dim) {
            int removed = 0;
            int range   = config().mechanicalArmRange;
            for (auto const& [p, take] : it->second.points) {
                int dx = p.x - pos.x, dy = p.y - pos.y, dz = p.z - pos.z;
                if (!ca::armInRange(dx, dy, dz, range)) {
                    ++removed;
                    continue;
                }
                r.points.push_back({dx, dy, dz, take});
            }
            st().selections.erase(it);
            int in  = static_cast<int>(listOf(r, true).size());
            int out = static_cast<int>(listOf(r, false).size());
            if (removed > 0)
                detail::actionBar(*placer, "§c" + trFor(*placer, "create.mechanical_arm.points_outside_range", "{0} selected interaction point(s) removed due to range limitations.", removed));
            else if (in + out > 0)
                detail::actionBar(*placer, trFor(*placer, "create.mechanical_arm.summary", "Mechanical Arm has {0} input(s) and {1} output(s).", in, out));
        }
    }
    save(gp, r);
    log().info("mechanical arm placed at {} with {} input(s) and {} output(s)", gp.key(), listOf(r, true).size(), listOf(r, false).size());
    // Create's initInteractionPoints (the owner is known from the placement on)
    if (placer) advancements::setOwner(gp, *placer);
    if (listOf(r, false).size() >= 10) advancements::awardOwner(gp, "arm_many_targets");
    if (std::any_of(r.points.begin(), r.points.end(), [&](Point const& p) { return typeOf(region, worldOf(gp, p)) == PointType::BlazeBurner; }))
        advancements::awardOwner(gp, "arm_blaze_burner");
}

std::pair<int, int> selectionCounts(Player const& player) {
    auto it = st().selections.find(uuidOf(player));
    if (it == st().selections.end()) return {0, 0};
    int in = 0, out = 0;
    for (auto const& [p, take] : it->second.points) (take ? in : out)++;
    return {in, out};
}

std::pair<int, int> pointCounts(GlobalPos const& pos) {
    auto& r = rt(pos);
    return {static_cast<int>(listOf(r, true).size()), static_cast<int>(listOf(r, false).size())};
}

ItemStack held(GlobalPos const& pos) { return rt(pos).held; }
ArmPhase  phase(GlobalPos const& pos) { return rt(pos).phase; }

void setSelectionMode(GlobalPos const& pos, ArmSelectionMode mode) {
    auto& r = rt(pos);
    r.mode  = mode;
    save(pos, r);
    log().info("arm at {} selection mode {}", pos.key(), static_cast<int>(mode));
}

bool addPoint(GlobalPos const& arm, BlockPos const& point, bool take) {
    auto* region = detail::regionOf(arm.dim);
    if (!region || !typeOf(*region, point)) return false;
    auto& r = rt(arm);
    r.points.push_back({point.x - arm.x, point.y - arm.y, point.z - arm.z, take});
    save(arm, r);
    return true;
}

void tick() {
    std::vector<GlobalPos> list(st().index.begin(), st().index.end());
    for (auto const& p : list) {
        try {
            tickOne(p);
        } catch (std::exception const& e) {
            log().error("mechanical arm at {} failed: {}", p.key(), e.what());
        }
    }
    if (st().indexDirty && detail::currentTick() % 100 == 0) persistIndex();
    // selections: forgotten when the player stops holding the arm (Create's handler), shown meanwhile
    if (detail::currentTick() % 10 == 0 && !st().selections.empty()) {
        auto level = ll::service::getLevel();
        if (!level) return;
        std::set<std::string> holding;
        level->forEachPlayer([&](Player& p) {
            auto const& item = p.getSelectedItem();
            if (!item.isNull() && item.getTypeName() == kArmId) {
                holding.insert(uuidOf(p));
                if (auto it = st().selections.find(uuidOf(p)); it != st().selections.end()) showSelection(p, it->second);
            }
            return true;
        });
        std::erase_if(st().selections, [&](auto const& e) { return !holding.contains(e.first); });
    }
}

void onWorldReady() {
    st().runtimes.clear();
    st().index.clear();
    st().selections.clear();
    if (auto j = game::db::load(kDbIndex); j && j->is_array())
        for (auto const& e : *j)
            if (auto p = detail::posFrom(e)) st().index.insert(*p);
}

void onWorldSave() {
    if (st().indexDirty) persistIndex();
}

void onWorldClosed() {
    st().runtimes.clear();
    st().index.clear();
    st().selections.clear();
}

void install() {
    kinetics::KineticSpec spec;
    spec.states          = {"levitate:ceiling"};
    spec.rotationAxis    = [](core::kinetics::BlockState const&) -> std::optional<core::kinetics::Axis> { return core::kinetics::Axis::Y; };
    spec.hasShaftTowards = [](core::kinetics::BlockState const& s, Face f) { return f == (s.get("levitate:ceiling") ? Face::Up : Face::Down); };
    // Create's ArmBlock is an ICogWheel: the cog on its base meshes with cogwheels beside it (U50)
    spec.cog             = [](core::kinetics::BlockState const&) { return core::kinetics::CogType::Small; };
    spec.impact          = 2;
    spec.rotorState      = "levitate:rotor";
    spec.rotorModel      = "geometry.levitate.mechanical_arm_cog";
    spec.rotorTexture    = "levitate_mechanical_arm";
    kinetics::registerBlock(kArmId, std::move(spec));
    registerValueBox();
    registerCarrier();
    bd::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
        if (previous != kArmId) return;
        st().runtimes.erase(pos);
        if (st().index.erase(pos)) st().indexDirty = true;
        auto it = doc.find(kDataKey);
        if (it == doc.end()) return;
        auto r = fromJson(*it);
        if (auto* region = detail::regionOf(pos.dim); region && !items::isEmpty(r.held))
            detail::dropItem(*region, pos.x + .5, pos.y + .5, pos.z + .5, r.held);
    });
    st().speedListener = kinetics::onSpeedChanged(kArmId, [](kinetics::SpeedChange const& c) {
        if (c.added) track(c.pos);
    });
}

void uninstall() {
    if (st().speedListener >= 0) kinetics::removeListener(st().speedListener);
    st().speedListener = -1;
}

} // namespace lv::feature::assembly::arm

// Create's ArmBlockEntity#addToTooltip (U59): an arm without interaction points hints at selecting
// some ("create.hint.mechanical_arm_no_targets").
#include "feature/goggles/GoggleInfo.h"
namespace lv::feature::assembly::arm {
LV_REGISTER_GOGGLE_INFO(kArmId, {.hover = [](goggles::InfoContext const& c, goggles::Tooltip& t) {
    if (c.sneaking) return false;
    auto [inputs, outputs] = pointCounts(c.pos);
    if (inputs != 0 || outputs != 0) return false;
    t.push_back(goggles::line(std::string(core::goggles::fmt::kGold) + c.tr("create.hint.mechanical_arm_no_targets.title", "No Targets")));
    goggles::addHint(t, c.tr("create.hint.mechanical_arm_no_targets",
                             "It appears this _Mechanical_ _Arm_ has not been assigned any _targets._ Select belts, depots, funnels and other blocks by _right-clicking_ them while _holding_ the _Mechanical_ _Arm_ in your _hand_."));
    return true;
}});
} // namespace lv::feature::assembly::arm
