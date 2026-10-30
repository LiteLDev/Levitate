// Encased fan and nozzle (Create's EncasedFanBlockEntity, AirCurrent, FanProcessing,
// AllFanProcessingTypes, NozzleBlockEntity).
//
// A turning fan blows (or sucks) along its facing up to fanPushDistance / fanPullDistance blocks
// (3 blocks at 0 RPM, the maximum at fanRotationArgmax), stopped by solid blocks. Blocks in the
// current change the air downstream of them: water -> splashing (washing), fire / lit campfire /
// smouldering blaze burner -> smoking, lava / heated blaze burner -> blasting, soul fire / lit soul
// campfire -> haunting. Entities in the current are pushed; items in processing air are processed
// after fanProcessingTime ticks; other entities take the type's effect. Items on belts / depots in
// the current are processed through the item capability.

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/entity/components_json_legacy/ContainerComponent.h"
#include "mc/world/Container.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/BaseAttributeMap.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/actor/animal/Horse.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "feature/advancements/Advancements.h"
#include "feature/bulk/Bulk.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "mod/Log.h"

namespace lv::feature::bulk {

namespace items = common::items;
using cb::FanType;
using core::kinetics::isPositive;
using core::kinetics::opposite;

namespace {

// ---- fan state ------------------------------------------------------------------------------------------------

struct FanState {
    cb::AirCurrent                                    current;
    cb::Box                                           bounds;
    float                                             speed          = 0;
    int                                               checkCooldown  = 0;
    int                                               searchCooldown = 0;
    bool                                              dirty          = true;
    std::vector<std::int64_t>                         caught;
    std::vector<std::pair<BlockPos, FanType>>         handlers;
    std::string                                       signature; ///< last logged current
};

std::map<GlobalPos, FanState>& fans() {
    static std::map<GlobalPos, FanState> m;
    return m;
}

/// Fan processing state of item entities (Create keeps it in the entity's persistent data).
struct ItemProcessing {
    FanType type = FanType::None;
    int     time = 0;
};
std::unordered_map<std::int64_t, ItemProcessing>& itemProcessing() {
    static std::unordered_map<std::int64_t, ItemProcessing> m;
    return m;
}

/// Create's TransportedItemStack.processedBy / processingTime for items on belts and depots.
struct TransportedProcessing {
    FanType     type = FanType::None;
    std::string item;
    int         count = 0;
    int         time  = 0;
};
std::map<std::tuple<int, int, int, int, int>, TransportedProcessing>& transported() {
    static std::map<std::tuple<int, int, int, int, int>, TransportedProcessing> m;
    return m;
}

/// Haunting progress of horses (Create's "CreateHaunting").
std::unordered_map<std::int64_t, int>& hauntingProgress() {
    static std::unordered_map<std::int64_t, int> m;
    return m;
}

constexpr std::size_t kNozzleEntityLimit = 256;

struct NozzleState {
    float                     range   = 0;
    bool                      pushing = true;
    std::vector<std::int64_t> entities;
    int                       lazy = 0;
};
std::map<GlobalPos, NozzleState>& nozzles() {
    static std::map<GlobalPos, NozzleState> m;
    return m;
}

/// Blocks the processing of items on them applies to (Create's TransportedItemStackHandlerBehaviour).
bool isTransportedHandler(std::string_view id) {
    return id == "levitate:belt" || id == "levitate:depot" || id == "levitate:weighted_ejector";
}

// ---- blocks in the current -------------------------------------------------------------------------------------

/// Create's #create:fan_transparent.
bool fanTransparent(std::string_view id) {
    static std::unordered_set<std::string_view> const ids{
        "levitate:blaze_burner",
        "levitate:lit_blaze_burner",
        "levitate:sail_frame",
        "levitate:andesite_bars",
        "levitate:brass_bars",
        "levitate:copper_bars",
        "levitate:copycat_base",
        "minecraft:iron_bars",
        "minecraft:mangrove_roots",
        "minecraft:campfire",
        "minecraft:soul_campfire",
        "minecraft:azalea_leaves_flowered",
    };
    if (ids.contains(id)) return true;
    if (id.ends_with("_leaves")) return true;
    if (id.ends_with("copper_grate")) return true;
    if (id.ends_with("_fence") || id == "minecraft:fence") return true;
    return false;
}

cb::FlowBlock flowBlockAt(BlockSource& region, BlockPos const& p) {
    if (!loaded(region, p)) return {cb::FlowBlock::Kind::Unloaded};
    auto const& block = region.getBlock(p);
    if (block.isAir() || fanTransparent(block.getTypeName())) return {};
    // every box of the collision shape (Create's VoxelShape: stairs, walls, ... have several)
    std::vector<AABB> shapes;
    block.getBlockType().addCollisionShapes(block, region, p, nullptr, shapes, {});
    cb::FlowBlock b{cb::FlowBlock::Kind::Partial};
    bool          first = true;
    for (auto const& shape : shapes) {
        double lo[3] = {shape.min.x - p.x, shape.min.y - p.y, shape.min.z - p.z};
        double hi[3] = {shape.max.x - p.x, shape.max.y - p.y, shape.max.z - p.z};
        if (hi[0] - lo[0] <= 1e-6 || hi[1] - lo[1] <= 1e-6 || hi[2] - lo[2] <= 1e-6) continue;
        bool full = true;
        for (int i = 0; i < 3; ++i)
            if (lo[i] > 1e-4 || hi[i] < 1 - 1e-4) full = false;
        if (full) return {cb::FlowBlock::Kind::Full};
        cb::FlowBlock::Box box;
        for (int i = 0; i < 3; ++i) {
            box.min[i] = std::clamp(lo[i], 0.0, 1.0);
            box.max[i] = std::clamp(hi[i], 0.0, 1.0);
        }
        if (first) {
            b.min = box.min;
            b.max = box.max;
            first = false;
        } else {
            b.more.push_back(box);
        }
    }
    if (first) return {}; // no collision: the current passes
    return b;
}

std::string const& idOfBlock(Block const& b) { return b.getTypeName(); }

// ---- processing particles ---------------------------------------------------------------------------------------

void processingParticles(int dim, cb::Vec3d const& pos, FanType type) {
    if (random01() >= 1 / 8.f) return; // Create: one in eight ticks
    switch (type) {
    case FanType::Blasting:
        particle(dim, pos + cb::Vec3d{0, .25, 0}, "minecraft:basic_smoke_particle");
        break;
    case FanType::Haunting:
        particle(dim, pos + cb::Vec3d{0, .45, 0}, "minecraft:blue_flame_particle");
        if (random01() < .5f) particle(dim, pos + cb::Vec3d{0, .25, 0}, "minecraft:basic_smoke_particle");
        break;
    case FanType::Smoking:
        particle(dim, pos + cb::Vec3d{0, .25, 0}, "minecraft:white_smoke_particle");
        break;
    case FanType::Splashing:
        particle(dim, pos + cb::Vec3d{(random01() - .5) * .5, .5, (random01() - .5) * .5}, "minecraft:water_splash_particle");
        break;
    default:
        break;
    }
}

// ---- airflow particles ----------------------------------------------------------------------------------------------

struct Rgba {
    float r, g, b, a;
};
Rgba mix(std::uint32_t c0, std::uint32_t c1, float t, float alpha) {
    auto ch = [&](int shift) {
        float a = ((c0 >> shift) & 0xff) / 255.f, b = ((c1 >> shift) & 0xff) / 255.f;
        return a + (b - a) * t;
    };
    return {ch(16), ch(8), ch(0), alpha};
}
Rgba colourOf(FanType t) {
    float r = random01();
    switch (t) {
    case FanType::Splashing:
        return mix(0x4499FF, 0x2277FF, r, 1);
    case FanType::Smoking:
        return mix(0x000000, 0x555555, r, 1);
    case FanType::Blasting:
        return mix(0xFF4400, 0xFF8855, r, .5f);
    case FanType::Haunting:
        return mix(0x000000, 0x126568, r, 1);
    default:
        return {0xEE / 255.f, 0xEE / 255.f, 0xEE / 255.f, .25f};
    }
}

/// One AirFlowParticle: it starts at the fan (pushing) or at the far end (pulling) and moves at
/// (maxDistance + 1 - distance) / 16 blocks per tick (Create), i.e. with an exponential drag of
/// 1/16 per tick; its colour follows the processing type of the air it is in.
void airflowParticle(GlobalPos const& pos, FanState const& st) {
    auto const& c     = st.current;
    float       L     = c.maxDistance;
    if (L < .25f) return;
    constexpr float k = 20.f / 16.f; // per second
    auto        n     = cb::normalOf(c.direction);
    cb::Vec3d   centre{pos.x + .5, pos.y + .5, pos.z + .5};
    cb::Vec3d   start = centre + n * (c.pushing ? .5 : L + .5);
    auto        flow  = c.pushing ? n : n * -1.0;
    float       speed = c.pushing ? (L + 1) * k : k;
    float       drag  = c.pushing ? k : -k;
    float       life  = c.pushing ? std::log((L + 1) / .75f) / k : std::log(L + 1.25f) / k;
    life              = std::clamp(life, .05f, 2.f);
    FanType t0        = c.typeAt(c.pushing ? 0.f : L);
    FanType t1        = t0;
    float   tSwitch   = 99.f;
    if (c.segments.size() > 1) {
        float boundary = float(c.segments[0].endOffset);
        t1             = c.segments[1].type;
        if (c.pushing) {
            float frac = boundary / (L + 1);
            if (frac < 1) tSwitch = -std::log(1 - frac) / k;
        } else {
            tSwitch = std::log(std::max(L + 1 - boundary, 1.f)) / k;
        }
    }
    auto a = colourOf(t0), b = colourOf(t1);
    particleWith(
        pos.dim,
        start,
        "levitate:fan_airflow",
        {
            {"dir_x",  float(flow.x)},
            {"dir_y",  float(flow.y)},
            {"dir_z",  float(flow.z)},
            {"speed",  speed        },
            {"drag",   drag         },
            {"life",   life         },
            {"switch", tSwitch      },
            {"r0",     a.r          },
            {"g0",     a.g          },
            {"b0",     a.b          },
            {"a0",     a.a          },
            {"r1",     b.r          },
            {"g1",     b.g          },
            {"b1",     b.b          },
            {"a1",     b.a          },
    }
    );
}

// ---- rebuild -------------------------------------------------------------------------------------------------------

void rebuild(BlockSource& region, GlobalPos const& pos, cb::Face facing, FanState& st) {
    st.handlers.clear();
    if (st.speed == 0) {
        st.current.maxDistance = 0;
        st.current.segments.clear();
        st.bounds = {};
        if (!st.signature.empty()) log().info("encased fan at {} {} {}: stopped", pos.x, pos.y, pos.z);
        st.signature.clear();
        return;
    }
    auto bp                = toBlockPos(pos);
    st.current.direction   = facing;
    st.current.pushing     = cb::fanPushing(facing, st.speed);
    float max              = cb::fanMaxDistance(st.speed, fanConfig());
    st.current.maxDistance = cb::flowLimit(max, facing, [&](int i) { return flowBlockAt(region, offset(bp, facing, i)); });
    st.current.buildSegments([&](int i) { return catalystAt(region, offset(bp, facing, i)); });
    st.bounds = cb::currentBounds(pos.x, pos.y, pos.z, facing, st.current.maxDistance);

    // Create's findAffectedHandlers: belts / depots in the current or just below it
    int limit = cb::limitOf(st.current.maxDistance);
    for (int i = 1; i <= limit; ++i) {
        FanType segment = st.current.typeAt(float(i - 1));
        for (int down = 0; down <= 1; ++down) {
            auto p = offset(bp, facing, i);
            p.y -= down;
            if (isTransportedHandler(idAt(region, p))) {
                FanType t = catalystAt(region, p);
                st.handlers.emplace_back(p, t == FanType::None ? segment : t);
            }
            if (core::kinetics::axisOf(facing) == cb::Axis::Y) break;
        }
    }

    std::string sig = std::to_string(int(st.current.maxDistance * 4)) + (st.current.pushing ? "+" : "-");
    for (auto const& s : st.current.segments) sig += std::string(cb::fanTypeName(s.type)) + "@" + std::to_string(s.endOffset) + ";";
    if (sig != st.signature) {
        std::string types;
        for (auto const& s : st.current.segments)
            if (s.type != FanType::None) types += std::string(types.empty() ? "" : ", ") + std::string(cb::fanTypeName(s.type));
        log().info(
            "encased fan at {} {} {}: {} {:.2f} blocks{}{}",
            pos.x,
            pos.y,
            pos.z,
            st.current.pushing ? "pushing" : "pulling",
            st.current.maxDistance,
            types.empty() ? "" : ", processing: ",
            types
        );
        st.signature = sig;
    }
}

// ---- item entities ---------------------------------------------------------------------------------------------------

// Create keeps "CreateData.Processing" {Type, Time} in the item entity's persistent data, so it
// survives saving and chunk unloading: Levitate mirrors it in an entity tag "lv_fan:<type>:<time>"
// (written when the type changes, when it is done and every second while it counts down).
constexpr std::string_view kProcessingTag = "lv_fan:";

void writeProcessingTag(Actor& e, ItemProcessing const& st) {
    if (e.hasTags())
        for (auto const& t : e.getTags())
            if (t.starts_with(kProcessingTag)) e.removeTag(t);
    if (st.type != FanType::None) e.addTag(std::string(kProcessingTag) + std::to_string(int(st.type)) + ":" + std::to_string(st.time));
}

ItemProcessing& processingOf(Actor& e) {
    auto id = idOf(e);
    if (auto it = itemProcessing().find(id); it != itemProcessing().end()) return it->second;
    ItemProcessing st;
    if (e.hasTags()) {
        for (auto const& t : e.getTags()) {
            if (!t.starts_with(kProcessingTag)) continue;
            int type = 0, time = 0;
            if (std::sscanf(t.c_str() + kProcessingTag.size(), "%d:%d", &type, &time) == 2) st = {static_cast<FanType>(type), time};
        }
    }
    return itemProcessing()[id] = st;
}

bool canProcessEntity(Actor& e, ItemStack const& stack, FanType type) {
    auto const& st = processingOf(e);
    if (st.type == FanType::None || st.type != type) return canFanProcess(stack, type);
    if (st.time >= 0) return true;
    return false;
}

bool applyProcessing(BlockSource& region, GlobalPos const& fan, Actor& entity, FanType type) {
    auto* stack = itemOf(entity);
    if (!stack) return false;
    auto  id = idOf(entity);
    auto& st = processingOf(entity);
    if (st.type != type) {
        st.type = type;
        st.time = cb::fanProcessingTime(items::countOf(*stack), config().fanProcessingTime);
        writeProcessingTag(entity, st);
    }
    if (--st.time != 0) {
        if (st.time % 20 == 0) writeProcessingTag(entity, st);
        return false;
    }
    auto results = fanProcess(*stack, type);
    if (!results) return false;
    auto input = items::idOf(*stack);
    int  count = items::countOf(*stack);
    if (results->empty()) {
        itemProcessing().erase(id);
        entity.remove();
        log().info("encased fan at {} {} {}: {} destroyed {} x{}", fan.x, fan.y, fan.z, cb::fanTypeName(type), input, count);
        return true;
    }
    // the client never sees a changed item entity: replace it (keeping its processing state)
    auto feet   = feetOf(entity);
    auto motion = motionOf(entity);
    entity.remove();
    itemProcessing().erase(id);
    bool first = true;
    for (auto const& s : *results) {
        auto nid = spawnItem(region, s, feet, motion);
        if (first && nid) {
            itemProcessing()[nid] = {type, -1};
            if (auto* fresh = entityById(nid)) writeProcessingTag(*fresh, {type, -1});
        }
        first = false;
    }
    log().info(
        "encased fan at {} {} {}: {} turned {} x{} into {} x{}",
        fan.x,
        fan.y,
        fan.z,
        cb::fanTypeName(type),
        input,
        count,
        items::idOf(results->front()),
        items::countOf(results->front())
    );
    return true;
}

// ---- transported items (belts, depots) --------------------------------------------------------------------------------

void tickHandlers(BlockSource& region, GlobalPos const& fan, FanState const& st) {
    for (auto const& [p, type] : st.handlers) {
        if (type == FanType::None) {
            // plain air resets the items' processing (Create clears processedBy)
            std::erase_if(transported(), [&](auto const& kv) {
                auto const& [d, x, y, z, s] = kv.first;
                return d == fan.dim && x == p.x && y == p.y && z == p.z;
            });
            continue;
        }
        common::Access at{region, p, -1};
        auto           handler = common::items::find(at);
        if (!handler || !handler->slots || !handler->getSlot) continue;
        int slots = handler->slots(at);
        for (int slot = 0; slot < slots; ++slot) {
            auto key   = std::make_tuple(fan.dim, p.x, p.y, p.z, slot);
            auto stack = handler->getSlot(at, slot);
            if (items::isEmpty(stack)) {
                transported().erase(key);
                continue;
            }
            auto& tp = transported()[key];
            auto  id = items::idOf(stack);
            if (tp.type != type || tp.item != id || tp.count != items::countOf(stack)) {
                tp = {type, id, items::countOf(stack), cb::fanProcessingTime(items::countOf(stack), config().fanProcessingTime)};
                if (!canFanProcess(stack, type)) tp.time = -1;
                continue;
            }
            processingParticles(fan.dim, {p.x + .5, p.y + .8, p.z + .5}, type);
            if (tp.time == -1) continue;
            if (tp.time-- > 0) continue;
            if (!fanProcess(stack, type)) continue;
            // the capability extracts by item, not by slot: process exactly what came out
            auto taken = handler->extract(at, [&](ItemStack const& s) { return sameItem(s, stack); }, items::countOf(stack), false);
            if (items::isEmpty(taken)) continue;
            auto results = fanProcess(taken, type);
            if (!results) results.emplace();
            std::vector<ItemStack> spill;
            for (auto const& out : *results) {
                auto rest = handler->insert ? handler->insert(at, out, false) : out;
                if (!items::isEmpty(rest)) spill.push_back(rest);
            }
            dropStacks(region, p, spill);
            log().info("encased fan at {} {} {}: {} processed {} x{} at {} {} {}", fan.x, fan.y, fan.z, cb::fanTypeName(type), id, items::countOf(taken), p.x, p.y, p.z);
            advancements::awardOwner(fan, "fan_processing");
            // the results are not processed again by the same air (Create copies processedBy onto
            // every result): mark every slot now holding one of them
            for (int s = 0; s < slots; ++s) {
                auto now = handler->getSlot(at, s);
                if (items::isEmpty(now)) continue;
                bool isResult = std::any_of(results->begin(), results->end(), [&](ItemStack const& r) { return sameItem(r, now); });
                if (s == slot || isResult) transported()[std::make_tuple(fan.dim, p.x, p.y, p.z, s)] = {type, items::idOf(now), items::countOf(now), -1};
            }
        }
    }
}

void tickEntities(BlockSource& region, GlobalPos const& pos, FanState& st) {
    cb::Vec3d centre{pos.x + .5, pos.y + .5, pos.z + .5};
    for (auto it = st.caught.begin(); it != st.caught.end();) {
        auto* e = entityById(*it);
        if (!e || !e->isAlive() || !boxOf(*e).intersects(st.bounds) || isCreativeFlying(*e)
            || (e->isPlayer() && static_cast<Player*>(e)->getPlayerGameType() == GameType::Spectator)) {
            it = st.caught.erase(it);
            continue;
        }
        ++it;
        auto feet = feetOf(*e);
        auto m    = cb::pushedMotion(motionOf(*e), st.current, st.speed, isSneaking(*e), (feet - centre).length());
        setMotion(*e, m);
        e->setFallDistance(0);
        if (e->isPlayer()) addWindListener(*e, st.speed, float(m.length()));
        double  distance = cb::alignedDistanceToFace(feet, pos.x, pos.y, pos.z, st.current.direction);
        FanType type     = st.current.typeAt(float(distance));
        if (type == FanType::None) continue;
        if (isItem(*e)) {
            processingParticles(pos.dim, feet, type);
            auto* stack = itemOf(*e);
            if (stack && canProcessEntity(*e, *stack, type) && applyProcessing(region, pos, *e, type)) advancements::awardOwner(pos, "fan_processing");
            continue;
        }
        affectEntity(*e, type, pos.dim);
    }
}

} // namespace

// ---- catalysts and processing ---------------------------------------------------------------------------------------------

FanType catalystAt(BlockSource& region, BlockPos const& p) {
    if (!loaded(region, p)) return FanType::None;
    std::vector<FanType> valid;
    auto const&          block = region.getBlock(p);
    auto const&          id    = idOfBlock(block);
    auto const&          liquid = region.getLiquidBlock(p).getTypeName();
    auto isWater = [](std::string const& s) { return s == "minecraft:water" || s == "minecraft:flowing_water"; };
    auto isLava  = [](std::string const& s) { return s == "minecraft:lava" || s == "minecraft:flowing_lava"; };
    if (isWater(id) || isWater(liquid)) valid.push_back(FanType::Splashing);
    if (isLava(id) || isLava(liquid)) valid.push_back(FanType::Blasting);
    if (id == "levitate:blaze_burner") {
        auto heat = common::heat::level(region, p);
        if (heat >= core::common::HeatLevel::Fading) valid.push_back(FanType::Blasting);
        if (heat == core::common::HeatLevel::Smouldering) valid.push_back(FanType::Smoking);
    } else if (id == "levitate:lit_blaze_burner") {
        bool soul = stateOf(block, "levitate:flame_type") == 1;
        valid.push_back(soul ? FanType::Haunting : FanType::Smoking);
    } else if (id == "minecraft:fire") {
        valid.push_back(FanType::Smoking);
    } else if (id == "minecraft:soul_fire") {
        valid.push_back(FanType::Haunting);
    } else if (id == "minecraft:campfire" || id == "minecraft:soul_campfire") {
        if (!stateOf(block, "extinguished")) valid.push_back(id == "minecraft:campfire" ? FanType::Smoking : FanType::Haunting);
    }
    return cb::bestType(valid);
}

namespace {
bool fireResistant(ItemStack const& s) {
    auto id = items::idOf(s);
    return id.find("netherite") != std::string::npos || id == "minecraft:ancient_debris";
}
} // namespace

bool canFanProcess(ItemStack const& stack, FanType type) {
    if (items::isEmpty(stack)) return false;
    switch (type) {
    case FanType::Splashing:
        return findRecipe("splashing", stack) != nullptr;
    case FanType::Haunting:
        return findRecipe("haunting", stack) != nullptr;
    case FanType::Smoking:
        return furnaceResult(stack, "smoker").has_value();
    case FanType::Blasting:
        if (furnaceResult(stack, "furnace") || furnaceResult(stack, "blast_furnace")) return true;
        return !fireResistant(stack);
    default:
        return false;
    }
}

std::optional<std::vector<ItemStack>> fanProcess(ItemStack const& stack, FanType type) {
    switch (type) {
    case FanType::Splashing:
    case FanType::Haunting: {
        auto const* r = findRecipe(type == FanType::Splashing ? "splashing" : "haunting", stack);
        if (!r) return std::nullopt;
        return rollRecipe(*r, items::countOf(stack));
    }
    case FanType::Smoking: {
        auto out = furnaceResult(stack, "smoker");
        if (!out) return std::nullopt;
        return multipliedOutput(stack, *out);
    }
    case FanType::Blasting: {
        auto smoking  = furnaceResult(stack, "smoker");
        auto smelting = furnaceResult(stack, "furnace");
        if (!smelting) smelting = furnaceResult(stack, "blast_furnace");
        // food that a smoker would cook is incinerated (Create)
        if (smelting && (!smoking || !items::sameItem(*smoking, *smelting))) return multipliedOutput(stack, *smelting);
        return std::vector<ItemStack>{};
    }
    default:
        return std::nullopt;
    }
}

/// Create's haunting copies the horse's saved data (all but its UUID) onto the skeleton horse and
/// drops its armor: Levitate copies what a horse's data holds that a skeleton horse can keep
/// (name, health / speed / jump attributes, tame state and owner, saddle).
void copyHorse(Actor& horse, Actor& skeleton, BlockSource& region) {
    if (auto const& name = horse.getNameTag(); !name.empty()) skeleton.setNameTag(name);
    for (auto const* attr : {&SharedAttributes::HEALTH(), &SharedAttributes::MOVEMENT_SPEED(), &SharedAttributes::JUMP_STRENGTH()}) {
        AttributeInstance const* from = horse.getAttribute(*attr).mPtr;
        if (!from) continue;
        auto* to = const_cast<BaseAttributeMap*>(&*skeleton.getAttributes());
        (void)to->setMaxValue(*attr, from->mCurrentMaxValue);
        (void)to->setCurrentValue(*attr, from->mCurrentValue);
    }
    // a tamed or named horse stays (Create copies PersistenceRequired)
    if (horse.isTame() || !horse.getNameTag().empty()) skeleton.setPersistent();
    if (horse.isTame()) {
        // Bedrock tames a horse to a player (Horse::tameToPlayer, the game's horse class covers the
        // skeleton horse too); an owner who is offline only becomes the owner
        auto  level = ll::service::getLevel();
        auto* owner = level ? level->getPlayer(horse.getOwnerId()) : nullptr;
        if (owner) static_cast<Horse&>(skeleton).tameToPlayer(*owner, false);
        skeleton.setOwner(horse.getOwnerId());
    }
    auto from = horse.getEntityContext().tryGetComponent<ContainerComponent>();
    auto to   = skeleton.getEntityContext().tryGetComponent<ContainerComponent>();
    if (!from) return;
    auto& in    = *from->mContainer;
    auto  feet  = feetOf(horse);
    Vec3  at{float(feet.x), float(feet.y), float(feet.z)};
    for (int slot = 0; slot < in.getContainerSize(); ++slot) {
        ItemStack stack = in.getItem(slot);
        if (items::isEmpty(stack)) continue;
        // slot 0 saddle (kept), 1 armor (dropped: a skeleton horse wears none)
        if (slot == 0 && to && to->mContainer->getContainerSize() > 0) {
            to->mContainer->setItem(0, stack);
            continue;
        }
        dropStacks(region, BlockPos{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))}, {stack});
    }
    in.removeAllItems();
}

void affectEntity(Actor& e, FanType type, int dim) {
    switch (type) {
    case FanType::Splashing: {
        auto const& t = e.getTypeName();
        if (t == "minecraft:enderman" || t == "minecraft:snow_golem" || t == "minecraft:blaze")
            e.hurtByCause(2, SharedTypes::Legacy::ActorDamageCause::Drowning);
        if (e.isOnFire()) {
            e.stopFire();
            sound(dim, feetOf(e), "random.fizz", .7f, 1.6f + (random01() - random01()) * .4f);
        }
        break;
    }
    case FanType::Smoking:
        if (!e.isFireImmune()) {
            e.setOnFire(2);
            hurtBy(e, 2, int(SharedTypes::Legacy::ActorDamageCause::Fire), "death.attack.create.fan_fire");
        }
        break;
    case FanType::Blasting:
        if (!e.isFireImmune()) {
            e.setOnFire(10);
            hurtBy(e, 4, int(SharedTypes::Legacy::ActorDamageCause::Lava), "death.attack.create.fan_lava");
        }
        break;
    case FanType::Haunting: {
        if (isLiving(e)) {
            auto effect = [&](unsigned id, int ticks, int amplifier) {
                EffectDuration d;
                d.mValue = ticks;
                MobEffectInstance inst(id, d);
                inst.mAmplifier     = amplifier;
                inst.mAmbient       = false;
                inst.mEffectVisible = false;
                e.addEffect(inst);
            };
            effect(15, 30, 0); // blindness
            effect(2, 20, 1);  // slowness II
        }
        if (e.isType(ActorType::Horse)) {
            auto  id       = idOf(e);
            auto& progress = hauntingProgress()[id];
            if (progress < 100) {
                if (progress % 10 == 0) sound(dim, feetOf(e), "particle.soul_escape", 1.f, 1.5f * progress / 100.f);
                ++progress;
                return;
            }
            hauntingProgress().erase(id);
            auto feet = feetOf(e);
            sound(dim, feet, "random.fizz", 1.25f, .65f);
            auto level = ll::service::getLevel();
            if (!level) return;
            auto& region = e.getDimensionBlockSource();
            // the actor factory (not the spawner, whose spawn rules may refuse it): Create's addFreshEntity
            Actor* skeleton = nullptr;
            if (auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier("minecraft:skeleton_horse"), nullptr,
                                                                         Vec3{float(feet.x), float(feet.y), float(feet.z)}, Vec2{0, 0}))
                skeleton = level->addEntity(region, std::move(owner));
            if (skeleton) copyHorse(e, *skeleton, region);
            e.remove();
            log().info("encased fan: haunted a horse into a skeleton horse at {:.1f} {:.1f} {:.1f}", feet.x, feet.y, feet.z);
        }
        break;
    }
    default:
        break;
    }
}

// ---- fans ----------------------------------------------------------------------------------------------------------------

void tickFan(BlockSource& region, GlobalPos const& pos) {
    auto& st     = fans()[pos];
    auto  bp     = toBlockPos(pos);
    auto  facing = faceOf(stateOf(region.getBlock(bp), "levitate:facing"));
    float speed  = kinetics::speedAt(pos);
    if (speed != st.speed || facing != st.current.direction) {
        st.speed = speed;
        st.dirty = true;
    }
    if (st.checkCooldown-- <= 0) {
        st.checkCooldown = std::max(1, config().fanBlockCheckRate);
        st.dirty         = true;
    }
    if (st.dirty) {
        st.dirty = false;
        rebuild(region, pos, facing, st);
    }
    if (speed == 0) return;
    if (st.searchCooldown-- <= 0) {
        st.searchCooldown = 5;
        st.caught.clear();
        for (auto* a : entitiesIn(region, st.bounds)) st.caught.push_back(idOf(*a));
    }
    tickEntities(region, pos, st);
    tickHandlers(region, pos, st);
    if (random01() < config().fanParticleDensity) airflowParticle(pos, st);
}

void markFanDirty(GlobalPos const& pos) {
    if (auto it = fans().find(pos); it != fans().end()) it->second.dirty = true;
}

void forgetFan(GlobalPos const& pos) {
    fans().erase(pos);
    nozzles().erase(pos);
}

void clearFans() {
    fans().clear();
    nozzles().clear();
    itemProcessing().clear();
    transported().clear();
    hauntingProgress().clear();
}

void pruneFanState() {
    std::set<std::tuple<int, int, int, int>> reached;
    for (auto const& [pos, st] : fans())
        for (auto const& [p, t] : st.handlers) reached.insert({pos.dim, p.x, p.y, p.z});
    std::erase_if(transported(), [&](auto const& kv) {
        auto const& [d, x, y, z, s] = kv.first;
        return !reached.contains({d, x, y, z});
    });
    std::erase_if(itemProcessing(), [](auto const& kv) { return entityById(kv.first) == nullptr; });
    std::erase_if(hauntingProgress(), [](auto const& kv) { return entityById(kv.first) == nullptr; });
}

std::optional<FanView> fanAt(GlobalPos const& pos) {
    auto it = fans().find(pos);
    if (it == fans().end()) return std::nullopt;
    return FanView{it->second.current, it->second.bounds, it->second.speed};
}

// ---- nozzle -----------------------------------------------------------------------------------------------------------------

bool nozzleCanSurvive(BlockSource& region, BlockPos const& p) {
    return nozzleCanSurvive(region, p, faceOf(stateOf(region.getBlock(p), "levitate:facing")));
}

bool nozzleCanSurvive(BlockSource& region, BlockPos const& p, cb::Face facing) {
    auto behind = offset(p, opposite(facing));
    if (idAt(region, behind) != kFan) return false;
    return stateOf(region.getBlock(behind), "levitate:facing") == int(facing);
}

namespace {
/// Create's NozzleBlockEntity.canSee: nothing solid between the entity and the nozzle.
bool canSee(BlockSource& region, BlockPos const& nozzle, cb::Vec3d const& from) {
    cb::Vec3d to{nozzle.x + .5, nozzle.y + .5, nozzle.z + .5};
    auto      d     = to - from;
    double    len   = d.length();
    int       steps = std::max(1, int(len / .2));
    for (int i = 0; i <= steps; ++i) {
        auto     p = from + d * (double(i) / steps);
        BlockPos b{int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))};
        if (b == nozzle) return true;
        auto fb = flowBlockAt(region, b);
        if (fb.kind == cb::FlowBlock::Kind::Unloaded) return false;
        if (fb.kind == cb::FlowBlock::Kind::Pass) continue;
        double lx = p.x - b.x, ly = p.y - b.y, lz = p.z - b.z;
        if (fb.kind == cb::FlowBlock::Kind::Full) return false;
        if (lx >= fb.min[0] && lx <= fb.max[0] && ly >= fb.min[1] && ly <= fb.max[1] && lz >= fb.min[2] && lz <= fb.max[2]) return false;
    }
    return true;
}
} // namespace

void tickNozzle(BlockSource& region, GlobalPos const& pos) {
    auto bp = toBlockPos(pos);
    if (!nozzleCanSurvive(region, bp)) {
        nozzles().erase(pos);
        log().info("nozzle at {} {} {}: no fan behind it, dropped", pos.x, pos.y, pos.z);
        breakBlock(region, bp);
        return;
    }
    auto& st     = nozzles()[pos];
    auto  facing = faceOf(stateOf(region.getBlock(bp), "levitate:facing"));
    auto  fanPos = toGlobal(pos.dim, offset(bp, opposite(facing)));
    float speed  = kinetics::speedAt(fanPos);
    float range  = speed == 0 ? 0.f : cb::fanMaxDistance(speed, fanConfig());
    if (speed != 0) st.pushing = cb::fanPushing(facing, speed);
    if (range != st.range) {
        st.range = range;
        if (range == 0) st.entities.clear();
        log().info("nozzle at {} {} {}: range {:.1f} ({})", pos.x, pos.y, pos.z, range, st.pushing ? "pushing" : "pulling");
    }
    cb::Vec3d centre{pos.x + .5, pos.y + .5, pos.z + .5};
    if (range != 0) {
        int chance = std::clamp(int(config().fanPushDistance) - int(range), 1, 10);
        if (random01() * chance < 1) {
            auto off = cb::Vec3d{random01() * 2 - 1.0, random01() * 2 - 1.0, random01() * 2 - 1.0} * (st.pushing ? 1.0 : range / 2.0);
            particle(pos.dim, centre + off, "minecraft:white_smoke_particle");
        }
    }
    for (auto it = st.entities.begin(); it != st.entities.end();) {
        auto* e = entityById(*it);
        if (!e || !e->isAlive()) {
            it = st.entities.erase(it);
            continue;
        }
        auto   diff = feetOf(*e) - centre;
        double dist = diff.length();
        if (dist > range || isSneaking(*e) || isCreativeFlying(*e)) {
            it = st.entities.erase(it);
            continue;
        }
        ++it;
        auto push = cb::nozzlePush(diff, range, st.pushing, isItem(*e));
        if (!push) continue;
        setMotion(*e, motionOf(*e) + *push);
        e->setFallDistance(0);
    }
    if (st.lazy-- > 0 || range == 0) return;
    st.lazy = 5;
    cb::Box bb{centre - cb::Vec3d{range / 2.0, range / 2.0, range / 2.0}, centre + cb::Vec3d{range / 2.0, range / 2.0, range / 2.0}};
    for (auto* e : entitiesIn(region, bb)) {
        auto   diff = feetOf(*e) - centre;
        double dist = diff.length();
        if (dist > range || isSneaking(*e) || isCreativeFlying(*e)) continue;
        auto id = idOf(*e);
        if (!canSee(region, bp, feetOf(*e))) {
            std::erase(st.entities, id);
            continue;
        }
        if (std::find(st.entities.begin(), st.entities.end(), id) == st.entities.end()) st.entities.push_back(id);
    }
    // Create's lazyTick: a pulling nozzle holding more than 256 entities explodes (power 2, no
    // block damage) and the entities are gone (players stay: Bedrock can't discard them)
    std::erase_if(st.entities, [](std::int64_t id) {
        auto* e = entityById(id);
        return !e || !e->isAlive();
    });
    if (!st.pushing && st.entities.size() > kNozzleEntityLimit) {
        if (auto level = ll::service::getLevel())
            level->explode(region, nullptr, Vec3{float(centre.x), float(centre.y), float(centre.z)}, 2.f, false, false, 0.f, false);
        log().info("nozzle at {} {} {}: {} entities, exploded", pos.x, pos.y, pos.z, st.entities.size());
        for (auto id : st.entities)
            if (auto* e = entityById(id); e && !e->isPlayer()) e->remove();
        st.entities.clear();
    }
}

} // namespace lv::feature::bulk
