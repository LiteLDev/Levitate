// Tracks unit: what players do with tracks (select, connect, pave, break) and portal tracks.

#include <cmath>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/PortalForcer.h"
#include "mc/world/level/PortalRecord.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/storage/GameRuleId.h"
#include "mc/world/level/storage/GameRules.h"

#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::tracks {

namespace {

constexpr std::string_view kGirderId = "levitate:metal_girder";

std::string uuidOf(Player const& p) { return p.getUuid().asString(); }

ct::Vec3d lookOf(Player const& p) {
    Vec3 v = p.getViewVector();
    return {v.x, v.y, v.z};
}

bool doTileDrops() {
    auto level = ll::service::getLevel();
    if (!level) return true;
    GameRuleId id;
    id.mValue = int(GameRules::GameRulesIndex::DoTileDrops);
    return level->getGameRules().getBool(id, true);
}

ItemStack makeStack(std::string_view id, int count) {
    ItemStack s;
    s.reinit(id, count, 0);
    return s;
}

void spawnStack(BlockSource& region, ItemStack const& stack, ct::Vec3d const& at) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->getSpawner().spawnItem(region, stack, nullptr, Vec3{float(at.x), float(at.y), float(at.z)}, 0);
}

/// Gives `count` items to the player (Create's placeItemBackInInventory: what doesn't fit drops).
void giveItems(Player& player, std::string_view id, int count) {
    while (count > 0) {
        int       n     = std::min(64, count);
        ItemStack stack = makeStack(id, n);
        if (!player.add(stack) || !stack.isNull()) {
            Vec3 p = player.getPosition();
            spawnStack(player.getDimensionBlockSource(), stack, {p.x, p.y, p.z});
        }
        count -= 64;
    }
    player.sendInventory(false);
}

bool isFlower(Block const& b) {
    static std::set<std::string, std::less<>> const flowers = {
        "minecraft:poppy",        "minecraft:dandelion",     "minecraft:blue_orchid",       "minecraft:allium",
        "minecraft:azure_bluet",  "minecraft:red_tulip",     "minecraft:orange_tulip",      "minecraft:white_tulip",
        "minecraft:pink_tulip",   "minecraft:oxeye_daisy",   "minecraft:cornflower",        "minecraft:lily_of_the_valley",
        "minecraft:wither_rose",  "minecraft:torchflower",   "minecraft:pink_petals",       "minecraft:sunflower",
        "minecraft:lilac",        "minecraft:rose_bush",     "minecraft:peony",             "minecraft:red_flower",
        "minecraft:yellow_flower", "minecraft:closed_eyeblossom", "minecraft:open_eyeblossom", "minecraft:wildflowers",
    };
    return flowers.contains(b.getTypeName());
}

bool replaceableAt(BlockSource& region, ct::BlockPos3 const& p) { return game::replaceable(region, toGame(p)); }

bool canPlaceTrack(BlockSource& region, ct::BlockPos3 const& p) {
    BlockPos bp = toGame(p);
    if (!region.hasChunksAt(bp, 0, false)) return false;
    return game::replaceable(region, bp) || isFlower(region.getBlock(bp));
}

std::optional<ct::Shape> trackShapeAt(BlockSource& region, ct::BlockPos3 const& p) { return shapeAt(region, toGame(p)); }

bool turnOf(Block const& b) { return game::intState(b, kTurnState).value_or(0) != 0; }

// ---- paving (Create's TrackPaver) ---------------------------------------------------------------

struct PaveBlock {
    Block const* full   = nullptr; ///< the block (double slab for slabs)
    Block const* bottom = nullptr; ///< slabs: bottom / top halves
    Block const* top    = nullptr;
    bool         slab   = false;
    bool         wall   = false; ///< walls and metal girders: only along the sides
    bool         girder = false;
};

std::optional<PaveBlock> paveBlockOf(ItemStack const& offhand) {
    if (offhand.isNull() || !offhand.mBlock || offhand.mBlock->isAir()) return std::nullopt;
    Block const* b  = offhand.mBlock;
    std::string  id = b->getTypeName();
    if (id == kTrackId || id == kFakeTrackId) return std::nullopt;
    PaveBlock p;
    p.full   = b;
    p.girder = id == kGirderId;
    p.wall   = p.girder || id.ends_with("_wall");
    if (id.ends_with("_slab") && id.find("double") == std::string::npos) {
        std::string base = id.substr(0, id.size() - 5);
        Block const* dbl = game::blockWith(base + "_double_slab");
        if (!dbl) {
            auto colon = id.find(':');
            dbl        = game::blockWith(id.substr(0, colon + 1) + "double_" + id.substr(colon + 1));
        }
        if (dbl) {
            p.slab   = true;
            p.full   = dbl;
            p.bottom = Block::tryGetFromRegistry(HashedString(id), {{"minecraft:vertical_half", std::string("bottom")}}).as_ptr();
            p.top    = Block::tryGetFromRegistry(HashedString(id), {{"minecraft:vertical_half", std::string("top")}}).as_ptr();
            if (!p.bottom || !p.top) p.slab = false, p.full = b;
        }
    }
    return p;
}

bool placeIfFree(BlockSource& region, ct::BlockPos3 const& p, Block const& block, bool simulate) {
    BlockPos bp = toGame(p);
    if (!region.hasChunksAt(bp, 0, false)) return false;
    Block const& at = region.getBlock(bp);
    if (&at.getBlockType() == &block.getBlockType() || !game::replaceable(region, bp)) return false;
    if (!simulate) setBlockQuiet(region, p, block);
    return true;
}

int paveStraight(
    BlockSource&             region,
    ct::BlockPos3 const&     startPos,
    ct::Vec3d const&         direction,
    int                      extent,
    PaveBlock const&         pave,
    bool                     simulate,
    std::set<ct::BlockPos3>& visited
) {
    int                     needed = 0;
    std::set<ct::BlockPos3> toPlaceOn;
    ct::Vec3d               start          = startPos.center();
    ct::Vec3d               mainNormal     = direction.cross({0, 1, 0});
    ct::Vec3d               normalizedN    = mainNormal.normalize();
    ct::Vec3d               normalizedDir  = direction.normalize();
    double const            diagFiller     = 0.45;
    for (int i = 0; i < extent; ++i) {
        ct::Vec3d mainPos = start + direction * i;
        toPlaceOn.insert(ct::containing(mainPos + mainNormal));
        toPlaceOn.insert(ct::containing(mainPos - mainNormal));
        if (pave.wall) continue;
        toPlaceOn.insert(ct::containing(mainPos));
        if (i < extent - 1)
            for (int x : {1, -1}) toPlaceOn.insert(ct::containing(mainPos + normalizedN * (x * diagFiller) + normalizedDir * diagFiller));
        if (i > 0)
            for (int x : {1, -1}) toPlaceOn.insert(ct::containing(mainPos + normalizedN * (x * diagFiller) - normalizedDir * diagFiller));
    }
    for (auto const& p : toPlaceOn) {
        if (!visited.insert(p).second) continue;
        if (placeIfFree(region, p, *pave.full, simulate)) needed += pave.slab ? 2 : 1;
    }
    return needed;
}

int paveCurve(BlockSource& region, ct::Curve const& curve, PaveBlock const& pave, bool simulate, std::set<ct::BlockPos3>& visited) {
    if (pave.wall) return pave.girder ? ((curve.segmentCount() + 1) / 2) * 2 : 0;
    std::map<std::pair<int, int>, double> yLevels;
    ct::BlockPos3 const                   base = curve.block(0);
    int const                             segs = curve.segmentCount();
    for (auto const& seg : curve.segments()) {
        if (seg.index >= segs) break;
        // the middle of each segment (Create samples at t + 0.5 / segmentCount)
        double    t      = std::min(1.0, curve.segmentT(seg.index) + 0.5 / segs);
        ct::Vec3d result = curve.position(t) - base.corner() + ct::Vec3d{0, 3 / 16.0, 0};
        ct::Vec3d deriv  = curve.tangent(t);
        ct::Vec3d fn1 = curve.spec().normals[0], fn2 = curve.spec().normals[1];
        ct::Vec3d faceNormal = fn1 == fn2 ? fn1 : ct::slerp(t, fn1, fn2);
        ct::Vec3d normal     = faceNormal.cross(deriv).normalize();
        ct::Vec3d below      = result + faceNormal * -1.125;
        ct::Vec3d rail1      = below + normal * .97;
        ct::Vec3d rail2      = below - normal * .97;
        for (ct::Vec3d v : {rail1, rail2, (rail1 + rail2) * .5}) {
            ct::BlockPos3 p = ct::containing(v);
            auto          k = std::pair{p.x, p.z};
            auto          it = yLevels.find(k);
            if (it == yLevels.end() || it->second > v.y) yLevels[k] = v.y;
        }
    }
    int needed = 0;
    for (auto const& [xz, y] : yLevels) {
        int           floorY    = int(std::floor(y));
        bool          placeSlab = pave.slab && y - floorY >= .5;
        ct::BlockPos3 target{base.x + xz.first, base.y + floorY + (placeSlab ? 1 : 0), base.z + xz.second};
        Block const&  state = placeSlab ? *pave.bottom : *pave.full;
        if (!visited.insert(target).second) continue;
        // as Create's TrackPaver: a full block under a curve costs 2 items, a half slab 1
        if (placeIfFree(region, target, state, simulate)) needed += !placeSlab ? 2 : 1;
        if (placeSlab) {
            if (!visited.insert(target.below()).second) continue;
            if (placeIfFree(region, target.below(), *pave.top, simulate)) ++needed;
        }
    }
    return needed;
}

int pave(BlockSource& region, ct::PlacementPlan const& plan, PaveBlock const& p, bool simulate) {
    std::set<ct::BlockPos3> visited;
    int                     needed = 0;
    for (bool first : {true, false}) {
        int extent = (first ? plan.end1Extent : plan.end2Extent) + (plan.curve ? 1 : 0);
        needed += paveStraight(region, (first ? plan.pos1 : plan.pos2).below(), first ? plan.axis1 : plan.axis2, extent, p, simulate, visited);
    }
    if (plan.curve) needed += paveCurve(region, *plan.curve, p, simulate, visited);
    return needed;
}

// ---- requirements -------------------------------------------------------------------------------

int requiredTracks(BlockSource& region, int dim, ct::PlacementPlan const& plan) {
    int n = 0;
    for (auto const& piece : plan.straight)
        if (canPlaceTrack(region, piece.pos)) ++n;
    if (plan.curve && !state().curves.between(dim, plan.target1, plan.target2)) n += plan.curve->trackItemCost();
    return n;
}

/// Counts (and when `consume`, takes) items like Create: every slot but the selected one, then
/// the selected one, then the off hand.
struct Found {
    int tracks = 0, pavement = 0;
};
Found takeItems(Player& player, int tracks, std::string const& paveItem, int pavement, bool consume) {
    Found found;
    auto& inv      = player.getInventory();
    int   size     = inv.getContainerSize();
    int   selected = player.getSelectedItemSlot();
    for (int j = 0; j <= size + 1; ++j) {
        int  i       = j;
        bool offhand = j == size + 1;
        if (j == size) i = selected;
        else if (offhand) i = 0;
        else if (j == selected) continue;
        ItemStack stack = offhand ? player.getOffhandSlot() : inv.getItem(i);
        if (stack.isNull()) continue;
        std::string id      = stack.getTypeName();
        bool        isTrack = id == kTrackId;
        if (!isTrack && (paveItem.empty() || id != paveItem)) continue;
        if (isTrack ? found.tracks >= tracks : found.pavement >= pavement) continue;
        int count = stack.mCount;
        if (consume) {
            int take = std::min(isTrack ? tracks - found.tracks : pavement - found.pavement, count);
            stack.set(count - take);
            ItemStack rest = count - take > 0 ? stack : ItemStack::EMPTY_ITEM();
            if (offhand) player.setOffhandSlot(rest);
            else inv.setItem(i, rest);
        }
        (isTrack ? found.tracks : found.pavement) += count;
    }
    if (consume) player.sendInventory(false);
    return found;
}

// ---- placing ------------------------------------------------------------------------------------

/// Places the straight runs and the curve of a valid plan (Create's placeTracks, not simulated).
void placeTracks(BlockSource& region, int dim, ct::PlacementPlan const& plan, ct::Shape shape1, ct::Shape shape2) {
    for (auto const& piece : plan.straight) {
        BlockPos bp = toGame(piece.pos);
        if (!region.hasChunksAt(bp, 0, false)) continue;
        Block const& at = region.getBlock(bp);
        if (auto existing = shapeOf(at)) {
            ct::Shape combined = ct::overlayShape(*existing, piece.shape);
            if (combined != *existing)
                if (auto const* b = trackBlock(combined, turnOf(at))) game::setBlock(region, bp, *b);
            continue;
        }
        if (!canPlaceTrack(region, piece.pos)) continue;
        if (auto const* b = trackBlock(piece.shape, false)) game::setBlock(region, bp, *b);
    }
    if (!plan.curve) return;
    for (auto [target, shape] : {std::pair{plan.target1, shape1}, std::pair{plan.target2, shape2}}) {
        BlockPos     bp = toGame(target);
        Block const& at = region.getBlock(bp);
        auto         existing = shapeOf(at);
        if (auto const* b = trackBlock(existing ? *existing : ct::runShape(shape), true)) game::setBlock(region, bp, *b);
    }
    addCurve(dim, *plan.curve);
}

struct PendingConnection {
    std::string          uuid;
    int                  dim = 0;
    ct::PlacementRequest request;
};

void denyAndCancel(Player& player) { playSound(player, "levitate.tracks.deny", 1, 1); }

std::optional<ct::PlacementPlan> planFor(Player& player, BlockSource& region, ct::PlacementRequest& req, ct::Shape& shape1, ct::Shape& shape2) {
    auto& s   = state();
    auto  sel = s.selections.find(uuidOf(player));
    if (sel == s.selections.end()) return std::nullopt;
    int dim = int(player.getDimensionId());
    req.from = sel->second.end;
    auto s1  = sel->second.dim == dim ? trackShapeAt(region, req.from.pos) : std::nullopt;
    req.fromIsTrack = s1.has_value();
    shape1          = s1.value_or(ct::Shape::ZO);
    if (auto existing = trackShapeAt(region, req.pos2)) req.shape2 = *existing;
    req.pos2Tilted = tiltAt(dim, req.pos2).has_value(); // Create: "Cannot start connection from a Turn"
    shape2 = req.shape2;
    return ct::planConnection(req, shape1);
}

void executeConnection(PendingConnection const& pc) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Player* player = level->getPlayer(mce::UUID::fromString(pc.uuid));
    if (!player || int(player->getDimensionId()) != pc.dim) return;
    auto&                region = player->getDimensionBlockSource();
    ct::PlacementRequest req    = pc.request;
    ct::Shape            shape1{}, shape2{};
    auto                 plan = planFor(*player, region, req, shape1, shape2);
    if (!plan) return;
    if (!plan->valid) {
        if (!plan->message.empty()) actionbar(*player, plan->message, plan->message);
        denyAndCancel(*player);
        return;
    }
    ItemStack const offhand = player->getOffhandSlot();
    auto            paving  = paveBlockOf(offhand);
    int             tracks  = requiredTracks(region, pc.dim, *plan);
    int             blocks  = paving ? pave(region, *plan, *paving, true) : 0;
    if (!player->isCreative()) {
        std::string paveItem = paving ? offhand.getTypeName() : std::string();
        Found       found    = takeItems(*player, tracks, paveItem, blocks, false);
        if (found.tracks < tracks) {
            actionbar(*player, "create.track.not_enough_tracks", "Not holding enough tracks");
            denyAndCancel(*player);
            return;
        }
        if (found.pavement < blocks) {
            actionbar(*player, "create.track.not_enough_pavement", "Not holding enough pavement blocks");
            denyAndCancel(*player);
            return;
        }
        takeItems(*player, tracks, paveItem, blocks, true);
    }
    if (paving) pave(region, *plan, *paving, false);
    placeTracks(region, pc.dim, *plan, shape1, shape2);
    state().selections.erase(pc.uuid);
    syncSelection(*player);
    playSound(*player, "dig.stone", 1, 0.8f);
    if (plan->curve) {
        log().info(
            "tracks: {} connected {} to {} with a curve of length {:.2f} ({} tracks)",
            player->getRealName(),
            plan->pos1.str(),
            plan->pos2.str(),
            plan->curve->length(),
            tracks
        );
    } else {
        log().info("tracks: {} placed {} straight tracks from {} to {}", player->getRealName(), tracks, plan->pos1.str(), plan->pos2.str());
    }
}

} // namespace

// ---- messages -----------------------------------------------------------------------------------

void actionbar(Player& player, std::string const& key, std::string const& fallback) {
    SetTitlePacket pkt(SetTitlePacketPayload::TitleType::Actionbar, trFor(player, key, fallback), std::nullopt);
    pkt.sendTo(player);
}

void playSound(Player& player, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    Vec3  pos   = player.getPosition();
    pkt.mName   = name;
    pkt.mPos    = BlockPos{int(pos.x * 8), int(pos.y * 8), int(pos.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(player);
}

void clearSelection(Player& player) {
    state().selections.erase(uuidOf(player));
    syncSelection(player);
    actionbar(player, "create.track.selection_cleared", "Selection Cleared");
    playSound(player, "block.itemframe.remove_item", 0.75f, 1);
    log().info("tracks: {} cleared the track selection", player.getRealName());
}

// ---- the track item -----------------------------------------------------------------------------

bool onTrackItemUse(Player& player, BlockPos const& clicked, int face, Block const* clickedBlock) {
    auto&       s    = state();
    std::string uuid = uuidOf(player);
    int         dim  = int(player.getDimensionId());
    auto&       region = player.getDimensionBlockSource();

    // Bedrock repeats the use while the button is held: one action per click
    if (auto it = s.lastUse.find(uuid); it != s.lastUse.end() && s.tick - it->second < 5) {
        bool handled = s.selections.contains(uuid) || (clickedBlock && isTrack(*clickedBlock));
        if (handled) return true;
    }

    // Create's CurvedTrackInteraction: a track item used on a curve says so and places nothing
    if (clickedBlock && clickedBlock->getTypeName() == kFakeTrackId) {
        s.lastUse[uuid] = s.tick;
        actionbar(player, "create.track.turn_start", "Cannot start connection from a Turn");
        return true;
    }

    std::optional<ct::Shape> clickedShape = clickedBlock ? shapeOf(*clickedBlock) : std::nullopt;
    auto                     sel          = s.selections.find(uuid);

    if (sel == s.selections.end()) {
        if (!clickedShape) return false; // plain placement
        s.lastUse[uuid] = s.tick;
        if (ct::isJunction(*clickedShape)) {
            actionbar(player, "create.track.junction_start", "Cannot start connection from a Junction");
            return true;
        }
        if (tiltAt(dim, toCore(clicked))) { // Create's TrackBlockItem: a tilted track can't start one
            actionbar(player, "create.track.turn_start", "Cannot start connection from a Turn");
            return true;
        }
        Selection selection;
        selection.dim = dim;
        selection.end = ct::selectEnd(toCore(clicked), *clickedShape, lookOf(player));
        s.selections[uuid] = selection;
        syncSelection(player);
        playSound(player, "block.itemframe.add_item", 0.75f, 1);
        actionbar(player, "create.track.second_point", "Place track or select a second point");
        log().info("tracks: {} selected the track at {}", player.getRealName(), toCore(clicked).str());
        return true;
    }

    s.lastUse[uuid] = s.tick;
    if (player.isSneaking()) {
        clearSelection(player);
        return true;
    }

    ct::PlacementRequest req;
    ct::BlockPos3        pos2 = toCore(clicked);
    if (clickedShape) {
        req.shape2 = *clickedShape;
    } else {
        if (!replaceableAt(region, pos2)) {
            static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
            if (face >= 0 && face < 6) pos2 = pos2.offset(dx[face], dy[face], dz[face]);
        }
        if (!canPlaceTrack(region, pos2)) {
            denyAndCancel(player);
            return true;
        }
        Vec3 look   = player.getViewVector();
        auto sturdy = [&](int ddx, int ddz) {
            BlockPos side = toGame(pos2.offset(ddx, 0, ddz));
            BlockPos top  = toGame(pos2.offset(ddx, 1, ddz));
            if (!region.hasChunksAt(side, 0, false)) return false;
            return region.getBlock(side).canProvideFullSupport(1) && !region.getBlock(top).canProvideFullSupport(0);
        };
        req.shape2 = ct::placementShape({look.x, look.y, look.z}, player.getRotation().y, sturdy);
    }
    req.pos2         = pos2;
    req.look         = lookOf(player);
    req.maxLength    = config().maxTrackPlacementLength;
    req.maximiseTurn = maximiseTurn(player);
    req.girder       = player.getOffhandSlot().getTypeName() == kGirderId;

    ct::Shape shape1{}, shape2{};
    auto      plan = planFor(player, region, req, shape1, shape2);
    if (!plan) return true;
    if (!plan->message.empty()) actionbar(player, plan->message, plan->message);
    if (!plan->valid) {
        log().info("tracks: {} cannot connect {} to {}: {}", player.getRealName(), req.from.pos.str(), pos2.str(), plan->message);
        denyAndCancel(player);
        return true;
    }
    // world edits next tick (the cancelled use rolls back block changes made now)
    s.tasks.push_back([pc = PendingConnection{uuid, dim, req}] { executeConnection(pc); });
    return true;
}

// ---- curves -------------------------------------------------------------------------------------

void placeFakeTracks(int dim, ct::Curve const& curve) {
    auto* region = regionOf(dim);
    auto const* fake = game::blockWith(kFakeTrackId);
    if (!region || !fake) return;
    for (auto const& p : curve.occupiedBlocks()) {
        BlockPos bp = toGame(p);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        Block const& at = region->getBlock(bp);
        if (isFakeTrack(at)) continue;
        if (!game::replaceable(*region, bp)) continue;
        setBlockQuiet(*region, p, *fake);
    }
}

void removeFakeTracks(int dim, ct::Curve const& curve) {
    auto* region = regionOf(dim);
    auto const* air = game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (auto const& p : curve.occupiedBlocks()) {
        if (!state().curves.curvesThrough(dim, p).empty()) continue; // another curve still passes
        BlockPos bp = toGame(p);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        if (isFakeTrack(region->getBlock(bp))) setBlockQuiet(*region, p, *air);
    }
}

namespace {

void setTurn(int dim, ct::BlockPos3 const& pos, bool turn) {
    auto* region = regionOf(dim);
    if (!region) return;
    BlockPos bp = toGame(pos);
    if (!region->hasChunksAt(bp, 0, false)) return;
    Block const& at    = region->getBlock(bp);
    auto         shape = shapeOf(at);
    if (!shape || turnOf(at) == turn) return;
    if (auto const* b = trackBlock(*shape, turn)) setBlockQuiet(*region, pos, *b);
}

/// Store + client sync + deferred world edits (fake tracks, end states, drops).
void removeCurveData(int dim, ct::Curve const& curve, bool drops, Player* refundTo) {
    auto& s = state();
    // a tilted end loses one of its two sloped curves: the other one goes back to its shape
    // (Create's undoSmoothing), with the graph updated from the end as it was
    std::vector<std::pair<ct::BlockPos3, std::optional<ct::TrackBlockView>>> tilted;
    for (int i = 0; i < 2; ++i)
        if (tiltAt(dim, curve.block(i))) tilted.emplace_back(curve.block(i), world().trackAt(dim, curve.block(i)));
    auto removed = s.curves.remove(dim, curve.block(0), curve.block(1));
    if (!removed) return;
    for (auto const& [end, before] : tilted) updateTilt(dim, end, before);
    markCurveChunkDirty(dim, removed->block(0));
    syncCurveRemoved(dim, *removed);
    if (refundTo) {
        giveItems(*refundTo, kTrackId, removed->trackItemCost());
        if (removed->girderItemCost() > 0) giveItems(*refundTo, kGirderId, removed->girderItemCost());
        drops = false;
    }
    s.tasks.push_back([dim, c = *removed, drops] {
        removeFakeTracks(dim, c);
        for (int i = 0; i < 2; ++i)
            if (!state().curves.hasCurvesAt(dim, c.block(i))) setTurn(dim, c.block(i), false);
        if (!drops || !doTileDrops()) return;
        auto* region = regionOf(dim);
        if (!region) return;
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> off(-0.125, 0.125);
        int const segs = c.segmentCount();
        for (auto const& seg : c.segments()) {
            if (seg.index % 2 != 0 || seg.index == segs) continue;
            ct::Vec3d at = seg.position + ct::Vec3d{off(rng), 3 / 16.0 + off(rng), off(rng)};
            spawnStack(*region, makeStack(kTrackId, 1), at);
            if (c.girder()) spawnStack(*region, makeStack(kGirderId, 2), at);
        }
    });
}

} // namespace

void addCurve(int dim, ct::Curve const& curve) {
    auto& s        = state();
    auto  replaced = s.curves.add(dim, curve);
    if (replaced) {
        markCurveChunkDirty(dim, replaced->block(0));
        syncCurveRemoved(dim, *replaced);
    }
    markCurveChunkDirty(dim, curve.primary() ? curve.block(0) : curve.block(1));
    syncCurveAdded(dim, curve.primary() ? curve : curve.secondary());
    placeFakeTracks(dim, curve);
    setTurn(dim, curve.block(0), true);
    setTurn(dim, curve.block(1), true);
    s.railAdded.insert({dim, curve.block(0)});
    s.railAdded.insert({dim, curve.block(1)});
    // Create's tryApplySmoothing on both ends (and undoSmoothing when an end has a third curve)
    updateTilt(dim, curve.block(0));
    updateTilt(dim, curve.block(1));
}

void destroyCurve(int dim, ct::Curve const& curve, bool drops, Player* refundTo) {
    auto& s = state();
    // the graph walks the world: update it around both ends with and without the curve
    ct::BlockPos3 a        = curve.block(0);
    auto          viewA    = world().trackAt(dim, a);
    removeCurveData(dim, curve, drops, refundTo);
    if (viewA) {
        // the old view still holds the curve: removing it drops its far-end node too
        auto oldView = *viewA;
        s.rail.onRailRemoved(world(), dim, a, oldView);
        s.rail.onRailAdded(world(), dim, a);
    }
    s.railAdded.insert({dim, curve.block(1)});
}

void onTrackRemoved(int dim, ct::BlockPos3 const& pos, ct::Shape oldShape, bool drops) {
    auto& s       = state();
    auto  oldView = viewOf(dim, pos, oldShape);
    forgetTilt(dim, pos); // (its curves go below: nothing to put back)
    int   removed = 0;
    for (auto const& c : s.curves.curvesAt(dim, pos)) {
        removeCurveData(dim, *c, drops, nullptr);
        ++removed;
    }
    if (auto it = s.portals.find({dim, pos}); it != s.portals.end()) {
        PortalLink other = it->second;
        s.portals.erase(it);
        s.portals.erase({other.dim, other.pos});
        s.portalsDirty = true;
        s.tasks.push_back([other] {
            auto* region = regionOf(other.dim);
            auto const* air = game::blockWith("minecraft:air");
            if (!region || !air) return;
            if (shapeAt(*region, toGame(other.pos))) game::setBlock(*region, toGame(other.pos), *air);
        });
    }
    s.rail.onRailRemoved(world(), dim, pos, oldView);
    if (removed > 0) log().info("tracks: removed {} curve(s) with the track at {}", removed, pos.str());
}

void restoreFakeTracks(int dim, ct::BlockPos3 const& pos) {
    for (auto const& c : state().curves.curvesThrough(dim, pos)) placeFakeTracks(dim, *c);
}

// ---- portals ------------------------------------------------------------------------------------

namespace {

struct PortalExit {
    int           dim = 0;
    ct::BlockPos3 pos;  ///< where the other track goes
    ct::Dir2      face; ///< it faces the portal this way
};

std::optional<PortalExit> findExit(int dim, ct::BlockPos3 const& pos, ct::Dir2 d) {
    if (dim != 0 && dim != 1) return std::nullopt; // nether portals link the overworld and the nether
    int  otherDim = dim == 0 ? 1 : 0;
    auto level    = ll::service::getLevel();
    if (!level) return std::nullopt;
    ct::Vec3d     dv     = ct::dirVec(d);
    ct::BlockPos3 portal = pos.offset(int(dv.x), 0, int(dv.z));
    ct::BlockPos3 centre = otherDim == 1 ? ct::BlockPos3{int(std::floor(portal.x / 8.0)), portal.y, int(std::floor(portal.z / 8.0))}
                                         : ct::BlockPos3{portal.x * 8, portal.y, portal.z * 8};
    BlockPos target{};
    auto     record = level->getPortalForcer()._findPortal(DimensionType(otherDim), toGame(centre), otherDim == 0 ? 128 : 16, target);
    if (!record) return std::nullopt;
    BlockPos      base      = record->mBaseBlockPos;
    bool          axisX     = record->mXInc != 0;
    ct::Dir2      targetDir = d;
    bool          dAxisX    = d == ct::Dir2::East || d == ct::Dir2::West;
    if (dAxisX == axisX) targetDir = static_cast<ct::Dir2>((static_cast<int>(d) + 1) & 3); // clockwise
    ct::Vec3d     tv = ct::dirVec(targetDir);
    PortalExit    exit;
    exit.dim  = otherDim;
    exit.pos  = toCore(base).offset(int(tv.x), 0, int(tv.z));
    exit.face = ct::dirOpposite(targetDir);
    return exit;
}

void tellNearest(int dim, ct::BlockPos3 const& pos, std::string const& reasonKey, std::string const& reasonFallback, std::optional<ct::BlockPos3> at) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Player*   nearest = nullptr;
    double    best    = 10 * 10;
    ct::Vec3d centre  = pos.center();
    level->forEachPlayer([&](Player& p) {
        if (int(p.getDimensionId()) != dim) return true;
        Vec3   v = p.getPosition();
        double d = ct::Vec3d{v.x, v.y, v.z}.distanceToSqr(centre);
        if (d <= best) {
            best    = d;
            nearest = &p;
        }
        return true;
    });
    if (!nearest) return;
    nearest->sendMessage("§6<!> " + trFor(*nearest, "create.portal_track.failed", "Cannot place portal track:"));
    std::string reason = at ? trFor(*nearest, reasonKey, reasonFallback, at->x, at->y, at->z) : trFor(*nearest, reasonKey, reasonFallback);
    nearest->sendMessage("§7 - §e" + reason);
}

void connectToPortal(int dim, ct::BlockPos3 const& pos, ct::Shape shape) {
    if (shape != ct::Shape::XO && shape != ct::Shape::ZO) return;
    auto& s      = state();
    auto* region = regionOf(dim);
    if (!region || !s.portalType) return;
    std::vector<ct::Dir2> dirs = shape == ct::Shape::XO ? std::vector{ct::Dir2::East, ct::Dir2::West}
                                                        : std::vector{ct::Dir2::South, ct::Dir2::North};
    bool                         pop = false;
    std::string                  fail;
    std::optional<ct::BlockPos3> failPos;
    for (auto d : dirs) {
        ct::Vec3d dv = ct::dirVec(d);
        BlockPos  pp = toGame(pos.offset(int(dv.x), 0, int(dv.z)));
        if (!region->hasChunksAt(pp, 0, false) || &region->getBlock(pp).getBlockType() != s.portalType) continue;
        pop       = true;
        auto exit = findExit(dim, pos, d);
        auto* otherRegion = exit ? regionOf(exit->dim) : nullptr;
        if (!exit || !otherRegion || !otherRegion->hasChunksAt(toGame(exit->pos), 0, false)) {
            fail = "missing";
            continue;
        }
        if (!game::replaceable(*otherRegion, toGame(exit->pos))) {
            fail    = "blocked";
            failPos = exit->pos;
            continue;
        }
        auto const* here  = trackBlock(ct::asPortal(d), true);
        auto const* there = trackBlock(ct::asPortal(exit->face), true);
        if (!here || !there) return;
        s.portals[{dim, pos}]             = PortalLink{exit->dim, exit->pos};
        s.portals[{exit->dim, exit->pos}] = PortalLink{dim, pos};
        s.portalsDirty                    = true;
        game::setBlock(*region, toGame(pos), *here);
        game::setBlock(*otherRegion, toGame(exit->pos), *there);
        log().info("tracks: portal track linked {}:{} with {}:{}", dim, pos.str(), exit->dim, exit->pos.str());
        pop = false;
    }
    // (as Create, `pop` is per direction: a portal on both sides where only one links still pops)
    if (!pop) return;
    // Create pops the track (with its drop)
    auto const* air = game::blockWith("minecraft:air");
    if (air) {
        s.noDrops.insert({dim, pos});
        game::setBlock(*region, toGame(pos), *air);
        if (doTileDrops()) spawnStack(*region, makeStack(kTrackId, 1), pos.center());
    }
    if (fail.empty()) return;
    if (fail == "missing") tellNearest(dim, pos, "create.portal_track.missing", "Target portal not generated yet", std::nullopt);
    else tellNearest(dim, pos, "create.portal_track.blocked", "Target location blocked ({},{},{})", failPos);
    log().info("tracks: portal track at {} failed ({})", pos.str(), fail);
}

} // namespace

void onTrackPlaced(int dim, ct::BlockPos3 const& pos) {
    auto* region = regionOf(dim);
    if (!region) return;
    auto shape = shapeAt(*region, toGame(pos));
    if (!shape) return;
    state().rail.onRailAdded(world(), dim, pos);
    if (!ct::isPortal(*shape)) connectToPortal(dim, pos, *shape);
}

void onPortalRemoved(int dim, ct::BlockPos3 const& portalPos) {
    auto* region = regionOf(dim);
    auto const* air = game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int i = 0; i < 4; ++i) {
        auto          d  = static_cast<ct::Dir2>(i);
        ct::Vec3d     dv = ct::dirVec(d);
        ct::BlockPos3 q  = portalPos.offset(-int(dv.x), 0, -int(dv.z));
        auto          sh = shapeAt(*region, toGame(q));
        if (sh && *sh == ct::asPortal(d)) {
            state().noDrops.insert({dim, q});
            game::setBlock(*region, toGame(q), *air);
        }
    }
}

} // namespace lv::feature::tracks
