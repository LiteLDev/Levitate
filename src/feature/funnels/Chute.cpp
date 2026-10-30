// Chutes and smart chutes (Create's ChuteBlock / SmartChuteBlock / ChuteBlockEntity,
// SPEC §3.6.12).
//
// A chute holds one stack that moves through it at Create's speed: gravity pulls items down at
// 4 blocks per second; encased fans at the top or bottom (airflow hook, FunnelsExt.h) push them
// up or down faster. Items come from the inventory above (below for upward chutes), from chutes
// feeding into this one, from items dropped on top, and leave into the inventory / chute below
// or drop out. Smart chutes filter, extract a configurable amount and stop when powered.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/advancements/Advancements.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

struct Runtime {
    bool      loaded  = false;
    ItemStack item;
    float     itemPos = 0;
    float     bottomPull     = 0;
    int       airCooldown    = 0;
    int       entityCooldown = 0;
    bool      canPickUp      = false;
};
std::unordered_map<std::string, Runtime>& runtimes() {
    static std::unordered_map<std::string, Runtime> m;
    return m;
}
/// push / pull of this tick (Create keeps them in the block entities and propagates changes).
struct Air {
    std::unordered_map<std::string, float> pull, push;
};
Air& air() {
    static Air a;
    return a;
}

/// A new world (the client loads several in one process) starts without cached state.
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldClosed([] { runtimes().clear(); });
    }
} const gLifecycle;

Runtime& rt(GlobalPos const& pos) {
    auto& r = runtimes()[pos.key()];
    if (!r.loaded) {
        r.loaded = true;
        r.item   = items::empty();
        if (auto const* d = docOf(pos)) {
            if (d->contains("item")) r.item = items::fromJson((*d)["item"]);
            r.itemPos = d->value("pos", 0.5f);
        }
    }
    return r;
}

Block const* chuteBlock(BlockSource& region, BlockPos const& p) {
    auto const* b = loadedBlock(region, p);
    return b && isChute(kindOf(b->getTypeName())) ? b : nullptr;
}

bool isSmart(Block const& b) { return b.getTypeName() == kSmartChute; }

/// `levitate:facing` of a chute runs 0..4 (Bedrock integer states are 0..n): 0 = down, 1..4 =
/// north / south / west / east (FacingID 2..5).
int facingState(int facing) { return cf::isHorizontal(facing) ? facing - 1 : 0; }
int chuteFacingOf(Block const& b) {
    int s = std::clamp(state(b, kFacing, 0), 0, 4);
    return s == 0 ? cf::kDown : s + 1;
}

/// Create's AbstractChuteBlock.getChuteFacing (-1 when not a chute).
int chuteFacing(BlockSource& region, BlockPos const& p) {
    auto const* b = chuteBlock(region, p);
    if (!b) return -1;
    if (isSmart(*b)) return cf::kDown;
    return chuteFacingOf(*b);
}

bool isOpenChute(Block const& b) {
    if (isSmart(b)) return true;
    return chuteFacingOf(b) == cf::kDown || state(b, kShape) == static_cast<int>(cf::ChuteShape::Intersection);
}

int funnelFacingAt(BlockSource& region, BlockPos const& p) {
    auto const* b = loadedBlock(region, p);
    if (!b || !isFunnel(kindOf(b->getTypeName()))) return -1;
    return state(*b, kFacing, cf::kUp);
}

/// Create's getTargetChute: the chute this one feeds into.
std::optional<BlockPos> targetChute(BlockSource& region, BlockPos const& p, int facing) {
    BlockPos t = offset(p, cf::kDown);
    if (cf::isHorizontal(facing)) t = offset(t, cf::oppositeFace(facing));
    if (!chuteBlock(region, t)) return std::nullopt;
    return t;
}

/// Create's getInputChutes: chutes feeding into this one (above facing down, above-diagonal
/// facing towards it).
std::vector<BlockPos> inputChutes(BlockSource& region, BlockPos const& p) {
    std::vector<BlockPos> out;
    for (int d : cf::kAllFaces) {
        if (d == cf::kDown) continue;
        int      dir = cf::oppositeFace(d);
        BlockPos c   = offset(p, cf::kUp);
        if (cf::isHorizontal(dir)) c = offset(c, dir);
        if (chuteFacing(region, c) == dir) out.push_back(c);
    }
    return out;
}

float pullAt(BlockSource& region, BlockPos const& p, int depth = 0);
float pushAt(BlockSource& region, BlockPos const& p, int depth = 0);

/// Create's calculatePull: a fan above blowing down (its speed), else the sum of the inputs.
float pullAt(BlockSource& region, BlockPos const& p, int depth) {
    auto key = game::globalPos(region, p).key();
    if (auto it = air().pull.find(key); it != air().pull.end()) return it->second;
    float total = 0;
    if (auto fan = common::airflow::fanAt(region, offset(p, cf::kUp)); fan && fan->facing == cf::kDown) {
        total = fan->speed;
    } else if (depth < 64) {
        for (auto const& in : inputChutes(region, p)) total += pullAt(region, in, depth + 1);
    }
    air().pull[key] = total;
    return total;
}

/// Create's calculatePush: a fan below blowing up, else the target chute's push shared among
/// its inputs.
float pushAt(BlockSource& region, BlockPos const& p, int depth) {
    auto key = game::globalPos(region, p).key();
    if (auto it = air().push.find(key); it != air().push.end()) return it->second;
    float total = 0;
    if (auto fan = common::airflow::fanAt(region, offset(p, cf::kDown)); fan && fan->facing == cf::kUp) {
        total = fan->speed;
    } else if (depth < 64) {
        if (auto t = targetChute(region, p, chuteFacing(region, p))) {
            auto branches = std::max<std::size_t>(1, inputChutes(region, *t).size());
            total         = pushAt(region, *t, depth + 1) / static_cast<float>(branches);
        }
    }
    air().push[key] = total;
    return total;
}

float motionAt(BlockSource& region, BlockPos const& p) { return cf::chuteItemMotion(pushAt(region, p), pullAt(region, p)); }

bool canActivate(Block const& b) { return !isSmart(b) || !state(b, kPowered); }

bool canAcceptItem(Block const& b, GlobalPos const& pos, Runtime& r, ItemStack const& stack) {
    if (!items::isEmpty(r.item)) return false;
    if (!isSmart(b)) return true;
    return canActivate(b) && common::filterslot::matches(pos, stack, "filter");
}

void setItem(GlobalPos const& pos, Runtime& r, ItemStack const& stack, float insertionPos) {
    r.item    = items::isEmpty(stack) ? items::empty() : stack;
    r.itemPos = insertionPos;
    auto& d   = editDoc(pos);
    if (items::isEmpty(r.item)) {
        d.erase("item");
        d.erase("pos");
    } else {
        d["item"] = items::toJson(r.item);
        d["pos"]  = insertionPos;
    }
    advancements::awardOwner(pos, "chute"); // Create's ChuteBlockEntity#setItem
}

/// Create's grabCapability: the inventory on a side; chutes don't count (they hand items over
/// themselves) except a smart chute below a downward chute.
bool hasCapability(BlockSource& region, BlockPos const& p, int side, float motion) {
    BlockPos n = offset(p, side);
    if (auto const* c = chuteBlock(region, n)) {
        if (side != cf::kDown || !isSmart(*c) || motion > 0) return false;
    }
    return common::items::has(region, n, cf::oppositeFace(side));
}

int extractionAmount(Block const& b, GlobalPos const& pos, BlockSource& region, float motion, bool& exact) {
    exact = false;
    if (!isSmart(b)) return 16;
    // Create shows the count only while the smart chute extracts from an inventory
    BlockPos    from     = offset(toBlockPos(pos), motion < 0 ? cf::kUp : cf::kDown);
    bool        extracts = !chuteBlock(region, from) && !replaceableAt(region, from);
    if (!extracts) return 64;
    auto s = amountOf(pos);
    exact  = !s.upTo;
    return s.count;
}

void handleInput(BlockSource& region, GlobalPos const& pos, Block const& b, Runtime& r, int side, float startLocation, float motion) {
    auto bp = toBlockPos(pos);
    if (!hasCapability(region, bp, side, motion) || !canActivate(b)) return;
    bool exact  = false;
    int  amount = extractionAmount(b, pos, region, motion, exact);
    auto pred   = [&](ItemStack const& s) { return canAcceptItem(b, pos, r, s); };
    auto got    = extractCounted(region, offset(bp, side), cf::oppositeFace(side), pred, amount, exact, false);
    if (!items::isEmpty(got)) setItem(pos, r, got, startLocation);
}

ItemActor* dropOut(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, float dy, float vy) {
    return spawnItem(region, stack, Vec3{pos.x + .5f, pos.y + .5f + dy, pos.z + .5f}, Vec3{0, vy, 0});
}

/// Create's handleDownwardOutput.
bool downwardOutput(BlockSource& region, GlobalPos const& pos, Block const& b, Runtime& r, bool simulate) {
    auto bp        = toBlockPos(pos);
    int  direction = isSmart(b) ? cf::kDown : chuteFacingOf(b);
    if (!canActivate(b)) return false;
    float motion = motionAt(region, bp);
    if (hasCapability(region, bp, cf::kDown, motion)) {
        auto rest = common::items::insert(region, offset(bp, cf::kDown), cf::kUp, r.item, simulate);
        int  held = items::countOf(r.item);
        if (!simulate) setItem(pos, r, rest, r.itemPos);
        if (items::countOf(rest) != held) return true;
        if (direction == cf::kDown) return false;
    }
    if (auto t = targetChute(region, bp, direction)) {
        auto  tpos = toGlobal(pos.dim, *t);
        auto& tr   = rt(tpos);
        auto const* tb = chuteBlock(region, *t);
        bool  canInsert = tb && canAcceptItem(*tb, tpos, tr, r.item);
        if (!simulate && canInsert) {
            setItem(tpos, tr, r.item, direction == cf::kDown ? 1.f : .51f);
            setItem(pos, r, items::empty(), 0);
        }
        return canInsert;
    }
    if (cf::isHorizontal(direction)) return false; // diagonal chutes can't drop items
    BlockPos below = offset(bp, cf::kDown);
    if (funnelFacingAt(region, below) == cf::kDown) return false;
    if (solidFace(region, below, cf::kUp)) return false;
    if (!simulate) {
        dropOut(region, pos, r.item, -12 / 16.f - .5f + .5f, -.25f);
        setItem(pos, r, items::empty(), 0);
    }
    return true;
}

/// Create's handleUpwardOutput.
bool upwardOutput(BlockSource& region, GlobalPos const& pos, Block const& b, Runtime& r, bool simulate) {
    auto bp = toBlockPos(pos);
    if (!canActivate(b)) return false;
    float motion = motionAt(region, bp);
    if (isOpenChute(b) && hasCapability(region, bp, cf::kUp, motion)) {
        int  before = items::countOf(r.item);
        auto rest   = common::items::insert(region, offset(bp, cf::kUp), cf::kDown, r.item, simulate);
        if (!simulate) setItem(pos, r, rest, r.itemPos);
        return items::countOf(rest) != before;
    }
    std::optional<BlockPos> best;
    float                   bestMotion = 0;
    auto                    inputs     = inputChutes(region, bp);
    for (auto const& in : inputs) {
        auto        ipos = toGlobal(pos.dim, in);
        auto const* ib   = chuteBlock(region, in);
        if (!ib || !canAcceptItem(*ib, ipos, rt(ipos), r.item)) continue;
        float m = motionAt(region, in);
        if (m < 0) continue;
        if (!best || bestMotion < m) {
            best       = in;
            bestMotion = m;
        }
    }
    if (best) {
        if (!simulate) {
            auto bpos = toGlobal(pos.dim, *best);
            setItem(bpos, rt(bpos), r.item, 0);
            setItem(pos, r, items::empty(), 0);
        }
        return true;
    }
    BlockPos above = offset(bp, cf::kUp);
    if (funnelFacingAt(region, above) == cf::kUp) return false;
    if (solidFace(region, above, cf::kDown)) return false;
    if (!inputs.empty()) return false;
    if (!simulate) {
        dropOut(region, pos, r.item, 8 / 16.f, motion * 2);
        setItem(pos, r, items::empty(), 0);
    }
    return true;
}

/// Create's canDirectlyInsert: items may come in from the top (belts, dropped items).
bool canDirectlyInsert(BlockSource& region, BlockPos const& p, Block const& b, float motion) {
    if (chuteFacing(region, offset(p, cf::kUp)) == cf::kDown) return false;
    if (motion > 0 && inputChutes(region, p).empty()) return false;
    return isOpenChute(b);
}

/// Create's updateAirFlow: how far below an upward chute items get sucked in.
void updateAirFlow(BlockSource& region, BlockPos const& p, Runtime& r, float motion) {
    if (motion <= 0) {
        r.bottomPull = 0;
        return;
    }
    float speed    = pullAt(region, p) - pushAt(region, p);
    float maxPull  = cf::chutePullDistance(speed);
    if (chuteBlock(region, offset(p, cf::kDown))) maxPull = 0;
    float limit = maxPull;
    for (int i = 1; i <= static_cast<int>(std::ceil(maxPull)); ++i) {
        if (!replaceableAt(region, offset(p, cf::kDown, i))) {
            limit = std::min(limit, static_cast<float>(i - 1));
            break;
        }
    }
    r.bottomPull = std::max(0.f, limit);
}

/// Create's findEntities: an upward chute sucks in items below it.
void findEntities(BlockSource& region, GlobalPos const& pos, Block const& b, Runtime& r, float motion) {
    if ((r.bottomPull <= 0 && !items::isEmpty(r.item)) || motion <= 0 || !canActivate(b) || r.bottomPull <= 0) return;
    Vec3 c{pos.x + .5f, pos.y + .5f, pos.z + .5f};
    Vec3 min{c.x - .45f, c.y - r.bottomPull - .5f - .45f, c.z - .45f};
    Vec3 max{c.x + .45f, c.y - .5f + .45f, c.z + .45f};
    for (auto* e : itemsIn(region, min, max)) {
        auto stack = e->item();
        if (!canAcceptItem(b, pos, r, stack)) continue;
        setItem(pos, r, stack, e->getPosition().y - static_cast<float>(pos.y));
        e->remove();
        break;
    }
}

/// Items dropped on top of an open chute fall in (Create's updateEntityAfterFallOn).
void pickUpFromTop(BlockSource& region, GlobalPos const& pos, Block const& b, Runtime& r) {
    if (!r.canPickUp || !items::isEmpty(r.item)) return;
    Vec3 min{float(pos.x), pos.y + .5f, float(pos.z)};
    Vec3 max{pos.x + 1.f, pos.y + 1.25f, pos.z + 1.f};
    for (auto* e : itemsIn(region, min, max)) {
        auto stack = e->item();
        auto rest  = common::items::insert(region, toBlockPos(pos), cf::kUp, stack, false);
        if (items::isEmpty(rest)) e->remove();
        else if (items::countOf(rest) < items::countOf(stack)) e->item() = rest;
        if (!items::isEmpty(r.item)) break;
    }
    (void)b;
}

common::items::Handler capability() {
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return 1; };
    h.getSlot = [](common::Access const& at, int) -> ItemStack {
        return rt(game::globalPos(at.region, at.pos)).item;
    };
    h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto        pos = game::globalPos(at.region, at.pos);
        auto const& b   = at.region.getBlock(at.pos);
        auto&       r   = rt(pos);
        if (items::isEmpty(stack) || !canAcceptItem(b, pos, r, stack)) return stack;
        // from the top only while Create's DirectBeltInputBehaviour would take it
        if (at.face == cf::kUp && !r.canPickUp) return stack;
        int max  = std::max(1, static_cast<int>(stack.getMaxStackSize()));
        int take = std::min(max, items::countOf(stack));
        if (!simulate) setItem(pos, r, items::withCount(stack, take), motionAt(at.region, at.pos) < 0 ? 1.f : 0.f);
        return items::withCount(stack, items::countOf(stack) - take);
    };
    h.extract = [](common::Access const& at, common::items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(at.region, at.pos);
        auto& r   = rt(pos);
        if (items::isEmpty(r.item) || amount <= 0 || !filter(r.item)) return items::empty();
        int take = std::min(amount, items::countOf(r.item));
        auto out = items::withCount(r.item, take);
        if (!simulate) setItem(pos, r, items::withCount(r.item, items::countOf(r.item) - take), r.itemPos);
        return out;
    };
    return h;
}

std::optional<std::array<double, 3>> smartFilterCentre(Block const&, int face) {
    if (!cf::isHorizontal(face)) return std::nullopt;
    return core::common::faceLocalPoint(face, 8, 11);
}
std::optional<std::array<double, 3>> smartAmountCentre(Block const&, int face) {
    if (!cf::isHorizontal(face)) return std::nullopt;
    return core::common::faceLocalPoint(face, 8, 5);
}

/// Shows the moving stack (Create's ChuteRenderer): at its height in the chute; in the lower
/// half of a diagonal chute it leans towards the chute it falls into.
void display(GlobalPos const& pos, Block const& b, Runtime const& r) {
    if (items::isEmpty(r.item)) {
        hideItem(pos);
        return;
    }
    float y  = std::clamp(r.itemPos, -3.f, 1.f);
    Vec3  at{pos.x + .5f, pos.y + y - .125f, pos.z + .5f};
    int   facing = isSmart(b) ? cf::kDown : chuteFacingOf(b);
    if (cf::isHorizontal(facing) && y < .5f) {
        auto o = cf::faceOffset(cf::oppositeFace(facing));
        at.x += o[0] * (.5f - y);
        at.z += o[2] * (.5f - y);
    }
    showItem(pos, r.item, at);
}

} // namespace

void beginChuteTick() {
    air().pull.clear();
    air().push.clear();
}

ItemStack chuteItem(GlobalPos const& pos) { return rt(pos).item; }

void tickChute(BlockSource& region, GlobalPos const& pos, Block const& blockIn, std::uint64_t tick) {
    auto         bp = toBlockPos(pos);
    Block const* b  = &blockIn;
    auto&        r  = rt(pos);
    if (isSmart(*b) && (tick + static_cast<std::uint64_t>(pos.x + pos.z)) % 2 == 0) {
        bool p = powered(region, bp);
        if (p != (state(*b, kPowered) != 0)) {
            setStates(region, bp, {{kPowered, p ? 1 : 0}});
            b = loadedBlock(region, bp);
            if (!b) return;
            log().info("funnels: smart chute at {} {}", pos.key(), p ? "powered" : "unpowered");
        }
    }
    float motion = motionAt(region, bp);
    r.canPickUp  = canDirectlyInsert(region, bp, *b, motion);

    if (r.airCooldown-- <= 0) {
        r.airCooldown = std::max(1, config().fanBlockCheckRate);
        updateAirFlow(region, bp, r, motion);
    }
    if (r.entityCooldown-- <= 0 && items::isEmpty(r.item)) {
        r.entityCooldown = 5;
        findEntities(region, pos, *b, r, motion);
    }
    pickUpFromTop(region, pos, *b, r);

    if (items::isEmpty(r.item)) {
        if (motion < 0) handleInput(region, pos, *b, r, cf::kUp, 1, motion);
        if (motion > 0) handleInput(region, pos, *b, r, cf::kDown, 0, motion);
        display(pos, *b, r);
        return;
    }
    float next = r.itemPos + motion;
    if (motion < 0) {
        if (next < .5f) {
            if (!downwardOutput(region, pos, *b, r, true)) next = .5f;
            else if (next < 0) {
                downwardOutput(region, pos, *b, r, false);
                next = r.itemPos;
            }
        }
    } else if (motion > 0) {
        if (next > .5f) {
            if (!upwardOutput(region, pos, *b, r, true)) next = .5f;
            else if (next > 1) {
                upwardOutput(region, pos, *b, r, false);
                next = r.itemPos;
            }
        }
    }
    r.itemPos = next;
    display(pos, *b, r);
}

bool useChute(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, ItemStack const& held) {
    auto& r = rt(pos);
    if (!items::isEmpty(held)) {
        if (items::idOf(held) != kIndustrialIronBlock || isSmart(block)) return false;
        auto shape = static_cast<cf::ChuteShape>(state(block, kShape, static_cast<int>(cf::ChuteShape::Normal)));
        if (shape == cf::ChuteShape::Intersection || shape == cf::ChuteShape::Encased) return false;
        setStates(region, toBlockPos(pos), {{kShape, static_cast<int>(cf::ChuteShape::Encased)}});
        playSound(region, toBlockPos(pos), "hit.netherite", .5f, 1.05f);
        log().info("funnels: chute at {} encased by {}", pos.key(), player.getRealName());
        return true;
    }
    if (items::isEmpty(r.item)) return false;
    giveOrDrop(player, r.item);
    log().info("funnels: {} took {} x{} out of the chute at {}", player.getRealName(), items::idOf(r.item), items::countOf(r.item), pos.key());
    setItem(pos, r, items::empty(), 0);
    return true;
}

bool wrenchChute(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block) {
    if (isSmart(block)) return true;
    auto shape = static_cast<cf::ChuteShape>(state(block, kShape, static_cast<int>(cf::ChuteShape::Normal)));
    bool down  = chuteFacingOf(block) == cf::kDown;
    if (shape == cf::ChuteShape::Intersection) return false;
    auto bp = toBlockPos(pos);
    if (shape == cf::ChuteShape::Encased) {
        setStates(region, bp, {{kShape, static_cast<int>(cf::ChuteShape::Normal)}});
        playSound(region, bp, "dig.netherite", 1.f, 1.f);
    } else if (down) {
        setStates(region, bp, {{kShape, static_cast<int>(shape != cf::ChuteShape::Normal ? cf::ChuteShape::Normal : cf::ChuteShape::Window)}});
        playSound(region, bp, "random.click", .5f, 1.2f);
    }
    log().info("funnels: chute at {} wrenched by {}", pos.key(), player.getRealName());
    return true;
}

void updateChuteShape(BlockSource& region, GlobalPos const& pos) {
    auto        bp = toBlockPos(pos);
    auto const* b  = chuteBlock(region, bp);
    if (!b || isSmart(*b)) return;
    int                 facing = chuteFacingOf(*b);
    auto                shape  = static_cast<cf::ChuteShape>(state(*b, kShape, static_cast<int>(cf::ChuteShape::Normal)));
    std::array<bool, 6> inputs{};
    for (int h : cf::kHorizontals) {
        BlockPos    c  = offset(offset(bp, cf::kUp), h);
        auto const* cb = chuteBlock(region, c);
        inputs[static_cast<std::size_t>(h)] = cb && !isSmart(*cb) && chuteFacingOf(*cb) == h;
    }
    bool targetIsChute = false;
    if (cf::isHorizontal(facing)) targetIsChute = chuteBlock(region, offset(offset(bp, cf::kDown), cf::oppositeFace(facing))) != nullptr;
    BlockPos above      = offset(bp, cf::kUp);
    bool     aboveDown  = chuteFacing(region, above) == cf::kDown || funnelFacingAt(region, above) == cf::kDown;
    auto [nf, ns]       = cf::updateChuteState(facing, shape, inputs, targetIsChute, aboveDown);
    setStates(region, bp, {{kFacing, facingState(nf)}, {kShape, static_cast<int>(ns)}});
}

void placeChute(Player* player, BlockSource& region, GlobalPos const& pos, int clickedFace, bool sneaking) {
    (void)player;
    auto        bp = toBlockPos(pos);
    auto const* b  = chuteBlock(region, bp);
    if (!b) return;
    if (isSmart(*b)) {
        setStates(region, bp, {{kPowered, powered(region, bp) ? 1 : 0}});
        return;
    }
    // Create: clicking the side of a block (not sneaking) makes the chute diagonal
    if (cf::isHorizontal(clickedFace) && !sneaking) setStates(region, bp, {{kFacing, facingState(clickedFace)}});
    updateChuteShape(region, pos);
}

void forgetChute(GlobalPos const& pos) {
    runtimes().erase(pos.key());
    hideItem(pos);
}

void installChutes() {
    for (char const* id : {kChute, kSmartChute}) {
        common::items::registerBlock(id, capability());
        // Create's DirectBeltInputBehaviour of a chute (onlyInsertWhen canDirectlyInsert): belt
        // ends, ejectors and tunnels put items in from any side while it can pick items up
        belts::registerDirectInput(
            id,
            belts::DirectInput{
                .canInsertFromSide = [](GlobalPos const& p, int) { return rt(p).canPickUp; },
                .insert =
                    [](GlobalPos const& p, belts::TransportedItem const& t, int side, bool simulate) {
                        auto* region = regionOf(p.dim);
                        if (!region) return t.stack;
                        return common::items::insert(*region, toBlockPos(p), cf::oppositeFace(side), t.stack, simulate);
                    },
            }
        );
    }
    // Create: holding the filter slot opens its count (the "Extracted Amount" board); the separate
    // amount box stays for vanilla players (SPEC §8)
    common::filterslot::registerSlot(
        kSmartChute,
        {"filter", smartFilterCentre, 0.5, nullptr, [](Player& p, GlobalPos const& pos) { common::valuebox::openEditor(p, pos, "amount"); }}
    );
    registerAmountBox(kSmartChute, smartAmountCentre);
    // `levitate:facing` of a chute is not in facing order (0 down, 1..4 north..east): undo the
    // common rotation, turn the real facing and keep the old one when it would leave the chute's
    // set (down or horizontal), as Create's horizontal-only states
    namespace ct = contraption;
    ct::registerRotationHandler(kChute, [](ct::cc::States& s, ct::cc::Axis axis, int quarter) {
        auto it = s.find(kFacing);
        if (it == s.end() || !std::holds_alternative<std::int64_t>(it->second)) return;
        auto v = std::get<std::int64_t>(it->second);
        if (v < 0 || v > 5) return;
        int  stored = static_cast<int>(ct::cc::rotate(static_cast<ct::cc::Dir>(v), axis, -quarter));
        if (stored > 4) return;
        auto before = stored == 0 ? ct::cc::Dir::Down : static_cast<ct::cc::Dir>(stored + 1);
        auto after  = ct::cc::rotate(before, axis, quarter);
        bool ok     = before == ct::cc::Dir::Down ? after == ct::cc::Dir::Down : ct::cc::axisOf(after) != ct::cc::Axis::Y;
        it->second  = std::int64_t{facingState(static_cast<int>(ok ? after : before))};
    });
}

} // namespace lv::feature::funnels::detail
