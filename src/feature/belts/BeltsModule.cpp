// Belts unit: module wiring (events, world lifecycle, persistence, index), shared world helpers,
// the kinetic spec of belts and ejectors, display entities and the public API (BeltApi.h).

#include <algorithm>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("belts", lv::feature::belts::BeltsConfig);

namespace lv::feature::belts {

namespace blockdata = common::blockdata;
namespace citems    = common::items;
namespace k         = core::kinetics;

BeltsConfig const& config() { return lv::config::get<BeltsConfig>(); }

State& state() {
    static State s;
    return s;
}

// ---- world helpers -------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

bool loadedAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    if (!r) return false;
    BlockPos bp = game::blockPos(p);
    if (bp.y < r->getMinHeight() || bp.y >= r->getMaxHeight()) return false;
    return r->hasChunksAt(bp, 0, false);
}

Block const* blockAt(GlobalPos const& p) {
    if (!loadedAt(p)) return nullptr;
    return &regionOf(p.dim)->getBlock(game::blockPos(p));
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? b->getTypeName() : std::string();
}

GlobalPos offset(GlobalPos p, cb::Vec3i const& d) {
    p.x += d.x;
    p.y += d.y;
    p.z += d.z;
    return p;
}

cb::Vec3i vecOf(GlobalPos const& p) { return {p.x, p.y, p.z}; }

bool setBlockQuiet(GlobalPos const& p, Block const& block) {
    auto* r = regionOf(p.dim);
    if (!r) return false;
    ++state().editing;
    bool ok = game::setBlock(*r, game::blockPos(p), block);
    --state().editing;
    return ok;
}

bool clearBlockQuiet(GlobalPos const& p) {
    auto const* air = game::blockWith("minecraft:air");
    return air && setBlockQuiet(p, *air);
}

std::optional<cb::SegmentState> segmentOf(Block const& block) {
    if (block.getTypeName() != kBeltId) return std::nullopt;
    cb::SegmentState s;
    s.facing = game::intState(block, kFacing).value_or(0) & 3;
    s.slope  = cb::Slope(std::clamp(game::intState(block, kSlope).value_or(0), 0, 4));
    s.part   = cb::Part(std::clamp(game::intState(block, kPart).value_or(1), 0, 3));
    s.casing = cb::Casing(std::clamp(game::intState(block, kCasing).value_or(0), 0, 2));
    s.color  = game::intState(block, kDyed).value_or(0) ? game::intState(block, kColor).value_or(0) : -1;
    return s;
}

std::optional<cb::SegmentState> segmentAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? segmentOf(*b) : std::nullopt;
}

Block const* beltBlock(cb::SegmentState const& s) {
    return game::blockWith(
        kBeltId,
        {{kFacing, s.facing & 3},
         {kSlope, int(s.slope)},
         {kPart, int(s.part)},
         {kCasing, int(s.casing)},
         {kDyed, s.color >= 0 ? 1 : 0},
         {kColor, std::max(s.color, 0)}}
    );
}

std::optional<cb::Axis> shaftAxisAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    if (!b) return std::nullopt;
    auto const& id = b->getTypeName();
    if (id != kShaftId && id != kTestShaftId) return std::nullopt;
    return cb::Axis(std::clamp(game::intState(*b, kAxis).value_or(0), 0, 2));
}

Block const* shaftBlock(cb::Axis axis) {
    if (auto const* b = game::blockWith(kShaftId, {{kAxis, int(axis)}})) return b;
    return game::blockWith(kTestShaftId, {{kAxis, int(axis)}});
}

std::optional<ItemStack> makeStack(std::string const& id, int count) { return citems::make(id, count); }

std::optional<ItemStack> shaftItem(int count) {
    if (auto s = makeStack(kShaftId, count)) return s;
    return makeStack(kTestShaftId, count);
}

bool isDepotId(std::string const& id) { return id == kDepotId || id == kEjectorId; }

void spawnItem(GlobalPos const& dimRef, double x, double y, double z, ItemStack const& stack, double vx, double vy, double vz) {
    if (citems::isEmpty(stack)) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(dimRef.dim);
    if (!level || !region) return;
    auto* a = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(x), float(y), float(z)}, 0);
    if (a && (vx != 0 || vy != 0 || vz != 0)) {
        a->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{float(vx), float(vy), float(vz)};
    }
}

void giveToPlayer(Player& player, ItemStack stack) {
    if (citems::isEmpty(stack)) return;
    if (!player.add(stack) || !stack.isNull()) {
        if (!citems::isEmpty(stack)) {
            Vec3 p = player.getPosition();
            spawnItem(game::globalPos(int(player.getDimensionId()), BlockPos{0, 0, 0}), p.x, p.y, p.z, stack);
        }
    }
    player.sendInventory(false);
}

void playSound(GlobalPos const& at, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{at.x * 8 + 4, at.y * 8 + 4, at.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(game::blockPos(at), DimensionType(at.dim));
}

void actionbar(Player& player, std::string const& text) {
    auto                            pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void particle(GlobalPos const& dimRef, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dimRef.dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

std::string uuidOf(Player const& player) { return player.getUuid().asString(); }

// ---- belt metadata --------------------------------------------------------------------------------------

namespace {
Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(int dim, Json const& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    return GlobalPos{dim, j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}
constexpr int kWalkLimit = 1000;
} // namespace

std::vector<GlobalPos> chainFrom(GlobalPos const& controller) {
    std::vector<GlobalPos> out;
    GlobalPos              cur = controller;
    for (int i = 0; i < kWalkLimit; ++i) {
        auto s = segmentAt(cur);
        if (!s) break;
        out.push_back(cur);
        auto next = cb::nextSegmentOffset(*s, true);
        if (!next) break;
        cur = offset(cur, *next);
    }
    return out;
}

namespace {
std::optional<GlobalPos> walkToStart(GlobalPos p) {
    for (int i = 0; i < kWalkLimit; ++i) {
        auto s = segmentAt(p);
        if (!s) return std::nullopt;
        if (s->part == cb::Part::Start) return p;
        auto prev = cb::nextSegmentOffset(*s, false);
        if (!prev) return std::nullopt;
        GlobalPos q = offset(p, *prev);
        if (!segmentAt(q)) return std::nullopt; // a broken chain
        p = q;
    }
    return std::nullopt;
}
} // namespace

std::optional<SegmentInfo> segmentInfo(GlobalPos const& p) {
    if (!segmentAt(p)) return std::nullopt;
    if (auto const* d = blockdata::get(p)) {
        auto it = d->find("belt");
        if (it != d->end() && it->is_object() && it->contains("c")) {
            auto c = posFrom(p.dim, (*it)["c"]);
            if (c) return SegmentInfo{*c, it->value("i", 0), it->value("n", 0)};
        }
    }
    auto start = walkToStart(p);
    if (!start) return std::nullopt;
    auto chain = chainFrom(*start);
    auto it    = std::find_if(chain.begin(), chain.end(), [&](GlobalPos const& q) { return q == p; });
    if (it == chain.end()) return std::nullopt;
    return SegmentInfo{*start, int(it - chain.begin()), int(chain.size())};
}

std::optional<GlobalPos> rebuildBelt(GlobalPos const& anySegment) {
    auto start = walkToStart(anySegment);
    if (!start) return std::nullopt;
    auto chain = chainFrom(*start);
    int  n     = int(chain.size());
    for (int i = 0; i < n; ++i) {
        auto& d    = blockdata::edit(chain[std::size_t(i)]);
        auto  prev = d.contains("belt") && d["belt"].is_object() ? d["belt"] : Json::object();
        prev["c"]  = posJson(*start);
        prev["i"]  = i;
        prev["n"]  = n;
        if (i != 0) prev.erase("items");
        d["belt"] = prev;
    }
    indexAdd(*start);
    auto& s = state();
    if (auto it = s.belts.find(*start); it != s.belts.end()) {
        it->second->length = n;
        if (auto st = segmentAt(*start)) it->second->state = *st;
    }
    return start;
}

std::uint64_t newItemId() { return state().nextItemId++; }

namespace {
Json itemsJson(std::vector<cb::Item> const& items, std::map<std::uint64_t, StackData> const& stacks) {
    Json arr = Json::array();
    for (auto const& it : items) {
        auto sd = stacks.find(it.id);
        if (sd == stacks.end() || citems::isEmpty(sd->second.stack)) continue;
        Json j{
            {"s", citems::toJson(sd->second.stack)},
            {"p", it.beltPosition},
            {"o", it.sideOffset},
            {"t", it.targetSideOffset},
            {"f", it.insertedFrom},
            {"a", it.insertedAt},
            {"l", it.locked},
            {"pt", sd->second.processingTime},
        };
        if (!sd->second.processedBy.empty()) j["pb"] = sd->second.processedBy;
        if (!sd->second.extra.is_null()) j["x"] = sd->second.extra;
        arr.push_back(std::move(j));
    }
    return arr;
}

void readItems(Json const& arr, std::vector<cb::Item>& out, std::map<std::uint64_t, StackData>& stacks) {
    if (!arr.is_array()) return;
    for (auto const& j : arr) {
        try {
            StackData sd;
            sd.stack = citems::fromJson(j.value("s", Json()));
            if (citems::isEmpty(sd.stack)) continue;
            sd.processingTime = j.value("pt", -1);
            sd.processedBy    = j.value("pb", std::string());
            if (j.contains("x")) sd.extra = j["x"];
            cb::Item it;
            it.id               = newItemId();
            it.beltPosition     = j.value("p", 0.f);
            it.prevBeltPosition = it.beltPosition;
            it.sideOffset       = j.value("o", 0.f);
            it.prevSideOffset   = it.sideOffset;
            it.targetSideOffset = j.value("t", 0.f);
            it.insertedFrom     = j.value("f", 1);
            it.insertedAt       = j.value("a", 0);
            it.locked           = j.value("l", false);
            stacks[it.id]       = std::move(sd);
            out.push_back(it);
        } catch (...) {
            log().warn("belts: skipped an unreadable item");
        }
    }
}
} // namespace

Belt* beltFor(GlobalPos const& controller) {
    auto& s = state();
    if (auto it = s.belts.find(controller); it != s.belts.end()) return it->second.get();
    auto seg = segmentAt(controller);
    if (!seg || seg->part != cb::Part::Start) return nullptr;
    auto b        = std::make_unique<Belt>();
    b->controller = controller;
    b->length     = int(chainFrom(controller).size());
    b->state      = *seg;
    if (auto const* d = blockdata::get(controller)) {
        if (auto it = d->find("belt"); it != d->end() && it->is_object()) {
            std::vector<cb::Item> items;
            readItems(it->value("items", Json::array()), items, b->stacks);
            b->inv.movementPositive = it->value("positive", true);
            for (auto& i : items) b->inv.insertSorted(i);
        }
    }
    indexAdd(controller);
    auto* raw = b.get();
    s.belts.emplace(controller, std::move(b));
    return raw;
}

Belt* beltOfSegment(GlobalPos const& segment) {
    auto info = segmentInfo(segment);
    if (!info) return nullptr;
    auto* b = beltFor(info->controller);
    if (!b) {
        // stale metadata (an edit moved the controller): rebuild once
        auto c = rebuildBelt(segment);
        if (!c) return nullptr;
        b = beltFor(*c);
    }
    return b;
}

namespace {
Json depotJson(Depot const& d) {
    Json j = Json::object();
    std::vector<cb::Item> items;
    if (d.held) items.push_back(*d.held);
    j["held"]     = itemsJson(items, d.stacks);
    j["incoming"] = itemsJson(d.incoming, d.stacks);
    Json outs     = Json::array();
    for (auto const& o : d.outputs) outs.push_back(citems::isEmpty(o) ? Json() : citems::toJson(o));
    j["outputs"] = outs;
    if (d.ejector) {
        j["stackSize"] = d.stackSize;
        j["target"]    = Json::array({d.targetH, d.targetV});
        j["state"]     = int(d.state);
        j["lid"]       = d.lid;
        Json air       = Json::array();
        for (auto const& a : d.airborne) {
            air.push_back({{"s", citems::toJson(a.data.stack)}, {"t", a.t}, {"h", a.launch.horizontal}, {"v", a.launch.vertical}, {"f", a.facing}});
        }
        j["airborne"] = air;
    }
    return j;
}
} // namespace

Depot* depotFor(GlobalPos const& pos) {
    auto& s = state();
    if (auto it = s.depots.find(pos); it != s.depots.end()) return it->second.get();
    auto id = idAt(pos);
    if (!isDepotId(id)) return nullptr;
    auto d     = std::make_unique<Depot>();
    d->pos     = pos;
    d->ejector = id == kEjectorId;
    if (auto const* doc = blockdata::get(pos)) {
        if (auto it = doc->find("depot"); it != doc->end() && it->is_object()) {
            std::vector<cb::Item> held;
            readItems(it->value("held", Json::array()), held, d->stacks);
            if (!held.empty()) d->held = held.front();
            readItems(it->value("incoming", Json::array()), d->incoming, d->stacks);
            auto outs = it->value("outputs", Json::array());
            for (std::size_t i = 0; i < outs.size() && i < d->outputs.size(); ++i) d->outputs[i] = citems::fromJson(outs[i]);
            d->stackSize = std::clamp(it->value("stackSize", 0), 0, 64);
            auto t       = it->value("target", Json::array({1, 0}));
            if (t.is_array() && t.size() == 2) {
                d->targetH = t[0].get<int>();
                d->targetV = t[1].get<int>();
            }
            d->state = EjectorState(std::clamp(it->value("state", 0), 0, 2));
            d->lid   = it->value("lid", 0.f);
            for (auto const& a : it->value("airborne", Json::array())) {
                Airborne ab;
                ab.data.stack = citems::fromJson(a.value("s", Json()));
                if (citems::isEmpty(ab.data.stack)) continue;
                ab.launch = cb::planLaunch(a.value("h", 1), a.value("v", 0));
                ab.t      = a.value("t", 0.0);
                ab.facing = a.value("f", 0);
                ab.origin = pos;
                d->airborne.push_back(std::move(ab));
            }
        }
    }
    // every depot has a document, so that BlockData's erase listener sees it go
    blockdata::edit(pos)["depot"] = depotJson(*d);
    indexAdd(pos);
    auto* raw = d.get();
    s.depots.emplace(pos, std::move(d));
    return raw;
}

void indexAdd(GlobalPos const& p) {
    auto& s = state();
    if (s.index.insert(p).second) s.indexDirty = true;
}

void indexRemove(GlobalPos const& p) {
    auto& s = state();
    if (s.index.erase(p)) s.indexDirty = true;
}

// ---- items in the world -------------------------------------------------------------------------------

cb::Vec3d itemWorldPos(Belt const& belt, cb::Item const& item) {
    auto      seg = segmentAt(belt.controller).value_or(belt.state); // the START may be gone (breaking)
    cb::Vec3d v   = cb::vectorForOffset(seg, belt.length, item.beltPosition);
    // side offset: along the clockwise horizontal of the facing (Create negates it for x belts)
    cb::Vec3i side = cb::facingNormal(cb::clockwise(seg.facing));
    double    so   = item.sideOffset * (cb::facingAxis(seg.facing) == cb::Axis::X ? -1 : 1);
    return cb::Vec3d{belt.controller.x + .5, belt.controller.y + .5, belt.controller.z + .5} + v
         + cb::Vec3d{side.x * so, cb::kBeltTop - .5, side.z * so};
}

void ejectAll(Belt& belt) {
    belt.inv.flushQueues();
    for (auto& it : belt.inv.items) {
        auto sd = belt.stacks.find(it.id);
        if (sd == belt.stacks.end()) continue;
        auto p = itemWorldPos(belt, it);
        spawnItem(belt.controller, p.x, p.y + .1, p.z, sd->second.stack);
        removeDisplay(sd->second);
    }
    belt.inv.items.clear();
    belt.stacks.clear();
}

void dropDepotContents(Depot& d) {
    double x = d.pos.x + .5, y = d.pos.y + 1, z = d.pos.z + .5;
    for (auto& [id, sd] : d.stacks) {
        spawnItem(d.pos, x, y, z, sd.stack);
        removeDisplay(sd);
    }
    for (auto& o : d.outputs) spawnItem(d.pos, x, y, z, o);
    for (auto& a : d.airborne) {
        spawnItem(d.pos, x, y, z, a.data.stack);
        removeDisplay(a.data);
    }
    d.stacks.clear();
    d.held.reset();
    d.incoming.clear();
    d.outputs = {};
    d.airborne.clear();
}

namespace {
Actor* fetchActor(std::int64_t id) {
    if (!id) return nullptr;
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    return level->fetchEntity(uid, false);
}
} // namespace

namespace {
/// The client mod's display item of a stack (Create's belt / depot item look): lying, half size,
/// a pile for bigger stacks, turned by a fixed angle per stack; it hides the fallback entity.
void publishShown(GlobalPos const& owner, StackData& data, cb::Vec3d const& at, std::int64_t hideActor) {
    static std::uint64_t nextKey = 0;
    if (!data.shownKey) data.shownKey = ++nextKey;
    else if (data.shownOwner != owner) itemrender::remove(data.shownOwner, static_cast<std::int64_t>(data.shownKey));
    data.shownOwner = owner;
    itemrender::DisplayItem d;
    d.stack     = data.stack;
    d.at        = {at.x, at.y, at.z};
    d.at.yaw    = static_cast<float>((data.shownKey * 137) % 360);
    d.at.scale  = .5f;
    d.style     = itemrender::Style::Lying;
    d.pile      = true;
    d.lerpTicks = 1;
    d.hideActor = hideActor;
    itemrender::set(owner, static_cast<std::int64_t>(data.shownKey), d);
}

void removeEntity(StackData& data) {
    if (!data.display) return;
    if (auto* a = fetchActor(data.display)) a->remove();
    state().displays.erase(data.display);
    data.display = 0;
}

/// The fallback item entity (vanilla players); returns its runtime id (0 when there is none).
std::int64_t syncEntity(GlobalPos const& dimRef, StackData& data, cb::Vec3d const& at) {
    if (!config().displayItems) {
        removeEntity(data);
        return 0;
    }
    auto* actor = fetchActor(data.display);
    if (actor && actor->getEntityTypeId() == ActorType::ItemEntity) {
        auto const& shown = static_cast<ItemActor*>(actor)->mItem.get();
        if (!citems::sameItemSameData(shown, data.stack) || citems::countOf(shown) != citems::countOf(data.stack)) {
            removeEntity(data);
            actor = nullptr;
        }
    } else if (actor) {
        removeEntity(data);
        actor = nullptr;
    } else if (data.display) {
        state().displays.erase(data.display);
        data.display = 0;
    }
    // item entities float: their position is the bottom of the item, a little above the surface
    Vec3 target{float(at.x), float(at.y), float(at.z)};
    if (!actor) {
        auto  level  = ll::service::getLevel();
        auto* region = regionOf(dimRef.dim);
        if (!level || !region) return 0;
        BlockPos bp{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))};
        if (!region->hasChunksAt(bp, 0, false)) return 0;
        auto* a = level->getSpawner().spawnItem(*region, data.stack, nullptr, target, 0);
        if (!a) return 0;
        a->addTag(kDisplayTag);
        auto* item          = static_cast<ItemActor*>(a);
        item->mPickupDelay  = 0x7fff;
        item->mLifeTime     = 0x7fffffff;
        a->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{0, 0, 0};
        data.display = a->getOrCreateUniqueID().rawID;
        state().displays.insert(data.display);
        return static_cast<std::int64_t>(a->getRuntimeID().rawID);
    }
    Vec3 const& cur = actor->getPosition();
    float       dx = cur.x - target.x, dy = cur.y - target.y, dz = cur.z - target.z;
    actor->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{0, 0, 0};
    if (dx * dx + dy * dy + dz * dz > 1e-5f) actor->teleportTo(target, false, 0, 0, false);
    return static_cast<std::int64_t>(actor->getRuntimeID().rawID);
}
} // namespace

void removeDisplay(StackData& data) {
    if (data.shownKey) {
        itemrender::remove(data.shownOwner, static_cast<std::int64_t>(data.shownKey));
        data.shownKey = 0;
    }
    removeEntity(data);
}

void syncDisplay(GlobalPos const& dimRef, StackData& data, cb::Vec3d const& at) {
    if (citems::isEmpty(data.stack)) {
        removeDisplay(data);
        return;
    }
    publishShown(dimRef, data, at, syncEntity(dimRef, data, at));
}

TransportedItem toApi(cb::Item const& item, StackData const& data) {
    TransportedItem t;
    t.stack          = data.stack;
    t.beltPosition   = item.beltPosition;
    t.sideOffset     = item.sideOffset;
    t.insertedFrom   = item.insertedFrom;
    t.insertedAt     = item.insertedAt;
    t.locked         = item.locked;
    t.processingTime = data.processingTime;
    t.processedBy    = data.processedBy;
    t.extra          = data.extra;
    return t;
}

void fromApi(TransportedItem const& api, cb::Item& item, StackData& data) {
    data.stack          = api.stack;
    item.beltPosition   = api.beltPosition;
    item.sideOffset     = api.sideOffset;
    item.insertedFrom   = api.insertedFrom;
    item.insertedAt     = api.insertedAt;
    item.locked         = api.locked;
    data.processingTime = api.processingTime;
    data.processedBy    = api.processedBy;
    data.extra          = api.extra;
}

// ---- registries ----------------------------------------------------------------------------------------

namespace {
template <class T>
std::map<std::string, T, std::less<>>& registryOf() {
    static std::map<std::string, T, std::less<>> m;
    return m;
}
} // namespace

void registerProcessor(std::string const& blockId, Processor processor) { registryOf<Processor>()[blockId] = std::move(processor); }
void unregisterProcessor(std::string const& blockId) { registryOf<Processor>().erase(blockId); }
void registerAboveInteractor(std::string const& blockId, PassFn fn) { registryOf<PassFn>()[blockId] = std::move(fn); }
void registerDirectInput(std::string const& blockId, DirectInput input) { registryOf<DirectInput>()[blockId] = std::move(input); }

Processor const* processorFor(std::string const& blockId) {
    auto& m  = registryOf<Processor>();
    auto  it = m.find(blockId);
    return it == m.end() ? nullptr : &it->second;
}
PassFn const* interactorFor(std::string const& blockId) {
    auto& m  = registryOf<PassFn>();
    auto  it = m.find(blockId);
    return it == m.end() ? nullptr : &it->second;
}
DirectInput const* directInputFor(std::string const& blockId) {
    auto& m  = registryOf<DirectInput>();
    auto  it = m.find(blockId);
    return it == m.end() ? nullptr : &it->second;
}

bool processingBlocked(GlobalPos const& transporter) {
    GlobalPos   between = offset(transporter, {0, 1, 0});
    auto const* b       = blockAt(between);
    if (!b || b->isAir()) return false;
    if (interactorFor(b->getTypeName())) return false; // funnels don't block
    auto* r = regionOf(between.dim);
    return r && !game::replaceable(*r, game::blockPos(between));
}

// ---- public API ---------------------------------------------------------------------------------------

bool isTransporter(GlobalPos const& pos) {
    if (auto s = segmentAt(pos)) return cb::canTransportObjects(s->slope);
    return isDepotId(idAt(pos));
}

bool handleCenteredProcessing(GlobalPos const& pos, float maxDistanceFromCentre, ProcessFn const& fn) {
    if (segmentAt(pos)) {
        auto  info = segmentInfo(pos);
        auto* belt = beltOfSegment(pos);
        if (!info || !belt) return false;
        return applyOnBelt(*belt, float(info->index) + .5f, maxDistanceFromCentre, fn);
    }
    if (auto* d = depotFor(pos)) return applyOnDepot(*d, maxDistanceFromCentre, fn);
    return false;
}

bool handleProcessingOnAllItems(GlobalPos const& pos, ProcessFn const& fn) {
    if (segmentAt(pos)) return handleCenteredProcessing(pos, .51f, fn);
    if (auto* d = depotFor(pos)) return applyOnDepot(*d, 1.f, fn);
    return false;
}

std::vector<TransportedItem> itemsAt(GlobalPos const& pos) {
    std::vector<TransportedItem> out;
    if (segmentAt(pos)) {
        auto  info = segmentInfo(pos);
        auto* belt = beltOfSegment(pos);
        if (!info || !belt) return out;
        for (auto const& it : belt->inv.all()) {
            if (it.beltPosition < info->index || it.beltPosition > info->index + 1) continue;
            if (auto sd = belt->stacks.find(it.id); sd != belt->stacks.end()) out.push_back(toApi(it, sd->second));
        }
        return out;
    }
    if (auto* d = depotFor(pos)) {
        if (d->held) {
            if (auto sd = d->stacks.find(d->held->id); sd != d->stacks.end()) out.push_back(toApi(*d->held, sd->second));
        }
        for (auto const& it : d->incoming)
            if (auto sd = d->stacks.find(it.id); sd != d->stacks.end()) out.push_back(toApi(it, sd->second));
    }
    return out;
}

bool hasDirectInput(GlobalPos const& pos) { return directInputFor(idAt(pos)) != nullptr; }

ItemStack insertDirect(GlobalPos const& pos, TransportedItem const& item, int side, bool simulate) {
    auto const* in = directInputFor(idAt(pos));
    if (!in || !in->insert) return item.stack;
    if (in->canInsertFromSide && !in->canInsertFromSide(pos, side)) return item.stack;
    return in->insert(pos, item, side, simulate);
}

std::optional<BeltInfo> beltAt(GlobalPos const& pos) {
    auto seg  = segmentAt(pos);
    auto info = segmentInfo(pos);
    if (!seg || !info) return std::nullopt;
    BeltInfo out;
    out.controller   = info->controller;
    out.index        = info->index;
    out.length       = info->length;
    out.speed        = kinetics::speedAt(info->controller);
    auto cseg        = segmentAt(info->controller).value_or(*seg);
    out.movementFace = cb::faceOfFacing(cb::movementFacing(out.speed, cseg.facing));
    out.facing       = seg->facing;
    out.slope        = int(seg->slope);
    out.casing       = int(seg->casing);
    out.color        = seg->color;
    return out;
}

// ---- kinetics ------------------------------------------------------------------------------------------

namespace {
cb::SegmentState segOfKState(k::BlockState const& st) {
    cb::SegmentState s;
    s.facing = st.get(kFacing) & 3;
    s.slope  = cb::Slope(std::clamp(st.get(kSlope), 0, 4));
    s.part   = cb::Part(std::clamp(st.get(kPart, 1), 0, 3));
    return s;
}

k::KineticSpec beltSpec() {
    k::KineticSpec s;
    s.states       = {kFacing, kSlope, kPart};
    s.rotationAxis = [](k::BlockState const& st) -> std::optional<k::Axis> { return k::Axis(cb::rotationAxis(segOfKState(st))); };
    s.hasShaftTowards = [](k::BlockState const& st, k::Face f) {
        auto seg = segOfKState(st);
        return cb::hasPulley(seg.part) && k::axisOf(f) == k::Axis(cb::rotationAxis(seg));
    };
    // Create's BeltBlockEntity.propagateRotationTo: segments of one belt turn together.
    s.propagate = [](k::NodeView const& from, k::NodeView const& to, k::Offset d, bool byAxis, bool) -> float {
        if (byAxis || !from.state || !to.state || to.state->id != kBeltId) return 0;
        auto a = segOfKState(*from.state), b = segOfKState(*to.state);
        cb::Vec3i dv{d.x, d.y, d.z};
        auto      af = cb::nextSegmentOffset(a, true), ab = cb::nextSegmentOffset(a, false);
        auto      bf = cb::nextSegmentOffset(b, true), bb = cb::nextSegmentOffset(b, false);
        if (af && *af == dv && bb && *bb == cb::Vec3i{-dv.x, -dv.y, -dv.z}) return 1;
        if (ab && *ab == dv && bf && *bf == cb::Vec3i{-dv.x, -dv.y, -dv.z}) return 1;
        return 0;
    };
    // sloped belts reach their diagonal neighbours (Create's addPropagationLocations)
    s.extraNeighbours = [](k::NodeView const& self, std::vector<GlobalPos>& out) {
        if (!self.state) return;
        auto seg = segOfKState(*self.state);
        for (bool fwd : {true, false}) {
            auto o = cb::nextSegmentOffset(seg, fwd);
            if (!o || std::abs(o->x) + std::abs(o->y) + std::abs(o->z) <= 1) continue;
            out.push_back(offset(self.pos, *o));
        }
    };
    s.stressKey = "belt";
    return s;
}

k::KineticSpec ejectorSpec() {
    k::KineticSpec s;
    s.states          = {kFacing};
    s.rotationAxis    = [](k::BlockState const& st) -> std::optional<k::Axis> { return k::Axis(cb::facingAxis(cb::clockwise(st.get(kFacing)))); };
    s.hasShaftTowards = [](k::BlockState const& st, k::Face f) { return k::axisOf(f) == k::Axis(cb::facingAxis(cb::clockwise(st.get(kFacing)))); };
    s.impact          = 2;
    s.rotorState      = kRotor;
    s.rotorModel      = "geometry.levitate.weighted_ejector";
    s.rotorTexture    = "levitate_weighted_ejector";
    return s;
}
} // namespace

LV_REGISTER_KINETIC_BLOCK(kBeltId, beltSpec());
LV_REGISTER_KINETIC_BLOCK(kEjectorId, ejectorSpec());

// ---- persistence ---------------------------------------------------------------------------------------

namespace {
constexpr char const* kIndexKey = "lv:belts:index";
}

void loadWorld() {
    closeWorld();
    auto& s = state();
    if (auto j = game::db::load(kIndexKey); j && j->is_array()) {
        for (auto const& e : *j) {
            if (e.is_array() && e.size() == 4) s.index.insert(GlobalPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
        }
    }
    s.indexDirty = false;
    log().info("belts: {} belts, depots and ejectors indexed", s.index.size());
}

void saveWorld() {
    auto& s = state();
    if (!game::db::available()) return;
    for (auto& [pos, b] : s.belts) {
        if (loadedAt(pos) && !segmentAt(pos)) continue; // gone (an unloaded belt is still saved)
        auto& d = blockdata::edit(pos);
        if (!d.contains("belt") || !d["belt"].is_object()) d["belt"] = Json::object();
        d["belt"]["items"]    = itemsJson(b->inv.all(), b->stacks);
        d["belt"]["positive"] = b->inv.movementPositive;
    }
    for (auto& [pos, dep] : s.depots) {
        if (loadedAt(pos) && !isDepotId(idAt(pos))) continue;
        blockdata::edit(pos)["depot"] = depotJson(*dep);
    }
    if (s.indexDirty) {
        Json arr = Json::array();
        for (auto const& p : s.index) arr.push_back({p.dim, p.x, p.y, p.z});
        game::db::save(kIndexKey, arr);
        s.indexDirty = false;
    }
}

void closeWorld() {
    auto& s = state();
    for (auto& [pos, b] : s.belts)
        for (auto& [id, sd] : b->stacks) removeDisplay(sd);
    for (auto& [pos, d] : s.depots) {
        for (auto& [id, sd] : d->stacks) removeDisplay(sd);
        for (auto& a : d->airborne) removeDisplay(a.data);
    }
    s.belts.clear();
    s.depots.clear();
    s.index.clear();
    s.displays.clear();
    s.tasks.clear();
    s.removals.clear();
    s.playerBroke.clear();
    s.placeIntents.clear();
    s.connector.clear();
    s.ejectorTargets.clear();
    s.indexDirty = false;
}

// ---- module -------------------------------------------------------------------------------------------

namespace {

void runTick() {
    auto& s = state();
    ++s.tick;
    auto tasks = std::exchange(s.tasks, {});
    for (auto& fn : tasks) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("belts: task failed: {}", e.what());
        }
    }
    processRemovals();

    std::vector<GlobalPos> idx(s.index.begin(), s.index.end());
    for (auto const& p : idx) {
        if (!loadedAt(p)) continue;
        auto id = idAt(p);
        if (id == kBeltId) {
            auto seg = segmentAt(p);
            if (seg && seg->part == cb::Part::Start) {
                if (auto* b = beltFor(p)) tickBelt(*b);
                continue;
            }
        } else if (isDepotId(id)) {
            if (auto* d = depotFor(p)) {
                if (d->ejector != (id == kEjectorId)) {
                    s.depots.erase(p);
                    d = depotFor(p);
                }
                if (d) tickDepot(*d);
            }
            continue;
        }
        // not a controller / depot any more
        indexRemove(p);
        if (auto it = s.belts.find(p); it != s.belts.end()) {
            for (auto& [i, sd] : it->second->stacks) removeDisplay(sd);
            s.belts.erase(it);
        }
        if (auto it = s.depots.find(p); it != s.depots.end()) {
            dropDepotContents(*it->second);
            s.depots.erase(it);
        }
    }
    if (s.tick % 5 == 0) connectorPreview();
    s.playerBroke.clear();
    s.placeIntents.clear();
}

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    auto&        s      = state();
    Block const& before = ev.previousBlock();
    Block const& after  = ev.newBlock();
    auto const&  prevId = before.getTypeName();
    auto const&  nowId  = after.getTypeName();
    bool         wasBelt = prevId == kBeltId, isBelt = nowId == kBeltId;
    bool         wasDepot = isDepotId(prevId), isDepot = isDepotId(nowId);
    if (!wasBelt && !isBelt && !wasDepot && !isDepot) return;
    auto pos = game::globalPos(region, ev.pos());
    // the game reports some changes twice: handle each change once
    static std::tuple<std::uint64_t, GlobalPos, Block const*, Block const*> last{};
    auto current = std::tuple{s.tick, pos, &before, &after};
    if (current == last) return;
    last = current;
    if (s.editing > 0) return;

    if (wasBelt && !isBelt) {
        if (auto seg = segmentOf(before)) s.removals.push_back({pos, *seg});
    } else if (isBelt && !wasBelt) {
        // a belt that appeared without us (commands, structures): give it its metadata
        s.tasks.push_back([pos] { rebuildBelt(pos); });
    }
    if (isDepot && prevId != nowId) {
        indexAdd(pos);
        if (nowId == kEjectorId) {
            if (auto it = s.placeIntents.find(pos); it != s.placeIntents.end()) {
                auto intent = it->second;
                s.placeIntents.erase(it);
                s.tasks.push_back([pos, intent] { onEjectorPlaced(pos, intent); });
            }
        }
    }
}

} // namespace

class BeltsModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        game::db::onWorldReady([] { loadWorld(); });
        game::db::onWorldSave([] { saveWorld(); });
        game::db::onWorldClosed([] { closeWorld(); });
        if (game::db::available()) loadWorld(); // BDS: the world exists before the mod is enabled

        blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const& prevId) {
            if (!isDepotId(prevId)) return;
            auto& s = state();
            if (auto it = s.depots.find(pos); it != s.depots.end()) {
                dropDepotContents(*it->second);
                s.depots.erase(it);
            } else if (auto d = doc.find("depot"); d != doc.end() && d->is_object()) {
                // never loaded this session: drop what the document holds
                Depot tmp;
                tmp.pos = pos;
                std::vector<cb::Item> held;
                readItems(d->value("held", Json::array()), held, tmp.stacks);
                readItems(d->value("incoming", Json::array()), tmp.incoming, tmp.stacks);
                auto outs = d->value("outputs", Json::array());
                for (std::size_t i = 0; i < outs.size() && i < tmp.outputs.size(); ++i) tmp.outputs[i] = citems::fromJson(outs[i]);
                dropDepotContents(tmp);
            }
            indexRemove(pos);
        });

        common::items::registerBlock(
            kBeltId,
            common::items::Handler{
                .slots = [](common::Access const& a) { return isTransporter(game::globalPos(a.region, a.pos)) ? 1 : 0; },
                .getSlot =
                    [](common::Access const& a, int) {
                        auto items = itemsAt(game::globalPos(a.region, a.pos));
                        return items.empty() ? ItemStack() : items.front().stack;
                    },
                .insert =
                    [](common::Access const& a, ItemStack const& stack, bool simulate) {
                        return insertOnSegment(game::globalPos(a.region, a.pos), stack, simulate);
                    },
                .extract =
                    [](common::Access const& a, common::items::ItemPredicate const& filter, int amount, bool simulate) {
                        return extractFromSegment(game::globalPos(a.region, a.pos), amount, filter, simulate);
                    },
            }
        );
        registerDirectInput(
            kBeltId,
            DirectInput{
                .canInsertFromSide = [](GlobalPos const& p, int side) { return canInsertFromSide(p, side); },
                .insert = [](GlobalPos const& p, TransportedItem const& item, int side, bool simulate) {
                    return insertFromSide(p, item, side, simulate);
                },
            }
        );
        registerDepotCapabilities();
        registerEjectorBox();

        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        installHooks();
    }

    void uninstall() {
        uninstallHooks();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveWorld();
        closeWorld();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::feature::belts

LV_REGISTER_MODULE(lv::feature::belts::BeltsModule);
