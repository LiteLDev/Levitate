// Schematics unit: world, block-state, item and message helpers.

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/dataloadhelper/DataLoadHelper.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/I18n.h"

LV_REGISTER_CONFIG("schematics", lv::feature::schematics::Config);

namespace lv::feature::schematics {

namespace items = common::items;

Config const& config() { return lv::config::get<Config>(); }

// ---- world ----------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
GlobalPos gp(int dim, cs::Pos const& p) { return {dim, p.x, p.y, p.z}; }
cs::Pos   cp(GlobalPos const& p) { return {p.x, p.y, p.z}; }

bool loaded(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r && p.y >= r->getMinHeight() && p.y < r->getMaxHeight() && r->hasChunksAt(bp(p), 0, false);
}

Block const* blockAt(GlobalPos const& p) {
    if (!loaded(p)) return nullptr;
    return &regionOf(p.dim)->getBlock(bp(p));
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? std::string(b->getTypeName()) : std::string();
}

int stateOr(GlobalPos const& p, char const* name, int fallback) {
    auto const* b = blockAt(p);
    if (!b) return fallback;
    return game::intState(*b, name).value_or(fallback);
}

bool place(GlobalPos const& p, Block const& block, int flags) {
    if (!loaded(p)) return false;
    BlockChangeContext ctx{};
    return regionOf(p.dim)->setBlock(bp(p), block, flags, nullptr, ctx);
}

bool placeId(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto const* b = game::blockWith(id, states);
    return b && place(p, *b);
}

bool clearAt(GlobalPos const& p) {
    auto const* air = game::blockWith("minecraft:air");
    return air && place(p, *air);
}

void destroyAt(GlobalPos const& p, bool drop) {
    auto level = ll::service::getLevel();
    if (!level || !loaded(p)) return;
    BlockChangeContext ctx{};
    level->destroyBlock(*regionOf(p.dim), bp(p), drop, ctx);
}

bool solid(Block const& block) { return !block.isAir() && block.mCachedComponentData->mIsSolid; }

bool holdsData(GlobalPos const& p, Block const& block) {
    if (block.getBlockType().getBlockEntityType() != BlockActorType::Undefined) return true;
    return common::blockdata::has(p);
}

bool unbreakable(Block const& block) {
    if (block.isAir()) return false;
    std::string const& n = block.getTypeName();
    if (n.find("water") != std::string::npos || n.find("lava") != std::string::npos) return false;
    return block.getBlockType().getDestroySpeed() < 0;
}

bool replaceable(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r && loaded(p) && game::replaceable(*r, bp(p));
}

// ---- block states ---------------------------------------------------------------------------------

namespace {

std::mutex                                       gStateMutex;
std::map<Block const*, cs::BlockState>           gStateCache;
std::map<std::string, Block const*>              gResolveCache;

std::optional<cs::nbt::Tag> toCore(CompoundTag const& tag) {
    auto bin = tag.toBinaryNbt(true);
    auto res = cs::nbt::read(bin, cs::nbt::Endian::Little);
    if (!res) return std::nullopt;
    return std::move(res->root);
}

Block const* fromCore(cs::nbt::Tag const& tag) {
    auto bin = cs::nbt::write(tag, "", cs::nbt::Endian::Little);
    auto ct  = CompoundTag::fromBinaryNbt(bin, true);
    if (!ct) return nullptr;
    auto b = Block::tryGetFromRegistry(*ct);
    return b ? b.as_ptr() : nullptr;
}

/// Converts `value` to the type of `like` (ints <-> bytes); nullopt when incompatible.
std::optional<cs::nbt::Tag> coerce(cs::nbt::Tag const& value, cs::nbt::Tag const& like) {
    using cs::nbt::Type;
    if (like.type() == Type::String) {
        if (value.asString()) return value;
        return std::nullopt;
    }
    auto n = value.asInt();
    if (!n) {
        // "true" / "false" strings for bits
        if (auto const* s = value.asString()) {
            if (*s == "true") n = 1;
            else if (*s == "false") n = 0;
        }
        if (!n) return std::nullopt;
    }
    switch (like.type()) {
    case Type::Byte:
        return cs::nbt::Tag(static_cast<std::int8_t>(*n));
    case Type::Short:
        return cs::nbt::Tag(static_cast<std::int16_t>(*n));
    case Type::Long:
        return cs::nbt::Tag(static_cast<std::int64_t>(*n));
    case Type::Int:
        return cs::nbt::Tag(static_cast<std::int32_t>(*n));
    default:
        return std::nullopt;
    }
}

Block const* resolveName(std::string const& name, cs::nbt::Compound const& states) {
    auto def = Block::tryGetFromRegistry(HashedString(name));
    if (!def) return nullptr;
    // exact match first (a Bedrock palette entry)
    {
        cs::nbt::Compound tag;
        tag.set("name", cs::nbt::Tag(name));
        tag.set("states", cs::nbt::Tag(states));
        if (auto const* b = fromCore(cs::nbt::Tag(tag))) {
            if (b->getTypeName() == def->getTypeName()) return b;
        }
    }
    auto base = toCore(*def->mSerializationId);
    if (!base || !base->asCompound()) return def.as_ptr();
    auto* root = base->asCompound();
    auto* st   = root->find("states") ? root->find("states")->asCompound() : nullptr;
    if (!st) return def.as_ptr();
    // apply each known state that still gives a valid block
    Block const* best = def.as_ptr();
    for (std::size_t i = 0; i < states.keys.size(); ++i) {
        auto* cur = st->find(states.keys[i]);
        if (!cur) continue;
        auto v = coerce(states.values[i], *cur);
        if (!v) continue;
        auto saved = *cur;
        *cur       = *v;
        if (auto const* b = fromCore(*base)) best = b;
        else *cur = saved;
    }
    return best;
}

} // namespace

cs::BlockState stateOf(Block const& block) {
    std::lock_guard lock(gStateMutex);
    if (auto it = gStateCache.find(&block); it != gStateCache.end()) return it->second;
    cs::BlockState st;
    st.name = block.getTypeName();
    if (auto tag = toCore(*block.mSerializationId); tag && tag->asCompound()) {
        auto const& c = *tag->asCompound();
        if (auto const* s = c.find("states"); s && s->asCompound()) st.states = *s->asCompound();
        if (auto const* v = c.find("version")) st.version = static_cast<std::int32_t>(v->asInt().value_or(0));
    }
    gStateCache.emplace(&block, st);
    return st;
}

Block const* resolve(cs::BlockState const& state) {
    std::string key = state.describe();
    for (auto const& a : state.alternatives) key += "|" + a;
    {
        std::lock_guard lock(gStateMutex);
        if (auto it = gResolveCache.find(key); it != gResolveCache.end()) return it->second;
    }
    Block const* out = resolveName(state.name, state.states);
    for (std::size_t i = 0; !out && i < state.alternatives.size(); ++i) out = resolveName(state.alternatives[i], state.states);
    std::lock_guard lock(gStateMutex);
    gResolveCache[key] = out;
    return out;
}

void clearBlockCaches() {
    std::lock_guard lock(gStateMutex);
    gStateCache.clear();
    gResolveCache.clear();
}

std::unique_ptr<CompoundTag> gameTag(cs::nbt::Tag const& tag) {
    auto ct = CompoundTag::fromBinaryNbt(cs::nbt::write(tag, "", cs::nbt::Endian::Little), true);
    if (!ct) return nullptr;
    return std::make_unique<CompoundTag>(std::move(*ct));
}

std::string blockSnbt(Block const& block) { return block.mSerializationId->toSnbt(SnbtFormat::Minimize, 0); }

Block const* blockFromSnbt(std::string const& snbt) {
    auto tag = CompoundTag::fromSnbt(snbt);
    if (!tag) return nullptr;
    auto b = Block::tryGetFromRegistry(*tag);
    return b ? b.as_ptr() : nullptr;
}

// ---- messages / effects ----------------------------------------------------------------------------

void message(Player& player, std::string const& text) { player.sendMessage(text); }

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly msg;
    msg.mType = TextPacketType::Tip;
    msg.mMessage->assign(text);
    pkt.mBody = msg;
    pkt.sendTo(player);
}

void sound(GlobalPos const& p, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{p.x * 8 + 4, p.y * 8 + 4, p.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(bp(p), DimensionType(p.dim));
}

void particle(int dim, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

std::optional<std::pair<cs::Pos, int>> lookTrace(Player& player, double range) {
    auto& region = player.getDimensionBlockSource();
    auto  eye    = player.getEyePos();
    auto  dir    = player.getViewVector(1.f);
    return cs::raycast(eye.x, eye.y, eye.z, dir.x, dir.y, dir.z, range, [&](cs::Pos p) {
        BlockPos b{p.x, p.y, p.z};
        if (p.y < region.getMinHeight() || p.y >= region.getMaxHeight() || !region.hasChunksAt(b, 0, false)) return false;
        auto const& block = region.getBlock(b);
        if (block.isAir()) return false;
        std::string const& n = block.getTypeName();
        return n.find("water") == std::string::npos && n.find("lava") == std::string::npos;
    });
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

std::int64_t uidOf(Player const& player) { return const_cast<Player&>(player).getOrCreateUniqueID().rawID; }

namespace {
std::uint64_t                      gTick = 0;
std::vector<std::function<void()>> gDeferred;
} // namespace

std::uint64_t currentTick() { return gTick; }
void          defer(std::function<void()> fn) { gDeferred.push_back(std::move(fn)); }

namespace detail {
void runDeferred() {
    ++gTick;
    auto work = std::move(gDeferred);
    gDeferred.clear();
    for (auto& fn : work) fn();
}
void clearDeferred() { gDeferred.clear(); }
} // namespace detail

Json& data(GlobalPos const& p) {
    auto& doc = common::blockdata::edit(p);
    auto& d   = doc["schematics"];
    if (!d.is_object()) d = Json::object();
    return d;
}

Json const* dataIf(GlobalPos const& p) {
    auto const* doc = common::blockdata::get(p);
    if (!doc || !doc->contains("schematics")) return nullptr;
    auto const& d = (*doc)["schematics"];
    return d.is_object() ? &d : nullptr;
}

bool inReach(Player const& player, GlobalPos const& p, double reach) {
    if (static_cast<int>(player.getDimensionId()) != p.dim) return false;
    auto   pos = player.getFeetPos();
    double dx = pos.x - (p.x + 0.5), dy = pos.y + 1.6 - (p.y + 0.5), dz = pos.z - (p.z + 0.5);
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

// ---- items ------------------------------------------------------------------------------------------

std::optional<ItemStack> makeItem(std::string_view id, int count) { return items::make(id, count, 0); }

bool isItem(ItemStack const& s, std::string_view id) { return !items::isEmpty(s) && items::idOf(s) == id; }

void give(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy) || !items::isEmpty(copy)) {
        if (!items::isEmpty(copy)) player.drop(copy, false);
    }
    player.sendInventory(false);
}

void dropAt(GlobalPos const& p, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(p.dim);
    if (!level || !region) return;
    level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}, 10);
}

ItemStack takeFromInventory(Player& player, int slot, int count) {
    auto& inv = player.getInventory();
    if (slot < 0 || slot >= inv.getContainerSize()) return items::empty();
    ItemStack s = inv.getItem(slot);
    if (items::isEmpty(s)) return items::empty();
    int n = count < 0 ? items::countOf(s) : std::min(count, items::countOf(s));
    inv.setItem(slot, items::withCount(s, items::countOf(s) - n));
    player.sendInventory(false);
    return items::withCount(s, n);
}

std::optional<SchematicData> schematicData(ItemStack const& stack) {
    if (!isItem(stack, kSchematic)) return std::nullopt;
    auto j = items::getData(stack, "schematic");
    if (!j.is_object() || !j.contains("file")) return std::nullopt;
    SchematicData d;
    d.file     = j.value("file", "");
    d.owner    = j.value("owner", "");
    d.deployed = j.value("deployed", false);
    auto vec   = [&](char const* key) {
        auto a = j.value(key, Json::array());
        return a.is_array() && a.size() == 3 ? cs::Pos{a[0].get<int>(), a[1].get<int>(), a[2].get<int>()} : cs::Pos{};
    };
    d.anchor             = vec("anchor");
    d.size               = vec("size");
    d.transform.rotation = std::clamp(j.value("rotation", 0), 0, 3);
    d.transform.mirror   = static_cast<cs::Mirror>(std::clamp(j.value("mirror", 0), 0, 2));
    if (d.file.empty()) return std::nullopt;
    return d;
}

void setSchematicData(ItemStack& stack, SchematicData const& d) {
    items::setData(
        stack,
        "schematic",
        Json{
            {"file",     d.file                                         },
            {"owner",    d.owner                                        },
            {"deployed", d.deployed                                     },
            {"anchor",   {d.anchor.x, d.anchor.y, d.anchor.z}           },
            {"rotation", d.transform.rotation                           },
            {"mirror",   static_cast<int>(d.transform.mirror)           },
            {"size",     {d.size.x, d.size.y, d.size.z}                 },
    }
    );
    updateSchematicLore(stack);
}

void updateSchematicLore(ItemStack& stack) {
    auto d = schematicData(stack);
    if (!d) {
        stack.setCustomLore({"§c" + std::string(translate("create.schematic.invalid", "[!] Invalid Item - Use the Schematic Table instead"))});
        return;
    }
    std::vector<std::string> lore{"§6" + d->file};
    if (d->deployed) {
        lore.push_back("§7" + std::string(translate("create.schematic.position", "Position")) + ": " + std::to_string(d->anchor.x) + ", "
                       + std::to_string(d->anchor.y) + ", " + std::to_string(d->anchor.z));
    }
    stack.setCustomLore(lore);
}

std::optional<ItemStack> makeSchematic(std::string const& file, std::string const& owner) {
    auto s = makeItem(kSchematic);
    if (!s) return std::nullopt;
    SchematicData d;
    d.file  = file;
    d.owner = owner;
    if (auto loaded = loadSchematic(owner, file)) d.size = loaded->schematic.size;
    setSchematicData(*s, d);
    return s;
}

// ---- block entities and entities (Create's StructureTemplate with entities) ----------------------

namespace {

cs::nbt::Tag floatList(std::initializer_list<double> v) {
    cs::nbt::List l{cs::nbt::Type::Float, {}};
    for (double x : v) l.items.emplace_back(static_cast<float>(x));
    return cs::nbt::Tag(std::move(l));
}

std::vector<double> numbers(cs::nbt::Tag const* t) {
    std::vector<double> out;
    if (!t || !t->asList()) return out;
    for (auto const& i : t->asList()->items) {
        if (auto const* f = std::get_if<float>(&i.v)) out.push_back(*f);
        else if (auto const* d = std::get_if<double>(&i.v)) out.push_back(*d);
        else if (auto n = i.asInt()) out.push_back(static_cast<double>(*n));
    }
    return out;
}

/// The game's DefaultDataLoadHelper (no data: only its vtable), for loading saved NBT as is.
DataLoadHelper& defaultLoadHelper() {
    static void** helper = DefaultDataLoadHelper::$vftable();
    return *reinterpret_cast<DataLoadHelper*>(&helper);
}

/// Where a point of the schematic (block units from its local origin) lands, and the transformed
/// horizontal direction (dx, dz): mirror then rotate, as the blocks (cs::apply).
std::pair<std::array<double, 3>, std::array<double, 2>>
transformPoint(cs::Transform const& t, cs::Pos anchor, std::array<double, 3> local, std::array<double, 2> dir) {
    cs::Pos o  = cs::apply(t, {0, 0, 0});
    cs::Pos ax = cs::apply(t, {1, 0, 0}) - o;
    cs::Pos az = cs::apply(t, {0, 0, 1}) - o;
    cs::Pos b{static_cast<int>(std::floor(local[0])), static_cast<int>(std::floor(local[1])), static_cast<int>(std::floor(local[2]))};
    cs::Pos wb = anchor + cs::apply(t, b);
    double  fx = local[0] - b.x - 0.5, fz = local[2] - b.z - 0.5;
    std::array<double, 3> at{wb.x + 0.5 + ax.x * fx + az.x * fz, wb.y + (local[1] - b.y), wb.z + 0.5 + ax.z * fx + az.z * fz};
    std::array<double, 2> d{ax.x * dir[0] + az.x * dir[1], ax.z * dir[0] + az.z * dir[1]};
    return {at, d};
}

} // namespace

void captureExtras(int dim, cs::Pos lo, cs::Pos hi, cs::Schematic& s) {
    auto* region = regionOf(dim);
    if (!region) return;
    auto ctx = SaveContextFactory::createCloneSaveContext();
    for (int x = lo.x; x <= hi.x; ++x)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z) {
                GlobalPos g{dim, x, y, z};
                if (!loaded(g)) continue;
                auto* be = region->getBlockEntity(BlockPos{x, y, z});
                if (!be) continue;
                CompoundTag tag;
                if (!be->save(tag, *ctx)) continue;
                if (auto t = toCore(tag)) s.blockEntities[s.index({x - lo.x, y - lo.y, z - lo.z})] = std::move(*t);
            }
    // entities whose position is inside the area (Create: getEntitiesOfClass over the bounds), no players
    AABB box{Vec3{float(lo.x), float(lo.y), float(lo.z)}, Vec3{float(hi.x + 1), float(hi.y + 1), float(hi.z + 1)}};
    for (auto* a : region->getEntities(box, 0.f)) {
        if (!a || a->isPlayer()) continue;
        Vec3 const& p = a->getPosition();
        if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x >= hi.x + 1 || p.y >= hi.y + 1 || p.z >= hi.z + 1) continue;
        CompoundTag tag;
        if (!a->save(tag)) continue;
        if (auto t = toCore(tag)) s.entityTags.push_back(std::move(*t));
    }
    s.entities = static_cast<int>(s.entityTags.size());
}

bool placeBlockEntity(GlobalPos const& at, cs::nbt::Tag const& data) {
    auto* region = regionOf(at.dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level || !data.asCompound()) return false;
    auto* be = region->getBlockEntity(bp(at));
    if (!be) return false;
    auto copy = data;
    copy.asCompound()->set("x", cs::nbt::Tag(std::int32_t(at.x)));
    copy.asCompound()->set("y", cs::nbt::Tag(std::int32_t(at.y)));
    copy.asCompound()->set("z", cs::nbt::Tag(std::int32_t(at.z)));
    auto tag = gameTag(copy);
    if (!tag) return false;
    be->load(*level, *tag, defaultLoadHelper());
    be->refresh(*region);
    return true;
}

Actor* placeEntity(int dim, cs::nbt::Tag const& data, cs::Pos origin, cs::Transform const& t, cs::Pos anchor) {
    auto* region = regionOf(dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level || !data.asCompound()) return nullptr;
    auto  copy = data;
    auto& c    = *copy.asCompound();
    auto  pos  = numbers(c.find("Pos"));
    if (pos.size() != 3) return nullptr;
    auto   rot = numbers(c.find("Rotation"));
    double yaw = rot.size() == 2 ? rot[0] : 0, pitch = rot.size() == 2 ? rot[1] : 0;
    double rad = yaw * 3.14159265358979323846 / 180.0;
    auto [at, dir] = transformPoint(t, anchor, {pos[0] - origin.x, pos[1] - origin.y, pos[2] - origin.z}, {-std::sin(rad), std::cos(rad)});
    double newYaw  = std::atan2(-dir[0], dir[1]) * 180.0 / 3.14159265358979323846;
    c.set("Pos", floatList({at[0], at[1], at[2]}));
    c.set("Rotation", floatList({newYaw, pitch}));
    // a new entity: the game gives it a new id; riders / leashes of the copy are not linked
    for (auto const* k : {"UniqueID", "LinksTag", "LeasherID"}) {
        auto i = std::find(c.keys.begin(), c.keys.end(), k);
        if (i == c.keys.end()) continue;
        auto n = static_cast<std::size_t>(i - c.keys.begin());
        c.keys.erase(i);
        c.values.erase(c.values.begin() + static_cast<std::ptrdiff_t>(n));
    }
    auto tag = gameTag(copy);
    if (!tag) return nullptr;
    auto owner = level->getActorFactory().loadActor(tag.get(), defaultLoadHelper(), region->getDimension().mHeightRange.get(), nullptr);
    if (!owner) return nullptr;
    return level->addEntity(*region, std::move(owner));
}

} // namespace lv::feature::schematics
