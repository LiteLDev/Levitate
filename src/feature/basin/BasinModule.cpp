// Basin unit (U14): module, machine registry and ticking, world helpers, game hooks (block use,
// placement, blaze capture), kinetic specs, the basin's filter slot.
//
// Every block of the unit is remembered in a per-world list (`lv:basin:machines`) as it appears in
// the world (BlockChangedEvent: players, commands, contraptions, ...) and ticked while its chunk is
// loaded, as Create ticks its block entities. Per-block contents live in BlockData (U3).

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("basin", lv::feature::basin::BasinConfig);

namespace lv::feature::basin {

namespace k  = lv::kinetics;
namespace bd = common::blockdata;

// ---- kinetic blocks --------------------------------------------------------------------------------

namespace {
k::KineticSpec pressSpec() {
    // Create's MechanicalPressBlock: a horizontal shaft along its facing axis
    auto s = k::specs::withModel(k::specs::shaft(kAxisState), "geometry.levitate.mechanical_press_rotor", "levitate_mechanical_press_shaft");
    s.impact = 8;
    return s;
}

k::KineticSpec mixerSpec() {
    // Create's MechanicalMixerBlock: a small cog turning around Y, no shaft ends
    k::KineticSpec s;
    s.rotationAxis    = [](k::BlockState const&) -> std::optional<k::Axis> { return k::Axis::Y; };
    s.hasShaftTowards = [](k::BlockState const&, k::Face) { return false; };
    s.cog             = [](k::BlockState const&) { return k::CogType::Small; };
    s.impact          = 4;
    s.rotorState      = kRotorState;
    return k::specs::withModel(std::move(s), "geometry.levitate.mechanical_mixer_rotor", "levitate_mechanical_mixer_cog");
}
} // namespace

LV_REGISTER_KINETIC_BLOCK(kPress, pressSpec());
LV_REGISTER_KINETIC_BLOCK(kMixer, mixerSpec());

// ---- small helpers ---------------------------------------------------------------------------------

std::optional<Kind> kindOf(std::string_view id) {
    if (id == kBasin) return Kind::Basin;
    if (id == kPress) return Kind::Press;
    if (id == kMixer) return Kind::Mixer;
    if (id == kBurner) return Kind::Burner;
    if (id == kLitBurner) return Kind::LitBurner;
    if (id == kEmptyBurner) return Kind::EmptyBurner;
    return std::nullopt;
}

int oppositeFace(int face) {
    if (face < 0 || face > 5) return face;
    return face ^ 1;
}

BlockPos offsetOf(BlockPos const& p, int face, int n) {
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face < 0 || face > 5) return p;
    return BlockPos{p.x + dx[face] * n, p.y + dy[face] * n, p.z + dz[face] * n};
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(DimensionType(dim)).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

bool loaded(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return false;
    return region.hasChunksAt(pos, 0, false);
}

std::string idAt(BlockSource& region, BlockPos const& pos) {
    if (!loaded(region, pos)) return {};
    return region.getBlock(pos).getTypeName();
}

int stateAt(BlockSource& region, BlockPos const& pos, char const* state, int fallback) {
    if (!loaded(region, pos)) return fallback;
    return game::intState(region.getBlock(pos), state).value_or(fallback);
}

bool setState(BlockSource& region, BlockPos const& pos, char const* state, int value) {
    if (!loaded(region, pos)) return false;
    auto const& block = region.getBlock(pos);
    auto const* s     = block.getBlockType().getBlockState(HashedString(std::string(state)));
    if (!s) return false;
    if (block.getState<int>(*s) == value) return false;
    auto next = block.setState(*s, value);
    if (!next) return false;
    return game::setBlock(region, pos, *next.as_ptr());
}

void playSound(int dim, BlockPos const& pos, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(pos, DimensionType(dim));
}

void particle(int dim, float x, float y, float z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto d = level->getDimension(DimensionType(dim)).lock();
    if (!d) return;
    level->spawnParticleEffect(effect, Vec3{x, y, z}, d.get());
}

void popItem(BlockSource& region, float x, float y, float z, ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    // Create's Block.popResource: a little random offset around the point
    float ox = (random01() - .5f) * .5f, oz = (random01() - .5f) * .5f;
    level->getSpawner().spawnItem(region, stack, nullptr, Vec3{x + ox, y, z + oz}, 10);
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
}

float random01() {
    static std::mt19937                          rng{std::random_device{}()};
    static std::uniform_real_distribution<float> dist(0.f, 1.f);
    float                                        v = dist(rng);
    return v >= 1.f ? 0.f : v;
}

bool hasCollision(BlockSource& region, BlockPos const& pos) {
    if (!loaded(region, pos)) return true;
    auto const& block = region.getBlock(pos);
    if (block.isAir()) return false;
    AABB box = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
    return box.max.x - box.min.x > 1e-4f && box.max.y - box.min.y > 1e-4f && box.max.z - box.min.z > 1e-4f;
}

int maxStackOf(ItemStack const& stack) {
    if (stack.isNull()) return 64;
    return std::max<int>(1, stack.getMaxStackSize());
}

// ---- machine registry ------------------------------------------------------------------------------

namespace {

constexpr char kDbMachines[] = "lv:basin:machines";

struct Registry {
    std::map<GlobalPos, Kind>          machines;
    bool                               dirty = false;
    std::uint64_t                      ticks = 0;
    std::vector<std::function<void()>> deferred;
    std::vector<GlobalPos>             checks; ///< placement survival / neighbour checks
};

Registry& reg() {
    static Registry r;
    return r;
}

void persist() {
    auto& r = reg();
    if (!r.dirty || !game::db::available()) return;
    Json j = Json::array();
    for (auto const& [p, kind] : r.machines) j.push_back({p.dim, p.x, p.y, p.z, static_cast<int>(kind)});
    game::db::save(kDbMachines, j);
    r.dirty = false;
}

void loadRegistry() {
    auto& r = reg();
    r.machines.clear();
    r.deferred.clear();
    r.checks.clear();
    if (auto j = game::db::load(kDbMachines); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() != 5) continue;
            r.machines[GlobalPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}] = static_cast<Kind>(e[4].get<int>());
        }
    }
    r.dirty = false;
}

void untrack(GlobalPos const& pos) {
    auto& r = reg();
    if (r.machines.erase(pos)) r.dirty = true;
    basinops::forget(pos);
    operators::forget(pos);
    burners::forget(pos);
}

} // namespace

void track(GlobalPos const& pos, Kind kind) {
    auto& r  = reg();
    auto  it = r.machines.find(pos);
    if (it != r.machines.end() && it->second == kind) return;
    r.machines[pos] = kind;
    r.dirty         = true;
}

std::map<GlobalPos, Kind> const& tracked() { return reg().machines; }

std::uint64_t now() { return reg().ticks; }

void defer(std::function<void()> fn) { reg().deferred.push_back(std::move(fn)); }

// ---- placement ---------------------------------------------------------------------------------------

namespace {

/// Java 2D facing (0 S, 1 W, 2 N, 3 E) -> Bedrock face.
int faceOf2d(int f) {
    static constexpr int faces[] = {kSouth, kWest, kNorth, kEast};
    return faces[f & 3];
}

/// Create's HorizontalKineticBlock.getPreferredHorizontalFacing: the side a neighbouring shaft
/// connects from (nullopt when none or when two axes compete).
std::optional<int> preferredHorizontalFace(BlockSource& region, BlockPos const& pos) {
    std::optional<int> preferred;
    for (int face : kHorizontals) {
        auto        n     = offsetOf(pos, face);
        if (!loaded(region, n)) continue;
        auto const& block = region.getBlock(n);
        auto const* spec  = k::specOf(block.getTypeName());
        if (!spec) continue;
        k::BlockState snap;
        snap.id = block.getTypeName();
        for (auto const& name : spec->states) snap.values.emplace_back(name, game::intState(block, name).value_or(0));
        if (!spec->shaftTowards(snap, static_cast<k::Face>(oppositeFace(face)))) continue;
        if (preferred && (*preferred >> 1) != (face >> 1)) return std::nullopt;
        preferred = face;
    }
    return preferred;
}

/// Create's canSurvive: no basin directly under a press / mixer.
bool survives(BlockSource& region, BlockPos const& pos, Kind kind) {
    if (kind == Kind::Basin) {
        auto above = kindOf(idAt(region, offsetOf(pos, kUp)));
        return !(above && (*above == Kind::Press || *above == Kind::Mixer));
    }
    if (kind == Kind::Press || kind == Kind::Mixer) return idAt(region, offsetOf(pos, kDown)) != kBasin;
    return true;
}

/// Orients a block of the unit that a player just placed (Create's getStateForPlacement).
void orientPlaced(Player& player, BlockSource& region, BlockPos const& pos, Kind kind) {
    int dim = static_cast<int>(region.getDimensionId());
    if (!survives(region, pos, kind)) {
        // Create refuses the placement: the block pops as its item
        auto level = ll::service::getLevel();
        if (level) {
            BlockChangeContext ctx{};
            level->destroyBlock(region, pos, true, ctx);
        }
        log().info("basin: {} at {} {} {} can't stay there (basin under a press / mixer)", idAt(region, pos), pos.x, pos.y, pos.z);
        return;
    }
    switch (kind) {
    case Kind::Press: {
        auto preferred = preferredHorizontalFace(region, pos);
        int  face      = preferred && !player.isSneaking() ? *preferred : faceOf2d(game::playerFacing(player));
        setState(region, pos, kAxisState, (face == kWest || face == kEast) ? 0 : 2);
        break;
    }
    case Kind::Burner:
    case Kind::EmptyBurner:
    case Kind::LitBurner:
        setState(region, pos, kFacingState, faceOf2d(game::opposite(game::playerFacing(player))));
        break;
    default:
        break;
    }
    log().info("basin: {} placed at {} {} {} (dim {}) by {}", idAt(region, pos), pos.x, pos.y, pos.z, dim, player.getRealName());
}

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

/// Block-local hit position from a click position (relative or absolute).
core::common::Vec localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>>& lastUse() {
    static std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> m;
    return m;
}

InteractionResult handled() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    BasinUseItemOnHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItemOn,
    InteractionResult,
    ItemStack&      item,
    BlockPos const& at,
    uchar           face,
    Vec3 const&     hit,
    HandSlot        handSlot,
    Block const*    targetBlock,
    bool            isFirstEvent
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&        region  = player.getDimensionBlockSource();
    Block const* clicked = targetBlock ? targetBlock : &region.getBlock(at);
    std::string  target  = clicked->getTypeName();
    std::string  held    = item.isNull() ? std::string{} : item.getTypeName();
    auto         kind    = kindOf(target);
    bool         sneak   = player.isSneaking();

    auto gp = game::globalPos(region, at);
    // Bedrock repeats the use while the button is held (and SimulatedPlayers never send a "first"
    // event): one action per click, repeats of a handled click are swallowed
    auto  uid    = player.getOrCreateUniqueID().rawID;
    auto& last   = lastUse()[uid];
    bool  repeat = last.first == gp && now() - last.second < 4;
    auto  done   = [&] {
        last = {gp, now()};
        player.sendInventory(false);
        return handled();
    };
    if (kind) log().debug("basin: {} uses {} on {} at {}", player.getRealName(), held.empty() ? "an empty hand" : held, target, gp.key());
    if (repeat && (kind || held == kEmptyBurner)) return handled();

    // blocks of the unit: Create's use() (sneaking with an item places / uses the item instead)
    if (kind && !(sneak && !held.empty())) {
        if (*kind == Kind::Basin) {
            // the filter slot is U3's (value box interaction)
            if (common::filterslot::slotAt(*clicked, face, localHit(at, hit))) {
                return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
            }
            if (held == kWrench) {
                if (auto* b = basinops::get(region, gp)) basinops::onWrenched(region, gp, *b, face);
                return done();
            }
            if (basinops::use(player, region, gp, item, face)) return done();
        } else if (*kind == Kind::Burner || *kind == Kind::EmptyBurner || *kind == Kind::LitBurner) {
            if (burners::use(player, region, gp, item)) return done();
        }
    }
    // the empty burner on a blaze spawner
    if (held == kEmptyBurner && target == "minecraft:mob_spawner") {
        if (burners::captureFromSpawner(player, region, at)) return done();
    }
    // placing a block of the unit: orient it afterwards (Create's getStateForPlacement)
    auto placing = kindOf(held);
    if (!placing) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    BlockPos dest     = game::replaceable(region, at) ? at : offsetOf(at, face);
    auto     before   = idAt(region, dest);
    auto     result   = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto     afterKnd = kindOf(idAt(region, dest));
    if (afterKnd && idAt(region, dest) != before) orientPlaced(player, region, dest, *afterKnd);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    BasinInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&      entity,
    Vec3 const& location,
    HandSlot    handSlot
) {
    Player& player = mPlayer;
    if (isServerPlayer(player)) {
        auto const& held = player.getSelectedItem();
        if (!held.isNull() && held.getTypeName() == kEmptyBurner && burners::captureFromEntity(player, entity)) {
            player.sendInventory(false);
            return true;
        }
    }
    return origin(entity, location, handSlot);
}

// ---- module --------------------------------------------------------------------------------------------

class BasinModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
        BasinUseItemOnHook::hook();
        BasinInteractHook::hook();
        for (auto const* id : {kPress, kMixer}) {
            mSpeedListeners.push_back(k::onSpeedChanged(id, [](k::SpeedChange const& c) { operators::onSpeedChanged(c.pos); }));
        }
        static bool once = false;
        if (!once) {
            once = true;
            game::db::onWorldReady([] { worldReady(); });
            game::db::onWorldSave([] { persist(); });
            game::db::onWorldClosed([] { worldClosed(); });
            bd::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
                if (previous == kBasin || (previous.empty() && doc.contains("basin"))) {
                    if (auto* region = regionOf(pos.dim)) basinops::dropContents(*region, pos, doc);
                }
                basinops::forget(pos);
                operators::forget(pos);
                burners::forget(pos);
            });
            basinops::registerCapabilities();
            burners::registerHeat();
            // Create's BasinValueBox: a filter slot on every horizontal side, 12 px up
            common::filterslot::registerSlot(kBasin, {
                .id     = "filter",
                .centre = [](Block const&, int face) -> std::optional<core::common::Vec> {
                    if (face < 2 || face > 5) return std::nullopt;
                    return core::common::faceLocalPoint(face, 8, 12, 0.05);
                },
                .scale     = 0.5,
                .onChanged = [](GlobalPos const& pos) {
                    if (auto* region = regionOf(pos.dim))
                        if (auto* b = basinops::get(*region, pos)) basinops::notifyChangeOfContents(*b);
                },
            });
            common::recipes::loadType("pressing");
            common::recipes::loadType("mixing");
            common::recipes::loadType("compacting");
        }
        if (game::db::available()) worldReady(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        for (auto id : mSpeedListeners) k::removeListener(id);
        mSpeedListeners.clear();
        BasinUseItemOnHook::unhook();
        BasinInteractHook::unhook();
        persist();
    }

private:
    static void worldReady() {
        loadRegistry();
        basinops::forgetAll();
        operators::forgetAll();
        burners::forgetAll();
        recipes::reset();
        sync::reset();
        log().info("basin: {} machines known in this world", reg().machines.size());
    }

    static void worldClosed() {
        persist();
        reg().machines.clear();
        reg().deferred.clear();
        reg().checks.clear();
        basinops::forgetAll();
        operators::forgetAll();
        burners::forgetAll();
        recipes::reset();
        sync::reset();
    }

    static void onBlockChanged(ll::event::BlockChangedEvent& ev) {
        if (ev.layer() != 0) return;
        auto& region = ev.blockSource();
        if (region.getILevel().isClientSide()) return;
        auto const& prev    = ev.previousBlock();
        auto const& next    = ev.newBlock();
        auto        nextKnd = kindOf(next.getTypeName());
        auto        prevKnd = kindOf(prev.getTypeName());
        auto        pos     = game::globalPos(region, ev.pos());
        if (nextKnd) {
            if (*nextKnd == Kind::EmptyBurner) {
                if (prevKnd) untrack(pos);
            } else {
                if (prevKnd && *prevKnd != *nextKnd) untrack(pos);
                track(pos, *nextKnd);
            }
        } else if (prevKnd) {
            untrack(pos);
        }
        // a heat source or a machine around a basin changed: the basin's operator looks again
        if (&prev.getBlockType() != &next.getBlockType() || nextKnd == Kind::Burner) reg().checks.push_back(pos);
    }

    static void tick() {
        auto& r = reg();
        ++r.ticks;
        if (!ll::service::getLevel()) return;
        auto work = std::exchange(r.deferred, {});
        for (auto& fn : work) {
            try {
                fn();
            } catch (std::exception const& e) {
                log().error("basin: deferred action failed: {}", e.what());
            }
        }
        // neighbour changes: a basin above a changed heat source, a basin under a changed operator
        auto checks = std::exchange(r.checks, {});
        for (auto const& p : checks) {
            auto* region = regionOf(p.dim);
            if (!region) continue;
            GlobalPos above{p.dim, p.x, p.y + 1, p.z};
            if (idAt(*region, game::blockPos(above)) == kBasin) {
                if (auto* b = basinops::get(*region, above)) basinops::notifyChangeOfContents(*b);
            }
            if (idAt(*region, game::blockPos(p)) == kBasin) basinops::wakeOperator(p);
            GlobalPos below2{p.dim, p.x, p.y - 2, p.z};
            if (idAt(*region, game::blockPos(below2)) == kBasin) basinops::wakeOperator(below2);
        }
        auto snapshot = std::vector<std::pair<GlobalPos, Kind>>(r.machines.begin(), r.machines.end());
        for (auto const& [pos, kind] : snapshot) {
            auto* region = regionOf(pos.dim);
            if (!region) continue;
            auto bp = game::blockPos(pos);
            if (!loaded(*region, bp)) continue;
            auto now = kindOf(region->getBlock(bp).getTypeName());
            if (!now || *now != kind) {
                untrack(pos);
                if (now && *now != Kind::EmptyBurner) track(pos, *now);
                continue;
            }
            try {
                switch (kind) {
                case Kind::Basin:
                    if (auto* b = basinops::get(*region, pos)) basinops::tick(*region, pos, *b);
                    break;
                case Kind::Press:
                    if (auto* s = operators::get(pos, true)) operators::tickPress(*region, pos, *s);
                    break;
                case Kind::Mixer:
                    if (auto* s = operators::get(pos, false)) operators::tickMixer(*region, pos, *s);
                    break;
                case Kind::Burner:
                    if (auto* d = burners::get(pos)) burners::tick(*region, pos, *d);
                    break;
                case Kind::LitBurner:
                    burners::particles(*region, pos, kind);
                    break;
                default:
                    break;
                }
            } catch (std::exception const& e) {
                log().error("basin: ticking {} at {} failed: {}", static_cast<int>(kind), pos.key(), e.what());
            }
        }
        sync::flush();
        if (r.dirty && r.ticks % 200 == 0) persist();
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<k::ListenerId>          mSpeedListeners;
};

} // namespace lv::feature::basin

LV_REGISTER_MODULE(lv::feature::basin::BasinModule);
