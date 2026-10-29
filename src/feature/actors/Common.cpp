// Actors unit (U22): shared helpers (Create's MovementBehaviour / BlockBreakingMovementBehaviour
// parts the actors reuse), config, and game adapters.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/util/Random.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ResourceDrops.h"
#include "mc/world/level/block/ResourceDropsContext.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/actors/Actors.h"
#include "feature/common/Items.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

LV_REGISTER_CONFIG("actors", lv::feature::actors::Config);

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::ext;

// ---- blocks -----------------------------------------------------------------------------------

std::optional<int> seatColour(std::string_view id) {
    for (int i = 0; i < 16; ++i) {
        if (id.size() == 9 + kColours[i].size() + 5 && id.starts_with("levitate:") && id.ends_with("_seat")
            && id.substr(9, kColours[i].size()) == kColours[i]) {
            return i;
        }
    }
    return std::nullopt;
}
bool        isSeat(std::string_view id) { return seatColour(id).has_value(); }
std::string seatId(int colour) { return "levitate:" + std::string(kColours[std::clamp(colour, 0, 15)]) + "_seat"; }

cc::Dir facingOf(cc::States const& states, cc::Dir fallback) {
    auto it = states.find(kFacing);
    if (it == states.end()) return fallback;
    if (auto const* v = std::get_if<std::int64_t>(&it->second); v && *v >= 0 && *v <= 5) return static_cast<cc::Dir>(*v);
    return fallback;
}

cc::Dir facingAt(BlockSource& region, BlockPos const& pos, cc::Dir fallback) {
    auto v = game::intState(region.getBlock(pos), kFacing);
    if (!v || *v < 0 || *v > 5) return fallback;
    return static_cast<cc::Dir>(*v);
}

BlockPos toBlockPos(cc::Pos const& p) { return BlockPos{p.x, p.y, p.z}; }
BlockPos toBlockPos(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
cc::Pos  toPos(BlockPos const& p) { return {p.x, p.y, p.z}; }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

std::vector<Actor*> entitiesIn(BlockSource& region, cc::Vec3d const& lo, cc::Vec3d const& hi) {
    AABB                box{Vec3{float(lo.x), float(lo.y), float(lo.z)}, Vec3{float(hi.x), float(hi.y), float(hi.z)}};
    std::vector<Actor*> out;
    for (auto* a : region.getEntities(box, 0.0f))
        if (a && !a->isPlayer()) out.push_back(a);
    if (auto level = ll::service::getLevel()) {
        int dim = static_cast<int>(region.getDimensionId());
        level->forEachPlayer([&](Player& p) {
            if (static_cast<int>(p.getDimensionId()) != dim) return true;
            auto const& b = p.getAABB();
            if (b.max.x > lo.x && b.min.x < hi.x && b.max.y > lo.y && b.min.y < hi.y && b.max.z > lo.z && b.min.z < hi.z) out.push_back(&p);
            return true;
        });
    }
    return out;
}

bool recentlyUsed(std::int64_t key) {
    static std::unordered_map<std::int64_t, std::chrono::steady_clock::time_point> last;
    auto now = std::chrono::steady_clock::now();
    auto it  = last.find(key);
    if (it != last.end() && now - it->second < std::chrono::milliseconds(250)) return true;
    last[key] = now;
    return false;
}

bool setIntState(BlockSource& region, BlockPos const& pos, std::string const& state, int value) {
    auto const& block = region.getBlock(pos);
    auto        cur   = game::intState(block, state);
    if (!cur) return false;
    if (*cur == value) return true;
    auto const* next = ext::withStates(block, cc::States{{state, std::int64_t{value}}});
    if (!next || next == &block) return false;
    return game::setBlock(region, pos, *next);
}

void logAction(std::string const& text) { log().info("actors: {}", text); }

// ---- items ------------------------------------------------------------------------------------

ItemStack makeItem(std::string const& id, int count) {
    auto s = common::items::make(id, count);
    return s ? *s : ItemStack::EMPTY_ITEM();
}

void spawnItem(BlockSource& region, ItemStack const& item, cc::Vec3d const& pos, cc::Vec3d const& velocity) {
    if (item.isNull() || item.mCount == 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    auto* entity = level->getSpawner().spawnItem(region, item, nullptr, Vec3{float(pos.x), float(pos.y), float(pos.z)}, 0);
    if (!entity) return;
    entity->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{float(velocity.x), float(velocity.y), float(velocity.z)};
}

std::vector<ItemStack> dropsOf(BlockSource& region, BlockPos const& pos, Block const& block, ItemStack const& tool) {
    std::vector<ItemStack> out;
    auto                   level = ll::service::getLevel();
    if (!level) return out;
    ResourceDropsContext ctx{
        ResourceDropsCause::PlayerMining,
        0.f,
        tool,
        pos,
        region.getDimensionId(),
        region,
    };
    auto drops = block.getBlockType().getResourceDrops(block, level->getIRandom(), ctx);
    for (auto& s : drops.mItems.get())
        if (!s.isNull() && s.mCount > 0) out.push_back(s);
    return out;
}

// ---- actor helpers ----------------------------------------------------------------------------

cc::Vec3d worldFacing(con::MovementContext const& ctx) {
    auto local = cc::Vec3d::of(cc::offset(facingOf(ctx.states)));
    return ctx.contraption.transform().rotateVector(local);
}

cc::Vec3d relativeMotion(con::MovementContext const& ctx) { return ctx.contraption.transform().unrotateVector(ctx.motion); }

bool pointingTowards(cc::Vec3d const& vec, cc::Dir dir) {
    double len = vec.length();
    if (len < 1e-9) return false;
    cc::Vec3d n = vec * (1.0 / len);
    return (cc::Vec3d::of(cc::offset(dir)) - n).length() < .75;
}

float animationSpeed(con::MovementContext const& ctx) {
    double length = ctx.motion.length();
    if (length < 1.0 / 512) return 0;
    return static_cast<float>(static_cast<int>(length * 1000 + 100) / 100 * 100);
}

cc::Vec3d areaPoint(con::MovementContext const& ctx, cc::Vec3d const& localOffset) {
    return ctx.position + ctx.contraption.transform().rotateVector(localOffset);
}

std::optional<GlobalPos> newAreaPos(con::MovementContext& ctx, cc::Vec3d const& localOffset, char const* key) {
    auto p = areaPoint(ctx, localOffset);
    GlobalPos gp{ctx.contraption.dimension(), int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))};
    Json      j = Json::array({gp.x, gp.y, gp.z});
    if (ctx.data.contains(key) && ctx.data[key] == j) return std::nullopt;
    ctx.data[key] = j;
    return gp;
}

void collectOrDrop(con::MovementContext& ctx, ItemStack const& stack) {
    if (stack.isNull() || stack.mCount == 0) return;
    ItemStack rest = stack;
    if (config::get<Config>().moveItemsToStorage) rest = ctx.contraption.storage().insertItem(stack, false);
    if (rest.isNull() || rest.mCount == 0) return;
    auto* region = regionOf(ctx.contraption.dimension());
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;
    float     r = level->getRandom().nextFloat() * .3f;
    cc::Vec3d v = (ctx.motion + cc::Vec3d{0, .5, 0}) * r;
    spawnItem(*region, rest, ctx.position, v);
}

namespace {

bool isContraptionDisplay(Actor const& a) { return a.getEntityTypeId() == ActorType::FallingBlock; }

void addVelocity(Actor& a, cc::Vec3d const& v) {
    Vec3& delta = *a.mBuiltInComponents->mStateVectorComponent->mPosDelta;
    delta       = Vec3{delta.x + float(v.x), delta.y + float(v.y), delta.z + float(v.z)};
    if (a.isPlayer()) {
        // players move themselves: tell their client (Create throws players client side)
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
        if (!packet) return;
        auto& pkt       = static_cast<SetActorMotionPacket&>(*packet);
        pkt.mRuntimeId  = a.getRuntimeID();
        pkt.mMotion     = delta;
        pkt.sendTo(static_cast<Player&>(a));
    }
}

} // namespace

void damageEntities(con::MovementContext& ctx, GlobalPos const& pos, bool hurt, bool roller) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto bp = toBlockPos(pos);
    // Create: a redstone conductor (a full solid block) in that space protects what is inside
    auto const& here = region->getBlock(bp);
    if (!here.isAir() && here._isSolid() && hasCollision(*region, bp)) return;
    double speed = relativeMotion(ctx).length();
    cc::Vec3d lo = cc::Vec3d::of(cc::Pos{pos.x, pos.y, pos.z});
    for (auto* a : entitiesIn(*region, lo, lo + cc::Vec3d{1, 1, 1})) {
        if (!a || !a->isAlive() || isContraptionDisplay(*a) || a->getEntityTypeId() == ActorType::ItemEntity) continue;
        if (a->getTypeName() == kSeatEntity) continue;
        if (hurt) {
            float damage = std::clamp(6.f * static_cast<float>(std::pow(speed, 0.4)) + 1.f, 2.f, 10.f);
            a->hurtByCause(damage, roller ? SharedTypes::Legacy::ActorDamageCause::Contact : SharedTypes::Legacy::ActorDamageCause::Override);
        }
        // Create: thrown by the motion plus a quarter of its length upwards, at most 4 blocks/tick
        cc::Vec3d v = ctx.motion + cc::Vec3d{0, ctx.motion.length() / 4, 0};
        if (v.length() > 4) v = v * (4 / v.length());
        addVelocity(*a, v);
    }
}

// ---- breaking (Create's BlockBreakingMovementBehaviour) ---------------------------------------

bool isLiquid(std::string const& id) {
    return id == "minecraft:water" || id == "minecraft:flowing_water" || id == "minecraft:lava" || id == "minecraft:flowing_lava"
        || id == "minecraft:bubble_column";
}

bool isBreakable(Block const& block) {
    if (block.isAir()) return false;
    if (isLiquid(block.getTypeName())) return false;
    return block.getBlockType().getDestroySpeed() >= 0;
}

bool hasCollision(BlockSource& region, BlockPos const& pos) {
    auto const& block = region.getBlock(pos);
    if (block.isAir()) return false;
    AABB box = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
    return box.max.x - box.min.x > 1e-4f && box.max.y - box.min.y > 1e-4f && box.max.z - box.min.z > 1e-4f;
}

void destroyAndCollect(con::MovementContext& ctx, Block const& block, BlockPos const& pos) {
    auto* region = regionOf(ctx.contraption.dimension());
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;
    auto drops = dropsOf(*region, pos, block, ItemStack::EMPTY_ITEM());
    level->destroyBlock(*region, pos, false, BlockChangeContext{});
    for (auto const& s : drops) collectOrDrop(ctx, s);
}

namespace {
Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }
} // namespace

bool startBreaking(con::MovementContext& ctx, Breaker const& b, GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    auto bp = toBlockPos(pos);
    if (!region->hasChunksAt(bp, 0, false)) return false;
    auto const& block = region->getBlock(bp);
    if (!isBreakable(block) || !b.canBreak(*region, bp, block)) return false;
    ctx.data["BreakingPos"]  = posJson(pos);
    ctx.data["Progress"]     = 0;
    ctx.data["TicksUntil"]   = 0;
    ctx.stall                = true;
    return true;
}

void cancelBreaking(con::MovementContext& ctx) {
    ctx.data.erase("BreakingPos");
    ctx.data.erase("Progress");
    ctx.data.erase("TicksUntil");
    ctx.stall = false;
}

void tickBreaking(con::MovementContext& ctx, Breaker const& b) {
    if (!ctx.data.contains("BreakingPos")) return;
    auto const& j = ctx.data["BreakingPos"];
    GlobalPos   pos{ctx.contraption.dimension(), j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    // a break left over from before the actor was disabled / turned around: the block is no
    // longer in its reach
    if ((cc::Vec3d::center(cc::Pos{pos.x, pos.y, pos.z}) - ctx.position).length() > 3.5) {
        cancelBreaking(ctx);
        return;
    }
    auto*       region = regionOf(pos.dim);
    if (!region) return;
    auto        bp    = toBlockPos(pos);
    auto const& block = region->getBlock(bp);
    if (!isBreakable(block) || !b.canBreak(*region, bp, block)) {
        cancelBreaking(ctx);
        return;
    }
    ctx.stall = true;
    // Create: only while the contraption tries to move
    if (ctx.motion.lengthSqr() < 1e-12) return;
    int until = ctx.data.value("TicksUntil", 0);
    if (until > 0) {
        ctx.data["TicksUntil"] = until - 1;
        return;
    }
    float speed    = b.speed(ctx);
    float hardness = block.getBlockType().getDestroySpeed();
    int   progress = ctx.data.value("Progress", 0);
    int   step     = hardness <= 0 ? 10 - progress : std::clamp(static_cast<int>(speed / hardness), 1, 10 - progress);
    progress += step;
    ext::playSound(pos.dim, cc::Vec3d::center(cc::Pos{pos.x, pos.y, pos.z}), "hit.stone", .25f, 1.f);
    if (progress >= 10) {
        cancelBreaking(ctx);
        if (b.breakBlock) b.breakBlock(ctx, block, bp);
        else destroyAndCollect(ctx, block, bp);
        if (b.onBroken) b.onBroken(ctx, pos);
        return;
    }
    ctx.data["Progress"]   = progress;
    ctx.data["TicksUntil"] = hardness <= 0 ? 0 : static_cast<int>(hardness / speed);
}

} // namespace lv::feature::actors
