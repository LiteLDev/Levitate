// Crushing wheels (Create's CrushingWheelBlock / CrushingWheelControllerBlock(+Entity)).
//
// Two wheels two blocks apart with the same axis get an invisible controller between them; when
// they turn against each other (opposite speeds) the controller is valid and faces the direction
// items travel through the pair. Entities entering the controller are pulled through: items are
// taken in and crushed (crushing recipes, else milling, else destroyed) and ejected on the far
// side (or inserted into the block there when it accepts direct input); other entities take
// `crushingDamage` per hit.
//
// BlockData of the controller `data["bulk"]`: {"inv": [32 stacks], "time": float,
// "applied": bool, "entity": unique id}.

#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/shared_types/legacy/Difficulty.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/GetCollisionShapeInterface.h"
#include "mc/world/phys/AABB.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::bulk {

namespace blockdata = common::blockdata;
namespace items     = common::items;
using core::kinetics::axisOf;
using core::kinetics::kFaces;

namespace {

constexpr int kSlots = 32;
constexpr char const* kCrushDeath = "death.attack.create.crush";

/// Ejected items don't go back into the controller that ejected them (Create's
/// "BypassCrushingWheel").
std::unordered_map<std::int64_t, GlobalPos>& bypass() {
    static std::unordered_map<std::int64_t, GlobalPos> m;
    return m;
}

/// The entity each controller is pulling through (Create's processingEntity), by position: the
/// collision hook asks for it on every physics query, so it is not read from BlockData there.
std::atomic<std::thread::id> gServerThread{};

std::map<std::tuple<int, int, int>, std::int64_t>& processing() {
    static std::map<std::tuple<int, int, int>, std::int64_t> m;
    return m;
}

ControllerView load(GlobalPos const& pos) {
    ControllerView v;
    v.inventory.assign(kSlots, items::empty());
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains("bulk")) return v;
    auto const& j = (*d)["bulk"];
    v.remaining   = j.value("time", 0.f);
    v.applied     = j.value("applied", false);
    v.entity      = j.value("entity", std::int64_t{0});
    if (auto it = j.find("inv"); it != j.end() && it->is_array()) {
        for (std::size_t i = 0; i < it->size() && i < kSlots; ++i) v.inventory[i] = stackOf((*it)[i]);
    }
    return v;
}

bool inventoryEmpty(ControllerView const& v) {
    return std::all_of(v.inventory.begin(), v.inventory.end(), [](ItemStack const& s) { return items::isEmpty(s); });
}

void save(GlobalPos const& pos, ControllerView const& v) {
    if (v.entity) processing()[{pos.x, pos.y, pos.z}] = v.entity;
    else processing().erase({pos.x, pos.y, pos.z});
    if (inventoryEmpty(v) && !v.entity) {
        if (auto const* d = blockdata::get(pos); d && d->contains("bulk")) blockdata::edit(pos).erase("bulk");
        return;
    }
    Json inv = Json::array();
    int  last = -1;
    for (int i = 0; i < kSlots; ++i)
        if (!items::isEmpty(v.inventory[i])) last = i;
    for (int i = 0; i <= last; ++i) inv.push_back(stackJson(v.inventory[i]));
    blockdata::edit(pos)["bulk"] = Json{
        {"inv",     std::move(inv)},
        {"time",    v.remaining   },
        {"applied", v.applied     },
        {"entity",  v.entity      }
    };
}

void clearInventory(ControllerView& v) {
    v.inventory.assign(kSlots, items::empty());
    v.remaining = 0;
    v.applied   = false;
}

bool occupied(ControllerView const& v) { return v.entity != 0 || !inventoryEmpty(v); }

core::common::ProcessingRecipe const* recipeFor(ItemStack const& stack) {
    if (auto const* r = findRecipe("crushing", stack)) return r;
    return findRecipe("milling", stack);
}

void itemInserted(ControllerView& v) {
    auto const* r = recipeFor(v.inventory[0]);
    v.remaining   = r ? float(r->processingTime > 0 ? r->processingTime : cb::kDefaultProcessingTime) : 100.f;
    v.applied     = false;
}

void applyRecipe(GlobalPos const& pos, ControllerView& v) {
    auto const* r     = recipeFor(v.inventory[0]);
    auto        input = v.inventory[0];
    if (!r) {
        clearInventory(v); // Create: an item without a recipe is destroyed
        v.applied = true;
        log().info("crushing wheels at {} {} {}: destroyed {} x{} (no recipe)", pos.x, pos.y, pos.z, items::idOf(input), items::countOf(input));
        return;
    }
    int  rolls = items::countOf(input);
    auto list  = rollRecipe(*r, rolls);
    clearInventory(v); // Create's ProcessingInventory.clear() also resets the remaining time
    int produced = 0;
    for (std::size_t i = 0; i < list.size() && i + 1 < kSlots; ++i) {
        v.inventory[i + 1] = list[i];
        produced += items::countOf(list[i]);
    }
    log().info("crushing wheels at {} {} {}: crushed {} x{} into {} item(s)", pos.x, pos.y, pos.z, items::idOf(input), rolls, produced);
}

void spawnParticles(GlobalPos const& pos, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    for (int i = 0; i < 4; ++i) {
        cb::Vec3d at{pos.x + random01(), pos.y + random01(), pos.z + random01()};
        legacyParticle(pos.dim, at, 14 /* ParticleType::IconCrack */, (int(stack.getId()) << 16) | (stack.getAuxValue() & 0xffff));
    }
}

/// Blocks that take items straight from a crusher above them (Create's DirectBeltInputBehaviour:
/// belts, depots, ejectors, chutes, millstones, other crushers, ...): Levitate blocks with an
/// item handler. Vanilla containers don't (Create doesn't insert into them either).
bool acceptsDirectInput(BlockSource& region, BlockPos const& p) {
    auto id = idAt(region, p);
    if (!id.starts_with("levitate:")) return false;
    return common::items::has(region, p, -1);
}

bool cellOccupiedBy(GlobalPos const& pos, Actor const& a) {
    cb::Box cell{
        {pos.x + 1e-3, pos.y + 1e-3, pos.z + 1e-3},
        {pos.x + 1 - 1e-3, pos.y + 1 - 1e-3, pos.z + 1 - 1e-3}
    };
    return boxOf(a).intersects(cell);
}

bool mayCrush(Actor& a) {
    if (a.isPlayer()) {
        auto& p = static_cast<Player&>(a);
        if (p.isCreative() || p.getPlayerGameType() == GameType::Spectator) return false;
        auto level = ll::service::getLevel();
        if (level && level->getDifficulty() == SharedTypes::Legacy::Difficulty::Peaceful) return false;
    }
    return true;
}

} // namespace

ControllerView controllerAt(GlobalPos const& pos) { return load(pos); }

float crushingSpeedAt(BlockSource& region, GlobalPos const& pos) {
    auto bp = toBlockPos(pos);
    if (idAt(region, bp) != kController) return 0;
    if (!stateOf(region.getBlock(bp), "levitate:valid")) return 0;
    for (auto f : kFaces) {
        auto n = offset(bp, f);
        if (idAt(region, n) != kWheel) continue;
        if (stateOf(region.getBlock(n), "levitate:axis", 1) == int(axisOf(f))) continue;
        return cb::crushingSpeedOf(kinetics::speedAt(toGlobal(pos.dim, n)));
    }
    return 0;
}

void tickController(BlockSource& region, GlobalPos const& pos) {
    gServerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    auto        bp     = toBlockPos(pos);
    auto const& block  = region.getBlock(bp);
    bool        valid  = stateOf(block, "levitate:valid") != 0;
    auto        facing = faceOf(stateOf(block, "levitate:facing"));
    if (!valid) return; // crushing speed 0: Create's controller does nothing
    float cs = crushingSpeedAt(region, pos);
    if (cs == 0) return;
    auto v = load(pos);

    // entities entering the controller (Create's entityInside -> checkEntityForProcessing)
    if (!occupied(v)) {
        cb::Box cell{
            {pos.x + 0., pos.y + 0., pos.z + 0.},
            {pos.x + 1., pos.y + 1., pos.z + 1.}
        };
        for (auto* a : entitiesIn(region, cell)) {
            if (!cellOccupiedBy(pos, *a)) continue;
            if (auto it = bypass().find(idOf(*a)); it != bypass().end() && it->second == pos) continue;
            if (!mayCrush(*a)) continue;
            v.entity = idOf(*a);
            save(pos, v);
            break;
        }
    }
    if (!occupied(v) || cs == 0) return;
    // Create's tickAudio: the CRUSHING soundscape while something is inside
    addScapeSource(Scape::Crushing, pos, std::clamp(cs / 256.f + .45f, .85f, 1.f));

    auto outPos   = cb::crusherOutPos(pos.x, pos.y, pos.z, facing);
    auto outSpeed = cb::crusherOutSpeed(facing);

    if (!v.entity) {
        float ps = cb::crusherProcessingSpeed(cs, items::countOf(v.inventory[0]), v.applied);
        v.remaining -= ps;
        if (currentTick() % 2 == 0) spawnParticles(pos, v.inventory[0]);
        if (v.remaining < 20 && !v.applied) {
            applyRecipe(pos, v);
            v.applied = true;
            save(pos, v);
            return;
        }
        if (v.remaining > 0) {
            save(pos, v);
            return;
        }
        v.remaining = 0;
        // output into the block below / in front (Create's DirectBeltInputBehaviour)
        if (facing != cb::Face::Up) {
            BlockPos next = BlockPos{bp.x, bp.y - 1, bp.z};
            if (axisOf(facing) != cb::Axis::Y) next = offset(next, facing);
            if (acceptsDirectInput(region, next)) {
                int enterFace = static_cast<int>(core::kinetics::opposite(facing));
                for (auto& s : v.inventory) {
                    if (items::isEmpty(s)) continue;
                    s = common::items::insert(region, next, enterFace, s, false);
                }
                save(pos, v);
                return;
            }
        }
        int ejected = 0;
        for (auto& s : v.inventory) {
            if (items::isEmpty(s)) continue;
            if (auto id = spawnItem(region, s, outPos, outSpeed)) bypass()[id] = pos;
            ejected += items::countOf(s);
        }
        clearInventory(v);
        save(pos, v);
        (void)ejected;
        return;
    }

    auto* e = entityById(v.entity);
    cb::Box reach{
        {pos.x - .5, pos.y - .5, pos.z - .5},
        {pos.x + 1.5, pos.y + 1.5, pos.z + 1.5}
    };
    if (!e || !e->isAlive() || !boxOf(*e).intersects(reach)) {
        v.entity = 0;
        save(pos, v);
        return;
    }
    auto feet   = feetOf(*e);
    auto motion = cb::crusherEntityMotion(facing, cs, feet, pos.x, pos.z, isSneaking(*e));

    if (!isItem(*e)) {
        setMotion(*e, motion);
        int  offsetSign = core::kinetics::isPositive(facing) ? 1 : -1;
        auto n          = cb::normalOf(facing);
        auto entityOut  = outPos + n * (.5 * std::abs(offsetSign));
        int  damage     = config().crushingDamage;
        if (isLiving(*e) && e->getHealth() - damage <= 0) moveTo(*e, entityOut);
        // Create's AllDamageTypes.CRUSH (death message "death.attack.create.crush"), looting II
        hurtBy(*e, float(damage), int(SharedTypes::Legacy::ActorDamageCause::Contact), kCrushDeath);
        if (!e->isAlive() || e->isDead()) setCrushingVictim(e);
        if (!e->isAlive() || e->isDead()) {
            moveTo(*e, entityOut);
            // Create: the drops of a crushed mob keep no motion (they fall into place)
            cb::Box around{entityOut - cb::Vec3d{1.5, 1.5, 1.5}, entityOut + cb::Vec3d{1.5, 1.5, 1.5}};
            for (auto* drop : entitiesIn(region, around))
                if (isItem(*drop) && static_cast<ItemActor*>(drop)->mAge <= 1) setMotion(*drop, {});
            log().info("crushing wheels at {} {} {}: crushed {}", pos.x, pos.y, pos.z, e->getTypeName());
        }
        return;
    }

    // items are pulled through the controller (moved directly: the controller's collision slab
    // holds every other entity)
    static_cast<ItemActor*>(e)->pickupDelay() = 20;
    auto next = feet + motion;
    moveTo(*e, next);
    setMotion(*e, {});
    if (cb::crusherIntakeReached(facing, next, pos.x, pos.y, pos.z)) {
        auto* stack = itemOf(*e);
        clearInventory(v);
        if (stack) v.inventory[0] = *stack;
        itemInserted(v);
        e->remove();
        v.entity = 0;
        save(pos, v);
    }
}

// ---- wheels -----------------------------------------------------------------------------------------------------

void fixControllers(BlockSource& region, GlobalPos const& pos) {
    auto bp = toBlockPos(pos);
    if (idAt(region, bp) != kWheel) return;
    int  axis  = stateOf(region.getBlock(bp), "levitate:axis", 1);
    for (auto side : kFaces) {
        if (int(axisOf(side)) == axis) continue;
        auto ctrl  = offset(bp, side);
        auto other = offset(bp, side, 2);
        if (!loaded(region, ctrl) || !loaded(region, other)) continue;
        bool exists     = idAt(region, ctrl) == kController;
        bool wasValid   = exists && stateOf(region.getBlock(ctrl), "levitate:valid");
        int  oldFacing  = exists ? stateOf(region.getBlock(ctrl), "levitate:facing") : -1;
        bool shouldExist = false, shouldBeValid = false;
        auto newFacing   = cb::Face::Down;
        if (idAt(region, other) == kWheel) {
            shouldExist = true;
            float s1    = kinetics::speedAt(pos);
            float s2    = kinetics::speedAt(toGlobal(pos.dim, other));
            if ((s1 > 0) != (s2 > 0) && s1 != 0 && s2 != 0) {
                newFacing     = cb::controllerFacing(static_cast<cb::Axis>(axis), side, s1);
                shouldBeValid = true;
            }
            if (stateOf(region.getBlock(other), "levitate:axis", 1) != axis) shouldExist = false;
        }
        if (!shouldExist) {
            if (exists) game::clearBlock(region, ctrl);
            continue;
        }
        if (!exists) {
            if (!game::replaceable(region, ctrl)) continue;
            auto const* b = game::blockWith(kController, {{"levitate:valid", shouldBeValid ? 1 : 0}, {"levitate:facing", int(newFacing)}});
            if (b) game::setBlock(region, ctrl, *b);
            track(toGlobal(pos.dim, ctrl), Kind::Controller);
        } else if (wasValid != shouldBeValid || oldFacing != int(newFacing)) {
            setStates(region, ctrl, {{"levitate:valid", shouldBeValid ? 1 : 0}, {"levitate:facing", int(newFacing)}});
        }
    }
}

void removeControllersAround(BlockSource& region, GlobalPos const& pos, int /*axis*/) {
    auto bp = toBlockPos(pos);
    for (auto f : kFaces) {
        auto c = offset(bp, f);
        if (idAt(region, c) != kController) continue;
        // a controller stays only between two wheels with the same axis across it
        bool supported = false;
        for (auto g : kFaces) {
            if (!core::kinetics::isPositive(g)) continue;
            auto a = offset(c, g), b = offset(c, core::kinetics::opposite(g));
            if (idAt(region, a) != kWheel || idAt(region, b) != kWheel) continue;
            int ax = stateOf(region.getBlock(a), "levitate:axis", 1);
            if (ax == stateOf(region.getBlock(b), "levitate:axis", 1) && ax != int(axisOf(g))) supported = true;
        }
        if (!supported) game::clearBlock(region, c);
    }
}

bool wheelCanSurvive(BlockSource& region, BlockPos const& p) { return wheelCanSurvive(region, p, stateOf(region.getBlock(p), "levitate:axis", 1)); }

bool wheelCanSurvive(BlockSource& region, BlockPos const& p, int axis) {
    for (auto d : kFaces) {
        auto n  = offset(p, d);
        auto id = idAt(region, n);
        if (id == kController && int(axisOf(d)) != axis) return false;
        if (id != kWheel) continue;
        if (stateOf(region.getBlock(n), "levitate:axis", 1) != axis || axis != int(axisOf(d))) return false;
    }
    return true;
}

void clearCrushers() {
    bypass().clear();
    processing().clear();
}

// ---- collision (Create's CrushingWheelControllerBlock.getCollisionShape) -----------------------------------

namespace {

bool sameBox(AABB const& a, AABB const& b) { return a.min == b.min && a.max == b.max; }

/// The entity of `id` is the one asking (the shape interface carries no entity, only its box).
bool isAsking(std::int64_t id, AABB const& box) {
    auto* a = entityById(id);
    return a && sameBox(a->getAABB(), box);
}

/// Whether the controller at `p` has no collision for the entity whose box is `box`: the entity
/// being crushed falls through, and items it ejected land on top of it unless it faces up.
bool passesThrough(BlockPos const& p, int facing, AABB const& box) {
    if (auto it = processing().find({p.x, p.y, p.z}); it != processing().end() && isAsking(it->second, box)) return true;
    if (facing == int(cb::Face::Up)) return false;
    for (auto const& [id, at] : bypass())
        if (at.x == p.x && at.y == p.y && at.z == p.z && isAsking(id, box)) return true;
    return false;
}

BlockType const* controllerType() {
    static BlockType const* type = nullptr;
    if (!type)
        if (auto t = BlockType::tryGetFromRegistry(HashedString(std::string(kController)))) type = &*t;
    return type;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    CrusherCollisionHook,
    ll::memory::HookPriority::Normal,
    BlockType,
    &BlockType::$addCollisionShapes,
    bool,
    ::Block const&                                     block,
    ::IConstBlockSource const&                         region,
    ::BlockPos const&                                  pos,
    ::AABB const*                                      intersectTestBox,
    ::std::vector<::AABB>&                             inoutBoxes,
    ::optional_ref<::GetCollisionShapeInterface const> entity
) {
    // server thread only (the client of a local world queries its own level on another thread)
    if (entity && std::this_thread::get_id() == gServerThread.load(std::memory_order_relaxed)
        && (!processing().empty() || !bypass().empty()) && this == controllerType() && stateOf(block, "levitate:valid")
        && passesThrough(pos, stateOf(block, "levitate:facing"), entity->getAABB()))
        return false;
    return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
}

void installCrusherCollision() { CrusherCollisionHook::hook(); }
void uninstallCrusherCollision() { CrusherCollisionHook::unhook(); }

void pruneBypass() {
    std::erase_if(bypass(), [](auto const& kv) { return entityById(kv.first) == nullptr; });
}

// ---- capability ---------------------------------------------------------------------------------------------------

namespace controller_cap {

int slots() { return kSlots; }

ItemStack getSlot(GlobalPos const& pos, int slot) {
    auto v = load(pos);
    return slot >= 0 && slot < kSlots ? v.inventory[slot] : items::empty();
}

ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    if (items::isEmpty(stack)) return stack;
    auto v = load(pos);
    if (v.entity || !inventoryEmpty(v)) return stack; // Create: only into an empty inventory
    int move = std::min(slotLimit(stack), items::countOf(stack));
    if (!simulate) {
        clearInventory(v);
        v.inventory[0] = items::withCount(stack, move);
        itemInserted(v);
        save(pos, v);
    }
    return items::withCount(stack, items::countOf(stack) - move);
}

} // namespace controller_cap

} // namespace lv::feature::bulk
