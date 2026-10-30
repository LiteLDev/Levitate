// Deployer (Create's DeployerBlock / DeployerBlockEntity / DeployerHandler / DeployerItemHandler /
// BeltDeployerCallbacks / DeployerMovementBehaviour), SPEC §3.6.15.
//
// A kinetic block that, while turning, reaches its hand out two blocks in front of it and uses or
// punches with the item it holds, through the dimension's deployer player (FakePlayer.cpp).
// Per-position state (mode, held item, overflow) lives in BlockData under "deployer"; the running
// state (timer, phase) is kept in memory and written on every phase change.

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "core/assembly/Deployer.h"
#include "feature/assembly/AssemblyInternal.h"
#include "feature/assembly/SequencedAssembly.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::assembly::deployer {

namespace bd      = common::blockdata;
namespace items   = common::items;
namespace recipes = common::recipes;
namespace sa      = sequenced;
using ca::DeployerMode;
using ca::DeployerState;

namespace {

constexpr char kDbIndex[]    = "lv:assembly:deployers";
constexpr char kDataKey[]    = "deployer";
constexpr char kCarrier[]    = "assembly:deployer";

/// Blocks that carry items for the deployer to process from above (Create's
/// TransportedItemStackHandlerBehaviour: belts, depots, ejectors).
bool isTransportBlock(std::string const& id) {
    return id == "levitate:depot" || id == "levitate:belt" || id == "levitate:weighted_ejector";
}

struct Runtime {
    DeployerState                              state = DeployerState::Waiting;
    int                                        timer = 0;
    float                                      reach = 0.5f;
    DeployerMode                               mode  = DeployerMode::Use;
    ItemStack                                  held;
    std::vector<ItemStack>                     overflow;
    std::optional<std::pair<BlockPos, float>>  breaking;
    bool                                       redstone = false;
    bool                                       fistBump = false; ///< Create's fistBump
    ItemStack                                  synced; ///< `held` as last sent to the client mod
};

struct State {
    std::map<GlobalPos, Runtime> runtimes;
    std::set<GlobalPos>          index;
    bool                         indexDirty = false;
    kinetics::ListenerId         speedListener = -1;
};
State& st() {
    static State s;
    return s;
}

// ---- persistence ------------------------------------------------------------------------------

Json toJson(Runtime const& r) {
    Json j{{"mode", static_cast<int>(r.mode)}, {"state", static_cast<int>(r.state)}, {"reach", r.reach}};
    if (!items::isEmpty(r.held)) j["held"] = items::toJson(r.held);
    Json of = Json::array();
    for (auto const& s : r.overflow) of.push_back(items::toJson(s));
    j["overflow"] = std::move(of);
    if (r.breaking) j["breaking"] = {r.breaking->first.x, r.breaking->first.y, r.breaking->first.z, r.breaking->second};
    return j;
}

Runtime fromJson(Json const& j) {
    Runtime r;
    if (!j.is_object()) return r;
    r.mode  = j.value("mode", 1) == 0 ? DeployerMode::Punch : DeployerMode::Use;
    r.state = static_cast<DeployerState>(std::clamp(j.value("state", 0), 0, 3));
    r.reach = j.value("reach", 0.5f);
    if (r.state != DeployerState::Waiting) r.timer = ca::kDeployerCycle;
    if (auto it = j.find("held"); it != j.end()) r.held = items::fromJson(*it);
    if (auto it = j.find("overflow"); it != j.end() && it->is_array())
        for (auto const& e : *it)
            if (auto s = items::fromJson(e); !items::isEmpty(s)) r.overflow.push_back(s);
    if (auto it = j.find("breaking"); it != j.end() && it->is_array() && it->size() == 4)
        r.breaking = std::pair{BlockPos{(*it)[0].get<int>(), (*it)[1].get<int>(), (*it)[2].get<int>()}, (*it)[3].get<float>()};
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
    if (auto const* d = bd::get(pos)) {
        if (auto it = d->find(kDataKey); it != d->end()) r = fromJson(*it);
    }
    track(pos);
    return m.emplace(pos, std::move(r)).first->second;
}

void save(GlobalPos const& pos, Runtime const& r) { bd::edit(pos)[kDataKey] = toJson(r); }

Face facingOf(Block const& block) { return static_cast<Face>(std::clamp(detail::intState(block, "levitate:facing").value_or(1), 0, 5)); }

void syncState(GlobalPos const& pos, Runtime const& r, Face facing) {
    float speed = kinetics::speedAt(pos);
    sync::deployer(
        pos,
        {{"state", static_cast<int>(r.state)},
         {"timer", r.timer},
         {"speed", ca::deployerTimerSpeed(speed)},
         {"reach", r.reach},
         {"pose", static_cast<int>(ca::deployerHandPose(r.mode, !items::isEmpty(r.held)))},
         {"facing", static_cast<int>(facing)},
         {"held", items::isEmpty(r.held) ? Json() : items::toJson(r.held)}}
    );
}

bool sameStack(ItemStack const& a, ItemStack const& b) {
    return items::sameItemSameData(a, b) && items::countOf(a) == items::countOf(b);
}

float random01() {
    static std::mt19937                   rng{std::random_device{}()};
    std::uniform_real_distribution<float> d(0.f, 1.f);
    return d(rng);
}

// ---- recipes on items ---------------------------------------------------------------------------

bool keepHeld(core::common::ProcessingRecipe const& r) {
    return r.raw.value("keep_held_item", r.raw.value("keepHeldItem", false));
}

void useUpHeld(ItemStack& held) {
    if (held.isDamageableItem()) {
        auto const* item = held.mItem.get();
        short       max  = item ? item->getMaxDamage() : 0;
        held.hurtAndBreak(1, nullptr);
        if (held.isNull() || (max > 0 && held.getDamageValue() >= max)) held = items::empty();
        return;
    }
    held = items::withCount(held, items::countOf(held) - 1);
}

} // namespace

std::optional<std::vector<ItemStack>> deployOnItem(ItemStack const& item, ItemStack& held, bool simulate) {
    if (items::isEmpty(item) || items::isEmpty(held)) return std::nullopt;
    auto one = items::withCount(item, 1);
    // sand paper polishes (Create's SandPaperPolishingRecipe through the deployer)
    auto heldId = items::idOf(held);
    if (heldId == "levitate:sand_paper" || heldId == "levitate:red_sand_paper") {
        if (auto const* r = recipes::findFirst("sandpaper_polishing", one)) {
            if (simulate) return std::vector<ItemStack>{};
            auto out = recipes::rollResults(*r);
            useUpHeld(held);
            return out;
        }
    }
    // sequenced assembly first (Create's getRecipe asks SequencedAssemblyRecipe.getRecipe first)
    if (auto step = sa::findDeployingStep(one, held)) {
        if (simulate) return std::vector<ItemStack>{};
        auto out = sa::advance(*step, one);
        if (!sa::keepsHeldItem(*step)) useUpHeld(held);
        std::vector<ItemStack> v;
        if (!items::isEmpty(out)) v.push_back(out);
        return v;
    }
    // deploying recipes, then item application recipes (Create converts those into deploying ones)
    for (auto const* type : {"deploying", "item_application"}) {
        for (auto const& r : recipes::ofType(type)) {
            if (r.ingredients.size() < 2) continue;
            if (!recipes::matches(r.ingredients[0], one) || !recipes::matches(r.ingredients[1], held)) continue;
            if (simulate) return std::vector<ItemStack>{};
            auto out = recipes::rollResults(r);
            if (!keepHeld(r)) useUpHeld(held);
            return out;
        }
    }
    return std::nullopt;
}

namespace {

// ---- activation through the deployer player ---------------------------------------------------

struct Hand {
    int                                        dim = 0;
    double                                     cx = 0, cy = 0, cz = 0; ///< deployer centre
    double                                     dx = 0, dy = 0, dz = 0; ///< unit vector it faces
    Face                                       facing = Face::Up;
    BlockPos                                   clicked;
    DeployerMode                               mode = DeployerMode::Use;
};

void captureDrops(BlockSource& region, double x0, double y0, double z0, double x1, double y1, double z1, std::vector<ItemStack>& into) {
    AABB box{Vec3{float(x0), float(y0), float(z0)}, Vec3{float(x1), float(y1), float(z1)}};
    for (auto* a : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!a || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto& ia = static_cast<ItemActor&>(*a);
        if (ia.mAge > 1) continue; // only what this action just dropped
        auto stack = ia.item();
        if (items::isEmpty(stack)) continue;
        into.push_back(stack);
        a->remove();
    }
}

/// Create's DeployerHandler.activate: one action with `held`. Items the player ends up with
/// besides the held stack go to `extra`.
void act(Hand const& h, ItemStack& held, std::vector<ItemStack>& extra, std::optional<std::pair<BlockPos, float>>& breaking) {
    auto* region = detail::regionOf(h.dim);
    auto* fp     = fakeplayer::get(h.dim);
    if (!region || !fp) return;
    auto& inv = fp->getInventory();
    auto  clearOthers = [&](int from) {
        for (int i = from; i < inv.getContainerSize(); ++i) {
            if (auto const& s = inv.getItem(i); !items::isEmpty(s)) extra.push_back(s);
            inv.setItem(i, items::empty());
        }
        // equipment the action put on (armour, shields) is handed out too (Create: tryDisposeOfExcess)
        if (auto const& s = fp->getOffhandSlot(); !items::isEmpty(s)) {
            extra.push_back(s);
            fp->setOffhandSlot(items::empty());
        }
        for (int a = 0; a < 4; ++a) {
            auto slot = static_cast<SharedTypes::Legacy::ArmorSlot>(a);
            if (auto const& s = fp->getArmor(slot); !items::isEmpty(s)) {
                extra.push_back(s);
                fp->setArmor(slot, items::empty());
            }
        }
    };
    clearOthers(0);
    inv.setItem(0, held);
    fp->setSelectedSlot(0);

    // stand in the hand: the eye 1.5 blocks out, looking where the deployer faces
    double ox = h.cx + h.dx * (1.5 + 1 / 64.0), oy = h.cy + h.dy * (1.5 + 1 / 64.0), oz = h.cz + h.dz * (1.5 + 1 / 64.0);
    float  pitch = h.facing == Face::Up ? -90.f : h.facing == Face::Down ? 90.f : 0.f;
    float  yaw   = static_cast<float>(std::atan2(-h.dx, h.dz) * 180.0 / 3.14159265358979);
    fp->teleport(Vec3{float(ox), float(oy - 1.62), float(oz)}, DimensionType(h.dim), Vec2{pitch, yaw});

    auto&    gm   = *fp->mGameMode;
    Face     face = core::kinetics::opposite(h.facing);
    BlockPos c    = h.clicked;
    bool     done = false;

    // entities in the target block first
    {
        AABB box{Vec3{float(c.x), float(c.y), float(c.z)}, Vec3{float(c.x + 1), float(c.y + 1), float(c.z + 1)}};
        std::vector<Actor*> targets;
        for (auto* a : region->getEntities(box, 0.f)) {
            if (!a || a == fp || a->getEntityTypeId() == ActorType::FallingBlock) continue;
            if (a->isPlayer() && fakeplayer::isFake(static_cast<Player&>(*a))) continue;
            targets.push_back(a);
        }
        if (!targets.empty()) {
            Actor& target = *targets[static_cast<std::size_t>(random01() * targets.size()) % targets.size()];
            Vec3   at     = target.getPosition();
            bool   ok     = false;
            if (h.mode == DeployerMode::Use) {
                ok = gm.interact(target, at, HandSlot::Mainhand);
                if (!ok && target.isPlayer()) {
                    // feeding players (Create: food is eaten by the target)
                    auto const& cur  = inv.getItem(0);
                    auto const* item = cur.mItem.get();
                    if (item && item->isFood()) {
                        static_cast<Player&>(target).eat(cur);
                        inv.setItem(0, items::withCount(cur, items::countOf(cur) - 1));
                        ok = true;
                    }
                }
            } else {
                gm.attack(target, at);
                ok = true;
            }
            captureDrops(*region, at.x - 1.5, at.y - 1.5, at.z - 1.5, at.x + 1.5, at.y + 2.5, at.z + 1.5, extra);
            done = ok;
        }
    }

    if (!done && h.mode == DeployerMode::Punch) {
        auto const& block = region->getBlock(c);
        if (block.isAir()) {
            breaking.reset();
        } else {
            auto        firePos = detail::offsetPos(c, face);
            std::string fireId  = region->getBlock(firePos).getTypeName();
            if (fireId == "minecraft:fire" || fireId == "minecraft:soul_fire") {
                game::clearBlock(*region, firePos);
                detail::playSound(h.dim, firePos.x + .5, firePos.y + .5, firePos.z + .5, "random.fizz", .5f, 2.6f);
            } else {
                float before   = breaking && breaking->first == c ? breaking->second : 0.f;
                float progress = fp->getDestroyProgress(block) * ca::kDeployerBreakFactor + before;
                if (progress >= 1.f) {
                    // harvest as the player would (tool, enchantments), without the survival
                    // break timing checks GameMode applies to network players
                    bool destroyed = false;
                    gm.startDestroyBlock(c, static_cast<uchar>(face), destroyed);
                    if (!destroyed && region->getBlock(c).getTypeName() == block.getTypeName()) {
                        auto&       tool     = const_cast<ItemStack&>(inv.getItem(0));
                        auto const* toolItem = items::isEmpty(tool) ? nullptr : tool.mItem.get();
                        bool        harvest  = !block.getBlockType().mRequiresCorrectToolForDrops || (toolItem && toolItem->canDestroySpecial(block));
                        if (harvest) block.playerDestroy(*fp, c);
                        game::clearBlock(*region, c);
                        if (!items::isEmpty(tool) && tool.isDamageableItem()) {
                            tool.hurtAndBreak(1, fp);
                            if (tool.isNull()) inv.setItem(0, items::empty());
                        }
                    }
                    captureDrops(*region, c.x - .5, c.y - .5, c.z - .5, c.x + 1.5, c.y + 1.5, c.z + 1.5, extra);
                    breaking.reset();
                } else if (progress <= 0.f) {
                    breaking.reset();
                } else {
                    breaking = std::pair{c, progress};
                }
            }
        }
    } else if (!done) {
        auto& slot0  = const_cast<ItemStack&>(inv.getItem(0));
        auto  useOn  = [&](BlockPos const& at, Face f) {
            auto o = core::kinetics::offsetOf(f);
            Vec3 hit{0.5f + o.x * 0.5f, 0.5f + o.y * 0.5f, 0.5f + o.z * 0.5f};
            auto const& target = region->getBlock(at);
            return gm.useItemOn(slot0, at, static_cast<uchar>(f), hit, HandSlot::Mainhand, &target, true).mSuccess;
        };
        bool used = useOn(c, face);
        // a block item facing air: place it against a neighbour of the target (Create's ray
        // places into the empty block in front; Bedrock needs a face to click)
        if (!used && !items::isEmpty(slot0) && game::replaceable(*region, c) && game::blockWith(items::idOf(slot0)) != nullptr) {
            for (Face side : {h.facing, Face::Down, Face::North, Face::South, Face::West, Face::East, Face::Up}) {
                if (side == face) continue;
                auto n = detail::offsetPos(c, side);
                if (!region->hasChunksAt(n, 0, false) || game::replaceable(*region, n)) continue;
                if ((used = useOn(n, core::kinetics::opposite(side)))) break;
            }
            if (!used) {
                // nothing to place against: Create places into the air in front
                if (auto const* b = game::blockWith(items::idOf(slot0)); b && game::setBlock(*region, c, *b)) {
                    inv.setItem(0, items::withCount(slot0, items::countOf(slot0) - 1));
                    used = true;
                }
            }
        }
        if (!used && !items::isEmpty(slot0) && items::idOf(slot0) != "minecraft:ender_pearl") {
            gm.useItem(slot0, HandSlot::Mainhand);
            gm.releaseUsingItem();
        }
    }

    held = inv.getItem(0);
    if (items::isEmpty(held)) held = items::empty();
    clearOthers(1);
    inv.setItem(0, items::empty());
    // wait out of reach of item entities
    fp->teleport(Vec3{float(ox), 400.f, float(oz)}, DimensionType(h.dim), Vec2{0.f, 0.f});
}

Hand handOf(GlobalPos const& pos, Face facing, DeployerMode mode) {
    Hand h;
    h.dim    = pos.dim;
    h.cx     = pos.x + .5;
    h.cy     = pos.y + .5;
    h.cz     = pos.z + .5;
    auto o   = core::kinetics::offsetOf(facing);
    h.dx     = o.x;
    h.dy     = o.y;
    h.dz     = o.z;
    h.facing = facing;
    h.clicked = BlockPos{pos.x + o.x * 2, pos.y + o.y * 2, pos.z + o.z * 2};
    h.mode   = mode;
    return h;
}

/// Create's DeployerHandler.shouldActivate.
bool shouldActivate(BlockSource& region, ItemStack const& held, BlockPos const& target, Face facing) {
    if (!region.hasChunksAt(target, 0, false)) return false;
    auto const& block = region.getBlock(target);
    if (!items::isEmpty(held)) {
        std::string id = items::idOf(held);
        // a block item facing the same block (nothing to place)
        if (block.getTypeName() == id) return false;
        // a bucket facing its own fluid
        if (id == "minecraft:water_bucket" && (block.getTypeName() == "minecraft:water" || block.getTypeName() == "minecraft:flowing_water")) return false;
        if (id == "minecraft:lava_bucket" && (block.getTypeName() == "minecraft:lava" || block.getTypeName() == "minecraft:flowing_lava")) return false;
        if (facing == Face::Down && isTransportBlock(block.getTypeName())) return false;
    }
    return true;
}

/// Depots and belts under a deployer facing down: Create's BeltDeployerCallbacks through the
/// item capability (slot 0 is the item on the depot / belt segment).
bool transportItemHasRecipe(BlockSource& region, BlockPos const& target, ItemStack held) {
    common::Access at{region, target, 1};
    auto           handler = items::find(at);
    if (!handler || !handler->getSlot || !handler->slots || handler->slots(at) <= 0) return false;
    auto stack = handler->getSlot(at, 0);
    if (items::isEmpty(stack)) return false;
    return deployOnItem(stack, held, true).has_value();
}

void processTransportItem(GlobalPos const& pos, Runtime& r, BlockSource& region, BlockPos const& target) {
    common::Access at{region, target, 1};
    auto           handler = items::find(at);
    if (!handler || !handler->getSlot || !handler->extract || !handler->insert) return;
    auto           stack = handler->getSlot(at, 0);
    if (items::isEmpty(stack) || !deployOnItem(stack, r.held, true)) return;
    auto pred = [stack](ItemStack const& s) { return items::sameItemSameData(s, stack); };
    auto one  = handler->extract(at, pred, 1, false);
    if (items::isEmpty(one)) return;
    std::string heldId  = items::idOf(r.held); // logged after the held item may be used up
    auto        outputs = deployOnItem(one, r.held, false);
    if (!outputs || outputs->empty()) {
        // nothing came out: the item goes back where it was
        auto rest = handler->insert(at, one, false);
        if (!items::isEmpty(rest)) detail::dropItem(region, target.x + .5, target.y + 1.0, target.z + .5, rest);
        if (!outputs) return;
    }
    for (auto const& o : *outputs) {
        auto rest = handler->insert(at, o, false);
        if (!items::isEmpty(rest)) detail::dropItem(region, target.x + .5, target.y + 1.0, target.z + .5, rest);
    }
    if (items::isEmpty(r.held)) detail::playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.break", .25f, 1.f);
    detail::playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.pop", .25f, .75f);
    if (int more = 0; detail::logAction("deployer.apply@" + pos.key() + "#" + items::idOf(one), more))
        log().info("deployer at {} applied {} to {}{}", pos.key(), heldId, items::idOf(one), detail::similar(more));
}

void start(GlobalPos const& pos, Runtime& r, BlockSource& region, Face facing) {
    r.state = DeployerState::Expanding;
    // the reach: how far the hand gets before it hits something in the block in front
    auto     front = detail::offsetPos(game::blockPos(pos), facing, 2);
    auto const& b  = region.getBlock(front);
    float    hitDistance = b.isAir() ? 1.f : 0.f;
    r.reach = ca::deployerReach(hitDistance - 1 / 64.f);
    r.timer = ca::kDeployerCycle;
    (void)fakeplayer::get(pos.dim); // make sure the player exists when the hand arrives
    save(pos, r);
    syncState(pos, r, facing);
}

void activate(GlobalPos const& pos, Runtime& r, BlockSource& region, Face facing) {
    auto bp     = game::blockPos(pos);
    auto target = detail::offsetPos(bp, facing, 2);
    if (facing == Face::Down && region.hasChunksAt(target, 0, false) && isTransportBlock(region.getBlock(target).getTypeName())) {
        if (r.mode == DeployerMode::Use) processTransportItem(pos, r, region, target);
        return;
    }
    std::vector<ItemStack> extra;
    ItemStack              before = r.held;
    act(handOf(pos, facing, r.mode), r.held, extra, r.breaking);
    for (auto& e : extra) r.overflow.push_back(e);
    if (!sameStack(before, r.held) || !extra.empty()) {
        if (int more = 0; detail::logAction("deployer.use@" + pos.key() + "#" + items::idOf(before), more))
            log().info("deployer at {} used {} ({} item(s) to hand out){}", pos.key(), items::idOf(before), extra.size(), detail::similar(more));
    }
}

// ---- fist bump (Create's startFistBump / triggerFistBump) --------------------------------------------

int gFistBumps = 0;

/// The deployer 2..4 blocks in front (the first one found) and its distance.
std::optional<std::pair<GlobalPos, int>> partnerOf(BlockSource& region, GlobalPos const& pos, Face facing) {
    auto bp = game::blockPos(pos);
    for (int i = 2; i < 5; ++i) {
        auto other = detail::offsetPos(bp, facing, i);
        if (!region.hasChunksAt(other, 0, false)) return std::nullopt;
        if (region.getBlock(other).getTypeName() == kDeployerId) return std::make_pair(game::globalPos(pos.dim, other), i);
    }
    return std::nullopt;
}

/// Two punching deployers facing each other 2..4 blocks apart reach out to meet half way.
bool startFistBump(BlockSource& region, GlobalPos const& pos, Face facing) {
    auto partner = partnerOf(region, pos, facing);
    if (!partner) return false;
    auto const& other = region.getBlock(game::blockPos(partner->first));
    if (facingOf(other) != core::kinetics::opposite(facing)) return false;
    auto& pr = rt(partner->first);
    if (pr.mode != DeployerMode::Punch || kinetics::speedAt(partner->first) == 0) return false;
    for (auto const& [p, f] : {std::make_pair(pos, facing), std::make_pair(partner->first, core::kinetics::opposite(facing))}) {
        auto& r    = rt(p);
        r.fistBump = true;
        r.reach    = (partner->second - 2) * .5f;
        r.timer    = ca::kDeployerCycle;
        r.state    = DeployerState::Expanding;
        save(p, r);
        syncState(p, r, f);
    }
    return true;
}

/// Both hands out: the bump (the partner retracts now, this one after its activation).
void triggerFistBump(BlockSource& region, GlobalPos const& pos, Runtime& r, Face facing) {
    auto partner = partnerOf(region, pos, facing);
    if (!partner) return;
    auto& pr = rt(partner->first);
    if (!pr.fistBump || pr.state != DeployerState::Expanding || pr.timer > 0) return;
    r.fistBump  = false;
    pr.fistBump = false;
    pr.state    = DeployerState::Retracting;
    pr.timer    = ca::kDeployerCycle;
    save(partner->first, pr);
    syncState(partner->first, pr, core::kinetics::opposite(facing));
    auto const& a = pos;
    auto const& b = partner->first;
    detail::playSound(pos.dim, std::floor((a.x + b.x + 1) * .5) + .5, std::floor((a.y + b.y + 1) * .5) + .5, std::floor((a.z + b.z + 1) * .5) + .5,
                      "game.player.attack.nodamage", .75f, .75f);
    ++gFistBumps;
    log().info("deployers at {} and {} fist bumped", a.key(), b.key());
}

void tickOne(GlobalPos const& pos) {
    auto* region = detail::regionOf(pos.dim);
    if (!region) return;
    auto bp = game::blockPos(pos);
    if (!region->hasChunksAt(bp, 0, false)) return;
    auto const& block = region->getBlock(bp);
    if (block.getTypeName() != kDeployerId) {
        st().runtimes.erase(pos);
        if (st().index.erase(pos)) st().indexDirty = true;
        return;
    }
    // the client mod draws the held item in the hand: tell it when the item changed
    if (auto it = st().runtimes.find(pos); it != st().runtimes.end() && !sameStack(it->second.synced, it->second.held)) {
        it->second.synced = it->second.held;
        syncState(pos, it->second, facingOf(block));
    }
    float speed = kinetics::speedAt(pos);
    if (speed == 0) return;
    int   ts = ca::deployerTimerSpeed(speed);
    auto& r  = rt(pos);
    if (r.timer > 0) {
        r.timer -= ts;
        return;
    }
    Face facing = facingOf(block);
    switch (r.state) {
    case DeployerState::Waiting: {
        if (!r.overflow.empty()) {
            r.timer = ts * 10;
            return;
        }
        if (!items::isEmpty(r.held) && !common::filterslot::matches(pos, r.held)) {
            // a held item the filter rejects is handed out
            r.overflow.push_back(r.held);
            r.held  = items::empty();
            r.timer = ts * 10;
            save(pos, r);
            syncState(pos, r, facing);
            return;
        }
        r.redstone  = detail::powered(*region, bp);
        auto target = detail::offsetPos(bp, facing, 2);
        if (r.mode == DeployerMode::Use && facing == Face::Down && region->hasChunksAt(target, 0, false)
            && isTransportBlock(region->getBlock(target).getTypeName()) && !items::isEmpty(r.held)) {
            // belt / depot below: start when its item can be deployed on
            if (r.redstone || !transportItemHasRecipe(*region, target, r.held)) return;
            start(pos, r, *region, facing);
            return;
        }
        if (r.mode == DeployerMode::Use && !shouldActivate(*region, r.held, target, facing)) {
            r.timer = ts * 10;
            return;
        }
        // Create checks the advancement condition before the redstone lock
        if (r.mode == DeployerMode::Punch && !r.fistBump && startFistBump(*region, pos, facing)) return;
        if (r.redstone) return;
        start(pos, r, *region, facing);
        return;
    }
    case DeployerState::Expanding:
        if (r.fistBump) {
            // the hands meet in the gap: nothing in front is punched
            triggerFistBump(*region, pos, r, facing);
            r.fistBump = false;
        } else {
            activate(pos, r, *region, facing);
        }
        r.state = DeployerState::Retracting;
        r.timer = ca::kDeployerCycle;
        save(pos, r);
        syncState(pos, r, facing);
        return;
    case DeployerState::Retracting:
    case DeployerState::Dumping:
        r.state = DeployerState::Waiting;
        r.timer = ca::kDeployerPause;
        save(pos, r);
        syncState(pos, r, facing);
        return;
    }
}

// ---- item capability (Create's DeployerItemHandler) ---------------------------------------------

common::items::Handler capability() {
    common::items::Handler h;
    h.slots = [](common::Access const& at) {
        auto pos = game::globalPos(at.region, at.pos);
        return 1 + static_cast<int>(rt(pos).overflow.size());
    };
    h.getSlot = [](common::Access const& at, int slot) -> ItemStack {
        auto& r = rt(game::globalPos(at.region, at.pos));
        if (slot >= 0 && slot < static_cast<int>(r.overflow.size())) return r.overflow[static_cast<std::size_t>(slot)];
        return slot == static_cast<int>(r.overflow.size()) ? r.held : items::empty();
    };
    h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(at.region, at.pos);
        auto& r   = rt(pos);
        if (items::isEmpty(stack) || !common::filterslot::matches(pos, stack)) return stack;
        if (items::isEmpty(r.held)) {
            if (!simulate) {
                r.held = stack;
                save(pos, r);
                syncState(pos, r, facingOf(at.region.getBlock(at.pos)));
            }
            return items::empty();
        }
        if (!items::sameItemSameData(r.held, stack)) return stack;
        int limit = std::min<int>(r.held.getMaxStackSize(), 64);
        int add   = std::min(limit - items::countOf(r.held), items::countOf(stack));
        if (add <= 0) return stack;
        if (!simulate) {
            r.held = items::withCount(r.held, items::countOf(r.held) + add);
            save(pos, r);
        }
        return items::withCount(stack, items::countOf(stack) - add);
    };
    h.extract = [](common::Access const& at, common::items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(at.region, at.pos);
        auto& r   = rt(pos);
        for (std::size_t i = 0; i < r.overflow.size(); ++i) {
            auto const& s = r.overflow[i];
            if (!filter(s)) continue;
            int  take = std::min(amount, items::countOf(s));
            auto out  = items::withCount(s, take);
            if (!simulate) {
                if (take >= items::countOf(s)) r.overflow.erase(r.overflow.begin() + static_cast<long long>(i));
                else r.overflow[i] = items::withCount(s, items::countOf(s) - take);
                save(pos, r);
            }
            return out;
        }
        if (items::isEmpty(r.held) || !filter(r.held)) return items::empty();
        // a held item the filter asks for stays in the hand
        if (!items::isEmpty(common::filterslot::get(pos)) && common::filterslot::matches(pos, r.held)) return items::empty();
        int  take = std::min(amount, items::countOf(r.held));
        auto out  = items::withCount(r.held, take);
        if (!simulate) {
            r.held = items::withCount(r.held, items::countOf(r.held) - take);
            save(pos, r);
            syncState(pos, r, facingOf(at.region.getBlock(at.pos)));
        }
        return out;
    };
    return h;
}

// ---- contraption actor (Create's DeployerMovementBehaviour) --------------------------------------

contraption::CarriedBlock* carried(contraption::MovementContext& ctx) {
    for (auto const& b : ctx.contraption.blocks())
        if (b.local == ctx.localPos) return const_cast<contraption::CarriedBlock*>(&b);
    return nullptr;
}

Json& actorDoc(contraption::MovementContext& ctx) {
    if (!ctx.data.contains("deployer")) {
        Json doc = Json::object();
        if (auto* b = carried(ctx)) {
            if (auto it = b->data.find(kCarrier); it != b->data.end()) doc = *it;
        }
        ctx.data["deployer"] = doc;
    }
    return ctx.data["deployer"];
}

Face actorFacing(contraption::MovementContext& ctx) {
    auto it = ctx.states.find("levitate:facing");
    int  f  = 1;
    if (it != ctx.states.end())
        if (auto const* v = std::get_if<std::int64_t>(&it->second)) f = static_cast<int>(*v);
    return static_cast<Face>(std::clamp(f, 0, 5));
}

void actorActivate(contraption::MovementContext& ctx, GlobalPos const& visited) {
    auto& doc = actorDoc(ctx);
    auto  rtv = fromJson(doc.value(kDataKey, Json::object()));
    // grab an item from the contraption's storage when the hand is empty
    ItemStack filter = doc.contains("filter") && doc["filter"].contains("filter") ? items::fromJson(doc["filter"]["filter"]) : items::empty();
    auto      pass   = [&](ItemStack const& s) { return items::isEmpty(filter) || common::filter::matches(filter, s); };
    if (items::isEmpty(rtv.held)) rtv.held = ctx.contraption.storage().extractItem(pass, 1, false);

    auto  facing = actorFacing(ctx);
    auto  o      = core::kinetics::offsetOf(facing);
    auto  dir    = ctx.contraption.transform().rotateVector({double(o.x), double(o.y), double(o.z)});
    Hand  h;
    h.dim    = ctx.worldPos.dim;
    h.cx     = ctx.position.x;
    h.cy     = ctx.position.y;
    h.cz     = ctx.position.z;
    h.dx     = dir.x;
    h.dy     = dir.y;
    h.dz     = dir.z;
    h.facing = detail::nearestFace(dir.x, dir.y, dir.z);
    h.mode   = rtv.mode;
    h.clicked = game::blockPos(visited);
    auto* region = detail::regionOf(h.dim);
    if (!region || (h.mode == DeployerMode::Use && !shouldActivate(*region, rtv.held, h.clicked, h.facing))) {
        doc[kDataKey] = toJson(rtv); // keep what the hand took from the storage
        return;
    }

    std::vector<ItemStack> extra;
    act(h, rtv.held, extra, rtv.breaking);
    // dispose of excess: the held item if the filter rejects it, everything else
    if (!items::isEmpty(rtv.held) && !pass(rtv.held)) {
        extra.push_back(rtv.held);
        rtv.held = items::empty();
    }
    for (auto const& e : extra) {
        auto rest = ctx.contraption.storage().insertItem(e, false);
        if (!items::isEmpty(rest)) detail::dropItem(*region, ctx.position.x, ctx.position.y, ctx.position.z, rest);
    }
    ctx.stall = rtv.breaking.has_value();
    doc[kDataKey] = toJson(rtv);
}

GlobalPos actorTarget(contraption::MovementContext& ctx) {
    auto f   = actorFacing(ctx);
    auto o   = core::kinetics::offsetOf(f);
    auto dir = ctx.contraption.transform().rotateVector({double(o.x), double(o.y), double(o.z)});
    return GlobalPos{
        ctx.worldPos.dim,
        static_cast<int>(std::floor(ctx.position.x + dir.x * 2)),
        static_cast<int>(std::floor(ctx.position.y + dir.y * 2)),
        static_cast<int>(std::floor(ctx.position.z + dir.z * 2))
    };
}

void registerActor() {
    contraption::MovementBehaviour b;
    b.startMoving = [](contraption::MovementContext& ctx) { (void)actorDoc(ctx); };
    b.visitNewPosition = [](contraption::MovementContext& ctx, GlobalPos const&) { actorActivate(ctx, actorTarget(ctx)); };
    b.tick = [](contraption::MovementContext& ctx) {
        if (!ctx.data.contains("deployer")) return;
        auto& doc = ctx.data["deployer"];
        auto  rtv = fromJson(doc.value(kDataKey, Json::object()));
        if (!rtv.breaking) {
            ctx.stall = false;
            return;
        }
        int timer = ctx.data.value("timer", 0);
        if (timer < 20) {
            ctx.data["timer"] = timer + 1;
            ctx.stall         = true;
            return;
        }
        ctx.data.erase("timer");
        auto bp = rtv.breaking->first;
        actorActivate(ctx, GlobalPos{ctx.worldPos.dim, bp.x, bp.y, bp.z});
    };
    b.stopMoving = [](contraption::MovementContext& ctx) {
        if (!ctx.data.contains("deployer")) return;
        auto doc = ctx.data["deployer"];
        if (doc.contains(kDataKey)) {
            auto rtv = fromJson(doc[kDataKey]);
            rtv.breaking.reset();
            doc[kDataKey] = toJson(rtv);
        }
        if (auto* b = carried(ctx)) b->data[kCarrier] = doc;
        ctx.data.erase("deployer");
        ctx.stall = false;
    };
    contraption::registerActor(kDeployerId, std::move(b));
}

void registerCarrier() {
    contraption::registerDataCarrier({
        kCarrier,
        [](BlockSource& region, GlobalPos const& pos) -> std::optional<Json> {
            if (game::blockIdAt(region, game::blockPos(pos)) != kDeployerId) return std::nullopt;
            st().runtimes.erase(pos);
            if (st().index.erase(pos)) st().indexDirty = true;
            auto doc = bd::take(pos);
            return doc ? std::optional<Json>(*doc) : std::optional<Json>(Json::object());
        },
        [](BlockSource&, GlobalPos const& pos, Json const& doc) {
            if (doc.is_object() && !doc.empty()) bd::put(pos, doc);
            track(pos);
        },
        nullptr,
    });
    // the shaft axis follows the rotation of the facing
    contraption::registerRotationHandler(kDeployerId, [](contraption::cc::States& s, contraption::cc::Axis axis, int quarter) {
        auto get = [&](char const* k, int def) {
            auto it = s.find(k);
            if (it == s.end()) return def;
            auto const* v = std::get_if<std::int64_t>(&it->second);
            return v ? static_cast<int>(*v) : def;
        };
        auto newFacing  = static_cast<Face>(std::clamp(get("levitate:facing", 1), 0, 5));
        auto oldFacing  = contraption::cc::rotate(static_cast<contraption::cc::Dir>(newFacing), axis, -quarter);
        bool alongFirst = get("levitate:axis_along_first", 0) != 0;
        auto oldShaft   = ca::deployerShaftAxis(static_cast<Face>(oldFacing), alongFirst);
        auto newShaft   = contraption::cc::rotate(static_cast<contraption::cc::Axis>(oldShaft), axis, quarter);
        s["levitate:axis_along_first"] = std::int64_t{ca::deployerAlongFirstFor(newFacing, static_cast<core::kinetics::Axis>(newShaft)) ? 1 : 0};
    });
}

// ---- value boxes / filter slot -----------------------------------------------------------------

void registerFilterSlot() {
    common::filterslot::SlotSpec slot;
    slot.id     = "filter";
    slot.scale  = 0.5;
    slot.centre = [](Block const& block, int face) -> std::optional<core::common::Vec> {
        if (face < 0 || face > 5) return std::nullopt;
        Face facing = facingOf(block);
        bool along  = detail::intState(block, "levitate:axis_along_first").value_or(0) != 0;
        auto f      = static_cast<Face>(face);
        if (core::kinetics::axisOf(f) == core::kinetics::axisOf(facing)) return std::nullopt;
        if (core::kinetics::axisOf(f) == ca::deployerShaftAxis(facing, along)) return std::nullopt;
        return core::common::faceLocalPoint(face, 8, 8, 0.5);
    };
    common::filterslot::registerSlot(kDeployerId, std::move(slot));
}

} // namespace

// ---- public API ---------------------------------------------------------------------------------

DeployerMode mode(GlobalPos const& pos) { return rt(pos).mode; }

void setMode(GlobalPos const& pos, DeployerMode m) {
    auto& r = rt(pos);
    r.mode  = m;
    r.state = DeployerState::Waiting;
    r.timer = 0;
    r.breaking.reset();
    save(pos, r);
    if (auto* region = detail::regionOf(pos.dim)) syncState(pos, r, facingOf(region->getBlock(game::blockPos(pos))));
}

ItemStack held(GlobalPos const& pos) { return rt(pos).held; }

void setHeld(GlobalPos const& pos, ItemStack const& stack) {
    auto& r = rt(pos);
    r.held  = stack;
    save(pos, r);
}

std::vector<ItemStack> overflow(GlobalPos const& pos) { return rt(pos).overflow; }
int                    fistBumps() { return gFistBumps; }
DeployerState          state(GlobalPos const& pos) { return rt(pos).state; }

void activateNow(GlobalPos const& pos) {
    auto* region = detail::regionOf(pos.dim);
    if (!region) return;
    auto& r = rt(pos);
    activate(pos, r, *region, facingOf(region->getBlock(game::blockPos(pos))));
    save(pos, r);
}

void onPlaced(BlockSource& region, BlockPos const& pos, Player* placer, Face clickedFace) {
    (void)clickedFace;
    if (game::blockIdAt(region, pos) != kDeployerId) return;
    Face facing = Face::Up;
    bool lookX  = false;
    if (placer) {
        Face look = detail::lookFace(*placer);
        facing    = placer->isSneaking() ? look : core::kinetics::opposite(look);
        int f2    = game::playerFacing(*placer);
        lookX     = f2 == 1 || f2 == 3;
    }
    ca::PlacementNeighbours n;
    for (Face f : core::kinetics::kFaces)
        n.shaftTowards[static_cast<int>(f)] = detail::shaftTowards(region, detail::offsetPos(pos, f), core::kinetics::opposite(f));
    bool along = ca::deployerPlacementAlongFirst(facing, lookX, n);
    if (auto const* b = detail::withStates(region.getBlock(pos), {{"levitate:facing", static_cast<int>(facing)}, {"levitate:axis_along_first", along ? 1 : 0}}))
        game::setBlock(region, pos, *b);
    auto gp = game::globalPos(region, pos);
    st().runtimes.erase(gp);
    auto& r = rt(gp);
    save(gp, r);
    log().info("deployer placed at {} facing {}", gp.key(), core::kinetics::faceName(facing));
}

bool onUse(Player& player, BlockPos const& pos, Face face, ItemStack& heldByPlayer) {
    auto& region = player.getDimensionBlockSource();
    auto const& block = region.getBlock(pos);
    if (block.getTypeName() != kDeployerId) return false;
    auto gp     = game::globalPos(region, pos);
    Face facing = facingOf(block);
    std::string heldId = items::idOf(heldByPlayer);
    if (heldId == kWrenchId) {
        if (face != facing) return false; // other faces: the wrench rotates (U8)
        auto next = mode(gp) == DeployerMode::Punch ? DeployerMode::Use : DeployerMode::Punch;
        setMode(gp, next);
        detail::playSound(gp.dim, gp.x + .5, gp.y + .5, gp.z + .5, "random.click", .5f, 1.f);
        detail::actionBar(
            player,
            next == DeployerMode::Use ? trFor(player, "create.tooltip.deployer.using", "Mode: Use")
                                      : trFor(player, "create.tooltip.deployer.punching", "Mode: Attack")
        );
        log().info("deployer at {} switched to {} mode by {}", gp.key(), next == DeployerMode::Use ? "use" : "punch", player.getRealName());
        return true;
    }
    if (face != facing) return false;
    auto& r = rt(gp);
    ItemStack fromDeployer = r.held;
    if (items::isEmpty(fromDeployer) && items::isEmpty(heldByPlayer)) return true;
    ItemStack fromPlayer = heldByPlayer;
    heldByPlayer         = fromDeployer;
    player.setSelectedItem(fromDeployer);
    player.sendInventory(false);
    r.held = fromPlayer;
    save(gp, r);
    syncState(gp, r, facing);
    log().info("deployer at {} now holds {} (swapped with {})", gp.key(), items::idOf(fromPlayer), player.getRealName());
    return true;
}

void tick() {
    std::vector<GlobalPos> list(st().index.begin(), st().index.end());
    for (auto const& p : list) {
        try {
            tickOne(p);
        } catch (std::exception const& e) {
            log().error("deployer at {} failed: {}", p.key(), e.what());
        }
    }
    if (st().indexDirty && detail::currentTick() % 100 == 0) persistIndex();
}

void onWorldReady() {
    st().runtimes.clear();
    st().index.clear();
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
}

void install() {
    kinetics::KineticSpec spec;
    spec.states       = {"levitate:facing", "levitate:axis_along_first"};
    spec.rotationAxis = [](core::kinetics::BlockState const& s) -> std::optional<core::kinetics::Axis> {
        return ca::deployerShaftAxis(static_cast<Face>(std::clamp(s.get("levitate:facing", 1), 0, 5)), s.get("levitate:axis_along_first") != 0);
    };
    spec.hasShaftTowards = [](core::kinetics::BlockState const& s, Face f) {
        return core::kinetics::axisOf(f) == ca::deployerShaftAxis(static_cast<Face>(std::clamp(s.get("levitate:facing", 1), 0, 5)), s.get("levitate:axis_along_first") != 0);
    };
    spec.impact       = 4;
    spec.rotorState   = "levitate:rotor";
    spec.rotorModel   = "geometry.levitate.deployer_shaft";
    spec.rotorTexture = "levitate_deployer";
    kinetics::registerBlock(kDeployerId, std::move(spec));

    common::items::registerBlock(kDeployerId, capability());
    registerFilterSlot();
    registerActor();
    registerCarrier();

    bd::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
        if (previous != kDeployerId) return;
        st().runtimes.erase(pos);
        if (st().index.erase(pos)) st().indexDirty = true;
        auto it = doc.find(kDataKey);
        if (it == doc.end()) return;
        auto  r      = fromJson(*it);
        auto* region = detail::regionOf(pos.dim);
        if (!region) return;
        if (!items::isEmpty(r.held)) detail::dropItem(*region, pos.x + .5, pos.y + .5, pos.z + .5, r.held);
        for (auto const& s : r.overflow) detail::dropItem(*region, pos.x + .5, pos.y + .5, pos.z + .5, s);
    });
    st().speedListener = kinetics::onSpeedChanged(kDeployerId, [](kinetics::SpeedChange const& c) {
        if (c.added) track(c.pos);
    });
}

void uninstall() {
    if (st().speedListener >= 0) kinetics::removeListener(st().speedListener);
    st().speedListener = -1;
}

} // namespace lv::feature::assembly::deployer
