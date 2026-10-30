// Mechanical drill and saw as contraption actors (Create 6.0.10's BlockBreakingMovementBehaviour,
// DrillMovementBehaviour, SawMovementBehaviour), see docs/SPEC.md §3.6.

#include <cmath>
#include <random>

#include "nlohmann/json.hpp"

#include "mc/world/actor/Actor.h"
#include "mc/world/level/block/Block.h"

#include "core/crafter/Breaking.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/crafter/Internal.h"
#include "mod/Log.h"

namespace lv::feature::crafter {

namespace {

namespace k  = core::kinetics;
namespace cc = core::contraption;
namespace ct = feature::contraption;
namespace ci = common::items;
using Json   = nlohmann::json;

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
double rand01() { return std::uniform_real_distribution<double>(0, 1)(rng()); }

Face localFacing(ct::MovementContext const& ctx) {
    auto it = ctx.states.find(kFacing);
    if (it == ctx.states.end()) return Face::Up;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    return static_cast<Face>(std::clamp<std::int64_t>(v ? *v : 1, 0, 5));
}

/// The actor's facing as a world vector (the contraption may be turned).
cc::Vec3d worldFacing(ct::MovementContext const& ctx) {
    auto o     = k::offsetOf(localFacing(ctx));
    auto local = cc::Vec3d::center(ctx.localPos);
    auto a     = ctx.contraption.toWorld(local);
    auto b     = ctx.contraption.toWorld(local + cc::Vec3d{double(o.x), double(o.y), double(o.z)});
    return b - a;
}

/// VecHelper.isVecPointingTowards: |normal - normalize(vec)| < .75.
bool pointingTowards(cc::Vec3d const& vec, cc::Vec3d const& dir) {
    double len = vec.length();
    if (len < 1e-9) return false;
    cc::Vec3d n = vec * (1.0 / len);
    return (dir - n).length() < .75;
}

GlobalPos blockOf(int dim, cc::Vec3d const& p) {
    return {dim, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))};
}

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }
GlobalPos posFrom(int dim, Json const& j) { return {dim, j[0].get<int>(), j[1].get<int>(), j[2].get<int>()}; }

bool isSawActor(std::string const& kind) { return kind == "saw"; }

bool canBreak(std::string const& kind, GlobalPos const& pos) {
    auto const* b = world::blockAt(pos);
    if (!b || !world::breakable(*b)) return false;
    if (isSawActor(kind)) return trees::sawable(*b);
    // drills: no blocks without collision, no tracks (Create's DrillMovementBehaviour.canBreak)
    std::string_view id = b->getTypeName();
    if (id == "levitate:track" || id == "levitate:fake_track") return false;
    return world::hasCollision(pos);
}

/// MovementBehaviour.dropItem: into the contraption's storage first (moveItemsToStorage).
void dropItem(ct::MovementContext& ctx, ItemStack const& stack) {
    ItemStack rest = stack;
    if (config().moveItemsToStorage) rest = ctx.contraption.storage().insertItem(stack, false);
    if (ci::isEmpty(rest)) return;
    double s = rand01() * .3;
    world::dropItem(ctx.worldPos.dim, ctx.position.x, ctx.position.y, ctx.position.z, rest,
                    std::array<double, 3>{ctx.motion.x * s, (ctx.motion.y + .5) * s, ctx.motion.z * s});
}

void damageEntities(ct::MovementContext& ctx, std::string const& kind, GlobalPos const& pos) {
    double damage = cr::movingDamage(ctx.motion.length());
    for (auto* a : world::entitiesIn(pos.dim, pos.x, pos.y, pos.z, pos.x + 1., pos.y + 1., pos.z + 1.)) {
        if (world::isItemEntity(*a)) continue;
        if (a->getEntityTypeId() == ActorType::FallingBlock) continue; // the contraption's own display blocks
        world::hurt(*a, static_cast<float>(damage), isSawActor(kind) ? kSawDeath : kDrillDeath);
        // throwEntity: the motion plus a lift, at most 4 blocks/tick
        cc::Vec3d boost = ctx.motion + cc::Vec3d{0, ctx.motion.length() / 4, 0};
        double    len   = boost.length();
        if (len > 4) boost = boost * (4 / len);
        world::push(*a, boost.x, boost.y, boost.z);
    }
}

void visit(ct::MovementContext& ctx, std::string const& kind, GlobalPos const& pos) {
    damageEntities(ctx, kind, pos);
    if (!canBreak(kind, pos)) return;
    ctx.data["BreakingPos"] = posJson(pos);
    ctx.stall               = true;
}

bool isFalling(std::string const& id) {
    return id == "minecraft:sand" || id == "minecraft:red_sand" || id == "minecraft:gravel" || id.find("concrete_powder") != std::string::npos
        || id == "minecraft:suspicious_sand" || id == "minecraft:suspicious_gravel" || id == "minecraft:anvil" || id == "minecraft:dragon_egg"
        || id == "minecraft:pointed_dripstone";
}

void onBlockBroken(ct::MovementContext& ctx, std::string const& kind, GlobalPos const& pos, std::string const& brokenId) {
    if (isSawActor(kind)) {
        if (brokenId.find("leaves") != std::string::npos) return;
        trees::fell(pos, brokenId, [&](GlobalPos const& p, ItemStack const& stack) {
            ItemStack rest = stack;
            if (config().moveItemsToStorage) rest = ctx.contraption.storage().insertItem(stack, false);
            if (ci::isEmpty(rest)) return;
            cc::Vec3d drop{p.x + .5, p.y + .5, p.z + .5};
            double    distance = (drop - ctx.position).length();
            world::dropItem(p.dim, drop.x, drop.y, drop.z, rest,
                            std::array<double, 3>{ctx.motion.x * distance / 20, ctx.motion.y * distance / 20, ctx.motion.z * distance / 20});
        });
        return;
    }
    // falling blocks above: wait 10 ticks and visit again (BlockBreakingMovementBehaviour)
    if (isFalling(brokenId)) {
        ctx.data["WaitingTicks"] = 10;
        ctx.data["LastPos"]      = posJson(pos);
        ctx.stall                = true;
    }
}


void tickBreaker(ct::MovementContext& ctx, std::string const& kind) {
    auto& data = ctx.data;
    if (!data.contains("BreakingPos")) {
        ctx.stall = false;
        return;
    }
    if (ctx.motion.length() < 1e-9) {
        ctx.stall = false;
        return;
    }
    int ticksUntil = data.value("TicksUntilNextProgress", 0);
    if (ticksUntil-- > 0) {
        data["TicksUntilNextProgress"] = ticksUntil;
        ctx.stall                      = true;
        return;
    }
    int  dim      = ctx.worldPos.dim;
    auto breaking = posFrom(dim, data["BreakingPos"]);
    int  progress = data.value("Progress", 0);
    auto const* block = world::blockAt(breaking);
    if (!block || !canBreak(kind, breaking)) {
        if (progress != 0) world::crack(breaking, -1);
        data.erase("Progress");
        data.erase("TicksUntilNextProgress");
        data.erase("BreakingPos");
        ctx.stall = false;
        return;
    }
    float hardness   = world::hardness(*block);
    float breakSpeed = cr::movingBreakSpeed(cr::animationSpeed(ctx.motion.length()));
    progress += cr::progressStep(breakSpeed, hardness, progress);
    world::playHitSound(breaking, *block); // the block's own hit sound (SoundType.getHitSound)
    if (progress >= 10) {
        world::crack(breaking, -1);
        // break falling blocks from the top down
        GlobalPos og = breaking;
        while (isFalling(world::idAt(world::offset(breaking, Face::Up)))) breaking = world::offset(breaking, Face::Up);
        std::string brokenId = world::idAt(breaking);
        ctx.stall            = false;
        for (auto const& stack : world::destroyBlock(breaking, true)) dropItem(ctx, stack);
        log().info("moving {} broke {} at {}", kind, brokenId, breaking.key());
        data.erase("Progress");
        data.erase("TicksUntilNextProgress");
        data.erase("BreakingPos");
        onBlockBroken(ctx, kind, og, brokenId);
        return;
    }
    ticksUntil                     = cr::ticksUntilNextProgress(hardness, breakSpeed);
    data["TicksUntilNextProgress"] = ticksUntil;
    data["Progress"]               = progress;
    world::crack(breaking, progress);
    ctx.stall = true;
}

ct::MovementBehaviour breakerBehaviour(std::string kind) {
    ct::MovementBehaviour b;
    b.isActive = [](ct::MovementContext const& ctx) {
        // not while moving backwards (VecHelper.isVecPointingTowards(motion, facing.opposite))
        return !pointingTowards(ctx.motion, -worldFacing(ctx));
    };
    b.startMoving = [](ct::MovementContext& ctx) { ctx.data["BreakerId"] = 1; };
    b.tick        = [kind](ct::MovementContext& ctx) {
        // Create visits the block at the active area (centre + facing * .65)
        auto      area = ctx.position + worldFacing(ctx) * .65;
        GlobalPos at   = blockOf(ctx.worldPos.dim, area);
        bool      fresh = !ctx.data.contains("Visited") || posFrom(at.dim, ctx.data["Visited"]) != at;
        if (fresh) {
            ctx.data["Visited"] = posJson(at);
            visit(ctx, kind, at);
        }
        tickBreaker(ctx, kind);
        if (ctx.data.contains("WaitingTicks")) {
            int waiting = ctx.data["WaitingTicks"].get<int>();
            if (waiting-- > 0) {
                ctx.data["WaitingTicks"] = waiting;
                ctx.stall                = true;
                return;
            }
            auto last = posFrom(at.dim, ctx.data["LastPos"]);
            ctx.data.erase("WaitingTicks");
            ctx.data.erase("LastPos");
            ctx.stall = false;
            visit(ctx, kind, last);
        }
    };
    b.stopMoving = [](ct::MovementContext& ctx) {
        if (ctx.data.contains("BreakingPos")) world::crack(posFrom(ctx.worldPos.dim, ctx.data["BreakingPos"]), -1);
        ctx.data.erase("BreakingPos");
        ctx.data.erase("Progress");
        ctx.data.erase("TicksUntilNextProgress");
        ctx.data.erase("Visited");
        ctx.stall = false;
    };
    return b;
}

/// Rotates a face by quarter turns about an axis (right-hand rule, as the contraption transform).
Face rotateFace(Face f, cc::Axis axis, int quarter) {
    auto o = k::offsetOf(f);
    int  x = o.x, y = o.y, z = o.z;
    quarter = ((quarter % 4) + 4) % 4;
    for (int i = 0; i < quarter; ++i) {
        int nx = x, ny = y, nz = z;
        if (axis == cc::Axis::X) {
            ny = -z;
            nz = y;
        } else if (axis == cc::Axis::Y) {
            nx = z;
            nz = -x;
        } else {
            nx = -y;
            ny = x;
        }
        x = nx;
        y = ny;
        z = nz;
    }
    return k::nearestFace(x, y, z);
}

k::Axis rotateAxis(k::Axis a, cc::Axis axis, int quarter) {
    return k::axisOf(rotateFace(k::faceOf(a, true), axis, quarter));
}

struct ActorRegistration {
    ActorRegistration() {
        ct::registerActor(std::string(kDrill), breakerBehaviour("drill"));
        ct::registerActor(std::string(kSaw), breakerBehaviour("saw"));
        // the saw's blade axis follows a rotation (levitate:facing is rotated by the engine first)
        ct::registerRotationHandler(std::string(kSaw), [](cc::States& states, cc::Axis axis, int quarter) {
            auto get = [&](char const* key, std::int64_t fallback) {
                auto it = states.find(key);
                if (it == states.end()) return fallback;
                auto const* v = std::get_if<std::int64_t>(&it->second);
                return v ? *v : fallback;
            };
            Face newFacing  = static_cast<Face>(std::clamp<std::int64_t>(get(kFacing, 1), 0, 5));
            Face oldFacing  = rotateFace(newFacing, axis, -quarter);
            bool along      = get(kAlongFirst, 0) != 0;
            auto oldAxis    = cr::directionalAxis(oldFacing, along);
            auto wanted     = rotateAxis(oldAxis, axis, quarter);
            states[kAlongFirst] = std::int64_t{cr::directionalAxis(newFacing, true) == wanted ? 1 : 0};
        });
    }
};
ActorRegistration const gActors;

} // namespace

} // namespace lv::feature::crafter
