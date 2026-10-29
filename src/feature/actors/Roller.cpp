// Mechanical roller (Create 6's RollerMovementBehaviour / RollerBlockEntity).
//
// Settings (stationary, carried with the block on a contraption): the paving material filter
// slot and the mode value box (Clear Blocks and Pave / Straight Fill Below / Sloped Fill Below),
// both shared along a row of rollers. Moving towards its front, its active area (0.45 blocks
// ahead and 2 blocks down) damages and throws entities, clears the blocks above the path
// (tunnel pave) and paves / fills below it with the material taken from contraption storage.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/util/Random.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "feature/actors/Actors.h"
#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/I18n.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::actors_ext;
namespace bd  = ::lv::feature::common::blockdata;
namespace vb  = ::lv::feature::common::valuebox;
namespace fs  = ::lv::feature::common::filterslot;
namespace it  = ::lv::feature::common::items;

namespace {

char const* const kModeKeys[3]{
    "create.contraptions.roller_mode.tunnel_pave",
    "create.contraptions.roller_mode.straight_fill",
    "create.contraptions.roller_mode.wide_fill",
};
char const* const kModeText[3]{"Clear Blocks and Pave", "Straight Fill Below", "Sloped Fill Below"};

RollingMode modeOfDoc(Json const* doc) {
    if (!doc || !doc->is_object()) return RollingMode::TunnelPave;
    auto a = doc->find("actors");
    if (a == doc->end() || !a->is_object()) return RollingMode::TunnelPave;
    return static_cast<RollingMode>(std::clamp(a->value("mode", 0), 0, 2));
}

ItemStack filterOfDoc(Json const* doc) {
    if (!doc || !doc->is_object()) return ItemStack::EMPTY_ITEM();
    auto f = doc->find("filter");
    if (f == doc->end() || !f->is_object() || !f->contains("filter")) return ItemStack::EMPTY_ITEM();
    return it::fromJson((*f)["filter"]);
}

/// Create's RollerBlockEntity filter rule: a full-cube block without a block entity, not stairs.
Block const* pavingBlockOf(ItemStack const& filter) {
    if (it::isEmpty(filter)) return nullptr;
    std::string id = filter.getTypeName();
    if (id.ends_with("_stairs") || id.ends_with("_slab")) return nullptr;
    auto const* block = game::blockWith(id);
    if (!block || block->isAir()) return nullptr;
    if (block->getBlockType().mBlockEntityType != BlockActorType::Undefined) return nullptr;
    return block;
}

bool fullCube(BlockSource& region, BlockPos const& pos, Block const& block) {
    AABB b = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
    auto near = [](float a, float c) { return std::abs(a - c) < 1e-3f; };
    return near(b.min.x, float(pos.x)) && near(b.min.y, float(pos.y)) && near(b.min.z, float(pos.z)) && near(b.max.x, float(pos.x + 1))
        && near(b.max.y, float(pos.y + 1)) && near(b.max.z, float(pos.z + 1));
}

bool isPortal(std::string const& id) {
    return id == "minecraft:portal" || id == "minecraft:end_portal" || id == "minecraft:end_gateway";
}

/// Create's RollerMovementBehaviour.canBreak.
bool rollerCanBreak(BlockSource& region, BlockPos const& pos, Block const& block) {
    for (auto d : cc::kAllDirs) {
        auto o = cc::offset(d);
        if (isPortal(region.getBlock(BlockPos{pos.x + o.x, pos.y + o.y, pos.z + o.z}).getTypeName())) return false;
    }
    if (!hasCollision(region, pos)) return false;
    std::string id = block.getTypeName();
    if (id == "levitate:track" || id == "levitate:fake_track") return false;
    return isBreakable(block);
}

struct Settings {
    RollingMode  mode   = RollingMode::TunnelPave;
    ItemStack    filter = ItemStack::EMPTY_ITEM();
    Block const* paving = nullptr;
};

Settings settingsOf(con::MovementContext const& ctx) {
    Settings    s;
    auto const* doc = ext::carriedBlockData(ctx.contraption, ctx.localPos);
    s.mode          = modeOfDoc(doc);
    s.filter        = filterOfDoc(doc);
    s.paving        = pavingBlockOf(s.filter);
    return s;
}

bool hasStock(con::MovementContext& ctx, Settings const& s) {
    if (!s.paving) return false;
    std::string id   = s.filter.getTypeName();
    auto        got  = ctx.contraption.storage().extractItem([&](ItemStack const& i) { return i.getTypeName() == id; }, 1, true);
    return !got.isNull() && got.mCount > 0;
}

enum class Result { Fail, Pass, Success };

/// One position of the paver (Create's RollerMovementBehaviour.tryFill).
Result tryFill(con::MovementContext& ctx, BlockSource& region, BlockPos const& pos, Settings const& s) {
    if (!region.hasChunksAt(pos, 0, false)) return Result::Fail;
    auto const& existing = region.getBlock(pos);
    if (&existing == s.paving || existing.getTypeName() == s.paving->getTypeName() && game::blockId(existing) == game::blockId(*s.paving)) {
        return Result::Pass;
    }
    std::string id          = existing.getTypeName();
    bool        replaceable = existing.isAir() || existing.getBlockType().canBeBuiltOver(existing, region, pos) || id.ends_with("_leaves");
    if (!replaceable && hasCollision(region, pos)) return Result::Fail;
    if (isPortal(id)) return Result::Fail;
    if (!replaceable) return Result::Fail;
    std::string item = s.filter.getTypeName();
    auto got = ctx.contraption.storage().extractItem([&](ItemStack const& i) { return i.getTypeName() == item; }, 1, false);
    if (got.isNull() || got.mCount == 0) return Result::Fail;
    game::setBlock(region, pos, *s.paving);
    return Result::Success;
}

/// Create's paving loop from `anchor` down to rollerFillDepth. True when something was placed.
bool pave(con::MovementContext& ctx, GlobalPos const& anchor, Settings const& s) {
    if (!s.paving) return false;
    auto* region = regionOf(anchor.dim);
    if (!region) return false;
    int    depthLimit = config::get<Config>().rollerFillDepth;
    Result result     = Result::Fail;
    for (int depth = 0; depth <= depthLimit; ++depth) {
        std::vector<BlockPos> layer;
        int                   y = anchor.y - depth;
        if (s.mode == RollingMode::WideFill) {
            int r = (depth + 1) / 2;
            for (int dx = -r; dx <= r; ++dx)
                for (int dz = -r; dz <= r; ++dz)
                    if (std::abs(dx) + std::abs(dz) <= r) layer.push_back(BlockPos{anchor.x + dx, y, anchor.z + dz});
        } else {
            layer.push_back(BlockPos{anchor.x, y, anchor.z});
        }
        bool anySuccess = false, allFailed = true;
        for (auto const& p : layer) {
            auto r = tryFill(ctx, *region, p, s);
            if (r == Result::Success) anySuccess = true;
            if (r != Result::Fail) allFailed = false;
        }
        if (anySuccess) {
            result = s.paving->getBlockType().mFalling ? Result::Pass : Result::Success;
            if (result == Result::Success) break;
        } else if (!allFailed || depth == 0) {
            result = Result::Pass;
        } else {
            break;
        }
        if (s.mode == RollingMode::TunnelPave) break;
    }
    return result == Result::Success;
}

/// Roller drops: none for blocks that need a tool, otherwise each drop has a 50 % chance to go
/// into contraption storage; the rest is lost.
void rollerBreak(con::MovementContext& ctx, Block const& block, BlockPos const& pos) {
    auto* region = regionOf(ctx.contraption.dimension());
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;
    auto drops = dropsOf(*region, pos, block, ItemStack::EMPTY_ITEM());
    level->destroyBlock(*region, pos, false, BlockChangeContext{});
    for (auto const& d : drops) {
        if (level->getRandom().nextFloat() < .5f) ctx.contraption.storage().insertItem(d, false);
    }
}

Breaker const& breaker();

GlobalPos posOf(con::MovementContext const& ctx, Json const& j) {
    return {ctx.contraption.dimension(), j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

void startPaving(con::MovementContext& ctx, GlobalPos const& at, Settings const& s) {
    if (pave(ctx, at, s)) {
        ctx.data["WaitingTicks"] = 2;
        ctx.data["LastPos"]      = Json::array({at.x, at.y, at.z});
        ctx.stall                = true;
        logAction("roller paved below " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
    }
}

void visit(con::MovementContext& ctx, GlobalPos const& pos) {
    damageEntities(ctx, pos, true, true);
    auto* region = regionOf(pos.dim);
    if (!region) return;
    Settings s = settingsOf(ctx);
    // tunnel pave: clear the path above the paved block first (hardest block decides the time)
    std::vector<GlobalPos> candidates;
    if (s.mode == RollingMode::TunnelPave) {
        int start = hasStock(ctx, s) ? 0 : 1;
        for (int h = start; h <= 2; ++h) {
            GlobalPos   p{pos.dim, pos.x, pos.y + h, pos.z};
            auto        bp    = toBlockPos(p);
            auto const& block = region->getBlock(bp);
            if (h == 0 && s.paving && game::blockId(block) == game::blockId(*s.paving)) continue;
            if (rollerCanBreak(*region, bp, block)) candidates.push_back(p);
        }
    }
    if (candidates.empty()) {
        startPaving(ctx, pos, s);
        return;
    }
    auto hardest = std::max_element(candidates.begin(), candidates.end(), [&](GlobalPos const& a, GlobalPos const& b) {
        return region->getBlock(toBlockPos(a)).getBlockType().getDestroySpeed() < region->getBlock(toBlockPos(b)).getBlockType().getDestroySpeed();
    });
    Json others = Json::array();
    for (auto const& c : candidates)
        if (&c != &*hardest) others.push_back(Json::array({c.x, c.y, c.z}));
    ctx.data["ReferencePos"] = Json::array({pos.x, pos.y, pos.z});
    ctx.data["OtherBreaks"]  = others;
    startBreaking(ctx, breaker(), *hardest);
}

Breaker const& breaker() {
    static Breaker const b{
        [](con::MovementContext const& ctx) { return std::clamp(animationSpeed(ctx) / 500.f * 1.5f, 1.f / 128, 16.f); },
        rollerCanBreak,
        [](con::MovementContext& ctx, GlobalPos const&) {
            // the other blocks of the column go at once, then the reference position is paved
            auto* region = regionOf(ctx.contraption.dimension());
            if (!region || !ctx.data.contains("ReferencePos")) return;
            for (auto const& o : ctx.data.value("OtherBreaks", Json::array())) {
                auto        bp    = toBlockPos(posOf(ctx, o));
                auto const& block = region->getBlock(bp);
                if (rollerCanBreak(*region, bp, block)) rollerBreak(ctx, block, bp);
            }
            auto ref = posOf(ctx, ctx.data["ReferencePos"]);
            ctx.data.erase("ReferencePos");
            ctx.data.erase("OtherBreaks");
            startPaving(ctx, ref, settingsOf(ctx));
        },
        rollerBreak,
    };
    return b;
}

cc::Vec3d areaOffset(con::MovementContext const& ctx) {
    return cc::Vec3d::of(cc::offset(facingOf(ctx.states))) * .45 + cc::Vec3d{0, -2, 0};
}

void tick(con::MovementContext& ctx) {
    if (int w = ctx.data.value("WaitingTicks", 0); w > 0) {
        ctx.stall = true;
        ctx.data["WaitingTicks"] = w - 1;
        if (w - 1 > 0) return;
        ctx.data.erase("WaitingTicks");
        ctx.stall = false;
        if (ctx.data.contains("LastPos")) {
            auto last = posOf(ctx, ctx.data["LastPos"]);
            ctx.data.erase("LastPos");
            startPaving(ctx, last, settingsOf(ctx)); // keep filling the same column
            if (ctx.stall) return;
        }
    }
    tickBreaking(ctx, breaker());
    if (ctx.stall) return;
    if (auto p = newAreaPos(ctx, areaOffset(ctx))) visit(ctx, *p);
}

// ---- stationary settings ----------------------------------------------------------------------

/// The horizontal axis perpendicular to the facing (Create's clockwise axis): rollers in a row.
cc::Pos rowStep(cc::Dir facing) {
    switch (facing) {
    case cc::Dir::North:
    case cc::Dir::South: return {1, 0, 0};
    default: return {0, 0, 1};
    }
}

void copySettings(GlobalPos const& from, GlobalPos const& to) {
    auto const* src  = bd::get(from);
    Json        mode = src && src->contains("actors") ? (*src)["actors"].value("mode", 0) : 0;
    Json        filt = src && src->contains("filter") && (*src)["filter"].contains("filter") ? (*src)["filter"]["filter"] : Json();
    auto&       dst  = bd::edit(to);
    dst["actors"]["mode"] = mode;
    if (filt.is_null()) {
        if (dst.contains("filter")) dst["filter"].erase("filter");
    } else {
        dst["filter"]["filter"] = filt;
    }
}

/// Create's shareValuesToAdjacent: the settings go to the rollers in a row (up to 99 each way).
void share(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto facing = facingAt(*region, toBlockPos(pos));
    auto step   = rowStep(facing);
    for (int sign : {1, -1}) {
        for (int i = 1; i <= 99; ++i) {
            GlobalPos q{pos.dim, pos.x + step.x * i * sign, pos.y, pos.z + step.z * i * sign};
            auto      bp = toBlockPos(q);
            if (!region->hasChunksAt(bp, 0, false) || region->getBlock(bp).getTypeName() != kRoller || facingAt(*region, bp) != facing) break;
            copySettings(pos, q);
        }
    }
}

struct Registrar {
    Registrar() {
        con::MovementBehaviour b;
        b.isActive = [](con::MovementContext const& ctx) {
            if (ctx.contraption.kind() == "pulley") return false;
            return pointingTowards(relativeMotion(ctx), facingOf(ctx.states));
        };
        b.tick       = tick;
        b.stopMoving = [](con::MovementContext& ctx) {
            cancelBreaking(ctx);
            ctx.data.erase("WaitingTicks");
            ctx.data.erase("LastPos");
        };
        registerOwnActor(std::string(kRoller), std::move(b));
        con::registerMovementRules(
            std::string(kRoller),
            con::MovementRules{
                nullptr,
                [](cc::States const&) { return true; },
                [](cc::States const& s) { return cc::bit(cc::opposite(facingOf(s))); },
                nullptr,
            }
        );
    }
};
Registrar const gRegistrar;

/// The two boxes on the roller's top face: the filter 3 px to one side of the centre, the
/// mode 3 px to the other, 3 px back from the centre (Create: voxel (8 ± 3, 15.5, 11) facing
/// south).
std::optional<core::common::Vec> topBox(Block const& block, int face, int side) {
    if (face != 1) return std::nullopt;
    auto    f    = static_cast<cc::Dir>(std::clamp(game::intState(block, kFacing).value_or(2), 0, 5));
    auto    back = cc::offset(cc::opposite(f));
    auto    row  = rowStep(f);
    double  x    = .5 + back.x * 3 / 16.0 + row.x * side * 3 / 16.0;
    double  z    = .5 + back.z * 3 / 16.0 + row.z * side * 3 / 16.0;
    return core::common::Vec{x, 1.0, z};
}

} // namespace

RollingMode rollerMode(GlobalPos const& pos) { return modeOfDoc(bd::get(pos)); }

void setRollerMode(GlobalPos const& pos, RollingMode mode, bool shareRow) {
    bd::edit(pos)["actors"]["mode"] = static_cast<int>(mode);
    if (shareRow) share(pos);
}

void rollerPlaced(GlobalPos const& pos) {
    // Create's searchForSharedValues: take the settings of a neighbouring roller in the row
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto facing = facingAt(*region, toBlockPos(pos));
    auto step   = rowStep(facing);
    for (int sign : {1, -1}) {
        GlobalPos q{pos.dim, pos.x + step.x * sign, pos.y, pos.z + step.z * sign};
        if (region->getBlock(toBlockPos(q)).getTypeName() != kRoller || facingAt(*region, toBlockPos(q)) != facing) continue;
        copySettings(q, pos);
        return;
    }
}

void rollerInstall() {
    vb::registerBox(
        std::string(kRoller),
        vb::BoxSpec{
            .id            = "mode",
            .label         = "create.contraptions.roller_mode",
            .labelFallback = "Roller Mode",
            .board         = [](Block const&) { return core::common::optionBoard(3, tr("create.contraptions.roller_mode", "Roller Mode")); },
            .centre        = [](Block const& b, int face) { return topBox(b, face, -1); },
            .scale         = 0.5,
            .get           = [](GlobalPos const& p) { return core::common::ValueSettings{0, static_cast<int>(rollerMode(p))}; },
            .set =
                [](GlobalPos const& p, core::common::ValueSettings s, Player&) {
                    setRollerMode(p, static_cast<RollingMode>(std::clamp(s.value, 0, 2)));
                    logAction("roller mode at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + " set to "
                              + std::to_string(s.value));
                },
            .format = [](Block const&, core::common::ValueSettings s) {
                int i = std::clamp(s.value, 0, 2);
                return tr(kModeKeys[i], kModeText[i]);
            },
        }
    );
    fs::registerSlot(
        std::string(kRoller),
        fs::SlotSpec{
            .id        = "filter",
            .centre    = [](Block const& b, int face) { return topBox(b, face, 1); },
            .scale     = 0.5,
            .onChanged = [](GlobalPos const& p) {
                // Create only accepts full-cube blocks without block entities (not stairs)
                auto f = fs::get(p, "filter");
                if (!it::isEmpty(f) && !common::filter::isFilterItem(f)) {
                    auto*        region = regionOf(p.dim);
                    Block const* b      = pavingBlockOf(f);
                    if (!b || (region && !fullCube(*region, toBlockPos(p), *b))) fs::set(p, ItemStack::EMPTY_ITEM(), "filter");
                }
                share(p);
                logAction("roller paving material at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + " set to "
                          + it::idOf(fs::get(p, "filter")));
            },
        }
    );
}

} // namespace lv::feature::actors
