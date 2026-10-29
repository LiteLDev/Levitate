// Fluids unit (U19): module wiring. World helpers, the machine list (ticking), placement
// orientation, the player interaction hooks and the companion sync of hoses and spouts.

#include <cmath>
#include <map>
#include <mutex>
#include <set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/fluids/FluidsInternal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("fluids", lv::feature::fluids::FluidsConfig);

namespace lv::feature::fluids {

namespace bd    = common::blockdata;
namespace items = common::items;

FluidsConfig const& config() { return lv::config::get<FluidsConfig>(); }

namespace {

constexpr char kDbMachines[] = "lv:fluids:machines";
constexpr char kDbSources[]  = "lv:fluids:sources";
constexpr char kSyncChannel[] = "fluids.sync";

struct PlaceIntent {
    int  face  = 2; ///< the nearest looking direction (FacingID)
    int  horiz = 2; ///< the horizontal looking direction (FacingID 2..5)
    bool sneak = false;
};

struct State {
    std::map<GlobalPos, Kind>          machines; ///< ticking machines (not the fluid blocks)
    std::set<GlobalPos>                sources;  ///< honey / chocolate blocks
    bool                               sourcesDirty = false;
    std::vector<std::function<void()>> deferred;
    std::map<GlobalPos, PlaceIntent>   intents;
    std::map<GlobalPos, std::pair<float, float>> hoses; ///< last synced (offset, speed)
    std::map<std::int64_t, std::uint64_t>         lastUse; ///< player -> tick of the last handled use
    std::uint64_t                      ticks  = 0;
    bool                               loaded = false;
};
State& state() {
    static State s;
    return s;
}

void persist() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& [p, k] : state().machines) j.push_back({p.dim, p.x, p.y, p.z, int(k)});
    game::db::save(kDbMachines, j);
}

/// Fluid blocks can be many (hose pulleys fill whole lakes): saved with the world, not per change.
void saveSources() {
    auto& s = state();
    if (!s.sourcesDirty || !game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : s.sources) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbSources, j);
    s.sourcesDirty = false;
}

void loadWorld() {
    auto& s = state();
    s.machines.clear();
    s.sources.clear();
    s.sourcesDirty = false;
    s.intents.clear();
    s.hoses.clear();
    if (auto j = game::db::load(kDbMachines); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 5)
                s.machines[{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}] = Kind(e[4].get<int>());
    }
    if (auto j = game::db::load(kDbSources); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) s.sources.insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
    s.loaded = true;
}

/// FacingID of a direction vector's dominant axis.
int nearestFace(Vec3 const& v) {
    float ax = std::abs(v.x), ay = std::abs(v.y), az = std::abs(v.z);
    if (ay >= ax && ay >= az) return v.y < 0 ? 0 : 1;
    if (ax >= az) return v.x < 0 ? 4 : 5;
    return v.z < 0 ? 2 : 3;
}
int horizontalFace(Player const& p) {
    switch (game::playerFacing(p)) { // Java 2D: 0 S, 1 W, 2 N, 3 E
    case 0:
        return 3;
    case 1:
        return 4;
    case 2:
        return 2;
    default:
        return 5;
    }
}
void orient(GlobalPos const& p, std::string const& id, PlaceIntent const& in) {
    auto* region = regionOf(p.dim);
    if (!region) return;
    int facing;
    if (id == kHosePulley) {
        // Create's HosePulleyBlock: facing the player (opposite of the look), reversed when sneaking
        facing = cf::oppositeFace(in.horiz);
        if (in.sneak) facing = cf::oppositeFace(facing);
    } else {
        // PortableStorageInterfaceBlock: nearest looking direction, opposite (towards the player)
        facing = in.sneak ? in.face : cf::oppositeFace(in.face);
    }
    auto bp = toBlockPos(p);
    if (region->getBlock(bp).getTypeName() != id) return;
    if (auto const* b = game::blockWith(id, {{kFacing, facing}, {kRotor, 0}})) game::setBlock(*region, bp, *b);
}

ItemStack heldCopy(ItemStack const& item) { return item; }

} // namespace

// ---- world helpers -----------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

BlockPos toBlockPos(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

GlobalPos offsetPos(GlobalPos const& p, int face, int n) {
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face < 0 || face > 5) return p;
    return {p.dim, p.x + dx[face] * n, p.y + dy[face] * n, p.z + dz[face] * n};
}

std::string blockIdAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return {};
    return game::blockIdAt(*region, toBlockPos(p));
}

int intStateAt(GlobalPos const& p, char const* st, int fallback) {
    auto* region = regionOf(p.dim);
    if (!region || !region->hasChunksAt(toBlockPos(p), 0, false)) return fallback;
    return game::intState(region->getBlock(toBlockPos(p)), st).value_or(fallback);
}

bool solidAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region || !region->hasChunksAt(toBlockPos(p), 0, false)) return true;
    auto const& b = region->getBlock(toBlockPos(p));
    if (b.isAir()) return false;
    return b.getBlockType().mSolid && !game::replaceable(*region, toBlockPos(p));
}

bool passableAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region || !region->hasChunksAt(toBlockPos(p), 0, false)) return false;
    auto const& b  = region->getBlock(toBlockPos(p));
    auto        id = b.getTypeName();
    if (b.isAir() || id == kHoneyBlock || id == kChocolateBlock) return true;
    if (id.find("funnel") != std::string::npos) return true; // Create: funnels never block processing
    AABB box   = b.getBlockType().getCollisionShape(b, *region, toBlockPos(p), {});
    bool empty = box.max.x - box.min.x <= 0 || box.max.y - box.min.y <= 0 || box.max.z - box.min.z <= 0;
    return empty || game::replaceable(*region, toBlockPos(p));
}

void playSound(GlobalPos const& p, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{p.x * 8 + 4, p.y * 8 + 4, p.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(toBlockPos(p), DimensionType(p.dim));
}

void spawnItem(int dim, double x, double y, double z, ItemStack const& stack, double mx, double my, double mz) {
    if (items::isEmpty(stack)) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(dim);
    if (!level || !region) return;
    auto* actor = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(x), float(y), float(z)}, 0);
    if (actor && (mx != 0 || my != 0 || mz != 0))
        const_cast<Vec3&>(actor->getPosDelta()) = Vec3{float(mx), float(my), float(mz)};
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy) || !items::isEmpty(copy)) player.drop(copy, false);
}

// ---- tanks -------------------------------------------------------------------------------------

FluidStack tankOf(Json const& doc, char const* key) {
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_object()) return {};
    FluidStack f{it->value("fluid", ""), it->value("amount", 0)};
    return f.empty() ? FluidStack{} : f;
}

void setTank(Json& doc, FluidStack const& f, char const* key) {
    if (f.empty()) doc.erase(key);
    else doc[key] = {{"fluid", f.fluid}, {"amount", f.amount}};
}

Json const* dataAt(GlobalPos const& p) {
    auto const* d = bd::get(p);
    if (!d) return nullptr;
    auto it = d->find(kDataKey);
    return it == d->end() ? nullptr : &*it;
}

Json& editData(GlobalPos const& p) {
    auto& d = bd::edit(p);
    auto& f = d[kDataKey];
    if (!f.is_object()) f = Json::object();
    return f;
}

int tankFill(FluidStack& tank, int capacity, FluidStack const& in, bool simulate) {
    if (in.empty()) return 0;
    if (!tank.empty() && tank.fluid != in.fluid) return 0;
    int space = capacity - (tank.empty() ? 0 : tank.amount);
    int n     = std::clamp(in.amount, 0, std::max(0, space));
    if (!simulate && n > 0) {
        if (tank.empty()) tank = {in.fluid, 0};
        tank.amount += n;
    }
    return n;
}

FluidStack tankDrain(FluidStack& tank, FluidStack const& request, bool simulate) {
    if (tank.empty() || request.amount <= 0) return {};
    if (!request.fluid.empty() && request.fluid != tank.fluid) return {};
    int        n   = std::min(request.amount, tank.amount);
    FluidStack out = {tank.fluid, n};
    if (!simulate) {
        tank.amount -= n;
        if (tank.amount <= 0) tank = {};
    }
    return out;
}

bool exchangeWithTank(Player& player, FluidStack& tank, int capacity, ItemStack const& held, bool allowInsert) {
    if (items::isEmpty(held)) return false;
    bool creative = player.isCreative();
    // empty the item into the tank (FluidHelper.tryEmptyItemIntoBE: only when all of it fits)
    if (allowInsert) {
        if (auto e = emptyItem(held)) {
            if (tankFill(tank, capacity, e->fluid, true) != e->fluid.amount) return false;
            tankFill(tank, capacity, e->fluid, false);
            if (!creative) {
                player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
                giveOrDrop(player, e->container);
            }
            return true;
        }
    }
    // fill the item from the tank (tryFillItemFromBE: generic filling only)
    if (!canBeFilled(held, false) || tank.empty()) return false;
    int need = requiredAmount(held, tank, false);
    if (need < 0 || need > tank.amount) return false;
    auto out = fillItem(held, tank, false);
    if (items::isEmpty(out)) return false;
    tankDrain(tank, {tank.fluid, need}, false);
    if (!creative) player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
    giveOrDrop(player, out);
    return true;
}

// ---- tracking ----------------------------------------------------------------------------------

std::optional<Kind> kindOf(std::string_view id) {
    if (id == kSpout) return Kind::Spout;
    if (id == kItemDrain) return Kind::Drain;
    if (id == kHosePulley) return Kind::Hose;
    if (id == kPfi) return Kind::Pfi;
    if (id == kHoneyBlock || id == kChocolateBlock) return Kind::Source;
    return std::nullopt;
}

std::vector<GlobalPos> tracked(Kind kind) {
    std::vector<GlobalPos> out;
    if (kind == Kind::Source) return {state().sources.begin(), state().sources.end()};
    for (auto const& [p, k] : state().machines)
        if (k == kind) out.push_back(p);
    return out;
}

void untrack(GlobalPos const& p) {
    if (state().sources.erase(p)) state().sourcesDirty = true;
    if (state().machines.erase(p)) persist();
}

void defer(std::function<void()> fn) { state().deferred.push_back(std::move(fn)); }

std::uint64_t currentTick() { return state().ticks; }

// ---- client sync -------------------------------------------------------------------------------

namespace {
void sendNear(GlobalPos const& p, Json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& pl) {
        if (static_cast<int>(pl.getDimensionId()) != p.dim || !net::isCompanion(pl)) return true;
        auto  pos = pl.getPosition();
        float dx = pos.x - p.x, dz = pos.z - p.z;
        if (dx * dx + dz * dz > 192.f * 192.f) return true;
        net::send(pl, kSyncChannel, msg);
        return true;
    });
}
} // namespace

void syncHose(GlobalPos const& p, float offset, float speedPerTick, bool gone) {
    auto& last = state().hoses;
    if (gone) {
        if (!last.erase(p)) return;
        sendNear(p, {{"op", "hose_remove"}, {"dim", p.dim}, {"pos", {p.x, p.y, p.z}}});
        return;
    }
    auto it = last.find(p);
    if (it != last.end()) {
        if (it->second.first == offset && it->second.second == speedPerTick) return;
        // while moving at the same speed the client extrapolates: refresh twice a second
        if (it->second.second == speedPerTick && speedPerTick != 0 && state().ticks % 10 != 0) return;
    }
    last[p] = {offset, speedPerTick};
    sendNear(p, {{"op", "hose"}, {"dim", p.dim}, {"pos", {p.x, p.y, p.z}}, {"offset", offset}, {"speed", speedPerTick}});
}

void syncSpout(GlobalPos const& p, std::string const& fluid) {
    sendNear(p, {{"op", "spout"}, {"dim", p.dim}, {"pos", {p.x, p.y, p.z}}, {"color", cf::fluidColor(fluid)}});
}

void syncAllTo(Player& player) {
    int  dim  = static_cast<int>(player.getDimensionId());
    Json list = Json::array();
    for (auto const& p : tracked(Kind::Hose)) {
        if (p.dim != dim) continue;
        list.push_back({{"pos", {p.x, p.y, p.z}}, {"offset", hoseOffset(p)}, {"speed", 0}});
    }
    net::send(player, kSyncChannel, {{"op", "hoses"}, {"dim", dim}, {"hoses", list}});
}

/// ---- interactions (common/Interactions.h) --------------------------------------------------------

namespace ia = common::interactions;

namespace {

bool isOurFluidBlock(std::string const& id) { return id == kItemDrain || id == kHoneyBlock || id == kChocolateBlock; }
bool isOurBucket(std::string const& id) { return id == kHoneyBucket || id == kChocolateBucket; }

/// A bucket click on `pos` (drain / our fluid blocks / our buckets); Bedrock repeats the use while
/// the button is held: one action per 5 ticks.
ia::Result bucketClick(ia::UseOnContext const& c) {
    auto& player = c.player;
    auto& last   = state().lastUse[player.getOrCreateUniqueID().rawID];
    if (state().ticks - last < 5 && last != 0) {
        player.sendInventory(false);
        return ia::Result::Consume;
    }
    bool handled = (c.blockId == kItemDrain && !c.sneaking && useOnDrain(player, c.pos, heldCopy(c.item)))
                || useBucket(player, c.pos, c.face, heldCopy(c.item));
    if (!handled) return ia::Result::Pass;
    last = state().ticks;
    player.sendInventory(false);
    return ia::Result::Consume;
}

/// The first non-air block along the player's view within reach (an empty bucket is used "in the
/// air" by Bedrock: it never reaches useItemOn for blocks it can't pick up).
std::optional<GlobalPos> lookedAt(Player& player) {
    auto& region = player.getDimensionBlockSource();
    Vec3  eye    = player.getEyePos();
    Vec3  dir    = player.getViewVector();
    for (float t = 0; t <= 6.f; t += 0.05f) {
        BlockPos bp = game::blockPosOf(Vec3{eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t});
        if (!region.hasChunksAt(bp, 0, false)) return std::nullopt;
        if (!region.getBlock(bp).isAir()) return game::globalPos(region, bp);
    }
    return std::nullopt;
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

void installInteractions() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, [](ia::UseOnContext const& c) {
        try {
            // clicks on our blocks (with our buckets elsewhere: after the clicked block's own use)
            if (isOurFluidBlock(c.blockId)) {
                if (auto r = bucketClick(c); r != ia::Result::Pass) return r;
            }
            if (c.itemId == kHosePulley || c.itemId == kPfi) {
                auto&     player = c.player;
                auto&     region = player.getDimensionBlockSource();
                GlobalPos place  = game::replaceable(region, game::blockPos(c.pos)) ? c.pos : offsetPos(c.pos, c.face);
                Vec3      look   = player.getViewVector();
                state().intents[place] = PlaceIntent{nearestFace(look), horizontalFace(player), c.sneaking};
            }
        } catch (std::exception const& e) {
            log().error("fluids: use failed: {}", e.what());
        }
        return ia::Result::Pass;
    }));
    // our buckets on other blocks: the clicked block's own use (chests, doors, other units' tanks)
    // comes first
    auto unused = [](ia::UseOnContext const& c) {
        if (isOurFluidBlock(c.blockId)) return ia::Result::Pass;
        try {
            return bucketClick(c);
        } catch (std::exception const& e) {
            log().error("fluids: use failed: {}", e.what());
        }
        return ia::Result::Pass;
    };
    ids.push_back(ia::onUnusedUseOn({.item = kHoneyBucket}, unused));
    ids.push_back(ia::onUnusedUseOn({.item = kChocolateBucket}, unused));
    // the empty bucket "in the air" on a drain or our fluid blocks
    ids.push_back(ia::onUse("minecraft:bucket", [](ia::UseContext const& c) {
        if (c.onBlock) return ia::Result::Pass; // a block click was the use-on's
        auto& player = c.player;
        try {
            if (auto p = lookedAt(player)) {
                auto id = blockIdAt(*p);
                if (isOurFluidBlock(id)) {
                    auto& last = state().lastUse[player.getOrCreateUniqueID().rawID];
                    if (last != 0 && state().ticks - last < 5) return ia::Result::Consume;
                    bool handled = id == kItemDrain ? (!c.sneaking && useOnDrain(player, *p, heldCopy(c.item)))
                                                    : useBucket(player, *p, 1, heldCopy(c.item));
                    if (handled) {
                        last = state().ticks;
                        return ia::Result::Consume;
                    }
                }
            }
        } catch (std::exception const& e) {
            log().error("fluids: bucket use failed: {}", e.what());
        }
        return ia::Result::Pass;
    }));
}

void uninstallInteractions() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace

// ---- module ------------------------------------------------------------------------------------

class FluidsModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getILevel().isClientSide()) return;
            auto& s = state();
            if (!s.loaded) return;
            GlobalPos p    = game::globalPos(region, ev.pos());
            auto      next = ev.newBlock().getTypeName();
            auto      prev = ev.previousBlock().getTypeName();
            auto      kind = kindOf(next);
            if (kind == Kind::Source) {
                if (s.machines.contains(p)) untrack(p);
                if (s.sources.insert(p).second) s.sourcesDirty = true;
            } else if (kind) {
                if (s.sources.erase(p)) s.sourcesDirty = true;
                auto [it, fresh] = s.machines.insert_or_assign(p, *kind);
                if (fresh) persist();
                if (next != prev && (next == kHosePulley || next == kPfi)) {
                    if (auto in = s.intents.find(p); in != s.intents.end()) {
                        auto intent = in->second;
                        s.intents.erase(in);
                        defer([p, next, intent] { orient(p, next, intent); });
                    }
                }
            } else if (s.machines.contains(p) || s.sources.contains(p)) {
                if (s.machines.contains(p) && s.machines[p] == Kind::Hose) syncHose(p, 0, 0, true);
                untrack(p);
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([] { loadWorld(); });
            game::db::onWorldSave([] { saveSources(); });
            game::db::onWorldClosed([] {
                state() = State{};
            });
            net::onCompanionJoined([](Player& p) { syncAllTo(p); });
            // our machines keep their tanks / held items when a contraption moves them
            contraption::registerDataCarrier({
                "fluids",
                [](BlockSource& region, GlobalPos const& p) -> std::optional<Json> {
                    auto id   = game::blockIdAt(region, toBlockPos(p));
                    auto kind = kindOf(id);
                    if ((!kind || *kind == Kind::Source) && id != kTestDepot) return std::nullopt;
                    return bd::take(p);
                },
                [](BlockSource&, GlobalPos const& p, Json const& doc) { bd::put(p, doc); },
                nullptr,
            });
            registerSpout();
            registerDrain();
            registerHose();
            registerPfi();
            registerSources();
        }
        if (game::db::available()) loadWorld();
        installInteractions();
    }

    void uninstall() {
        uninstallInteractions();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    static void tick() {
        auto& s = state();
        if (!s.loaded) return;
        ++s.ticks;
        auto work = std::move(s.deferred);
        s.deferred.clear();
        for (auto& fn : work) {
            try {
                fn();
            } catch (std::exception const& e) {
                log().error("fluids: deferred action failed: {}", e.what());
            }
        }
        if (s.ticks % 20 == 0) s.intents.clear();
        // a copy: machines may be removed while ticking
        auto machines = s.machines;
        for (auto const& [p, kind] : machines) {
            auto* region = regionOf(p.dim);
            if (!region || !region->hasChunksAt(toBlockPos(p), 0, false)) continue;
            auto id = region->getBlock(toBlockPos(p)).getTypeName();
            if (kindOf(id) != kind) {
                if (kind == Kind::Hose) syncHose(p, 0, 0, true);
                untrack(p);
                continue;
            }
            try {
                switch (kind) {
                case Kind::Spout:
                    tickSpout(p);
                    break;
                case Kind::Drain:
                    tickDrain(p);
                    break;
                case Kind::Hose:
                    tickHose(p);
                    break;
                case Kind::Pfi:
                    tickPfi(p);
                    break;
                case Kind::Source:
                    break;
                }
            } catch (std::exception const& e) {
                log().error("fluids: machine at {} {} {} failed: {}", p.x, p.y, p.z, e.what());
            }
        }
        tickSources(s.ticks);
        if (s.ticks % 5 == 0) applyFluidEffects();
    }

    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::feature::fluids

LV_REGISTER_MODULE(lv::feature::fluids::FluidsModule);
