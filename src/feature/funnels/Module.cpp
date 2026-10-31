// Item logistics unit (U12): module, block index, world helpers, player hooks, block changes.
//
// Every funnel, tunnel, chute, smart chute, item vault and creative crate is kept in an index
// (persisted in the world DB under "lv:funnels:index") and ticked like Create's block entities.
// Player actions arrive through the shared dispatch (common/Interactions.h; SimulatedPlayers too).

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltApi.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("funnels", lv::feature::funnels::Config);

namespace lv::feature::funnels {

namespace items     = common::items;
namespace blockdata = common::blockdata;

Config const& config() { return lv::config::get<Config>(); }

Kind kindOf(std::string_view id) {
    if (id == kAndesiteFunnel) return Kind::AndesiteFunnel;
    if (id == kBrassFunnel) return Kind::BrassFunnel;
    if (id == kAndesiteTunnel) return Kind::AndesiteTunnel;
    if (id == kBrassTunnel) return Kind::BrassTunnel;
    if (id == kChute) return Kind::Chute;
    if (id == kSmartChute) return Kind::SmartChute;
    if (id == kItemVault) return Kind::Vault;
    if (id == kCreativeCrate) return Kind::Crate;
    return Kind::None;
}

namespace {

constexpr char kDbIndex[] = "lv:funnels:index";

struct Intent {
    std::int64_t  player = 0;
    int           face   = 1;
    bool          sneaking = false;
    Vec3          look{0, 0, 0};
    std::uint64_t tick = 0;
};

struct State {
    std::map<GlobalPos, Kind>               index;
    bool                                    indexDirty = false;
    std::map<GlobalPos, Intent>             intents;
    std::set<GlobalPos>                     shapeUpdates;
    std::vector<std::function<void()>>      deferred;
    std::vector<std::pair<GlobalPos, std::vector<ItemStack>>> drops;
    std::unordered_map<std::int64_t, std::uint64_t> lastUse;
    std::uint64_t                           tick = 0;
    bool                                    loaded = false;
};
State& st() {
    static State s;
    return s;
}

void saveIndex() {
    if (!st().indexDirty || !game::db::available()) return;
    Json arr = Json::array();
    for (auto const& [p, k] : st().index) arr.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbIndex, arr);
    st().indexDirty = false;
}

void loadIndex() {
    st().index.clear();
    st().indexDirty = false;
    if (auto j = game::db::load(kDbIndex); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() != 4) continue;
            // the kind is re-read from the world when the chunk is loaded
            st().index[{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}] = Kind::None;
        }
    }
    st().loaded = true;
}

/// Block-local hit position from a click position (relative or absolute).
std::array<double, 3> localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

bool ourBlockItem(std::string const& id) { return kindOf(id) != Kind::None; }

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

/// Where a plain right-click would put a block.
BlockPos placementPos(BlockSource& region, BlockPos const& clicked, int face) {
    if (game::replaceable(region, clicked)) return clicked;
    return detail::offset(clicked, face);
}

} // namespace

namespace detail {

// ---- world helpers --------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

BlockPos  toBlockPos(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
GlobalPos toGlobal(int dim, BlockPos const& p) { return GlobalPos{dim, p.x, p.y, p.z}; }

BlockPos offset(BlockPos const& p, int face, int n) {
    auto o = cf::faceOffset(face);
    return BlockPos{p.x + o[0] * n, p.y + o[1] * n, p.z + o[2] * n};
}
GlobalPos offset(GlobalPos const& p, int face, int n) {
    auto o = cf::faceOffset(face);
    return GlobalPos{p.dim, p.x + o[0] * n, p.y + o[1] * n, p.z + o[2] * n};
}

Block const* loadedBlock(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return nullptr;
    if (!region.hasChunksAt(pos, 0, false)) return nullptr;
    return &region.getBlock(pos);
}

std::string idAt(BlockSource& region, BlockPos const& pos) {
    auto const* b = loadedBlock(region, pos);
    return b ? std::string(b->getTypeName()) : std::string{};
}

Kind kindAt(BlockSource& region, BlockPos const& pos) { return kindOf(idAt(region, pos)); }

int state(Block const& block, char const* name, int fallback) { return game::intState(block, name).value_or(fallback); }

bool setStates(BlockSource& region, BlockPos const& pos, std::vector<std::pair<std::string_view, int>> const& states) {
    auto const* block = loadedBlock(region, pos);
    if (!block) return false;
    bool changed = false;
    for (auto const& [k, v] : states) {
        auto cur = game::intState(*block, k);
        if (cur && *cur != v) changed = true;
    }
    if (!changed) return false;
    // our blocks only have integer states: rebuild the full state list
    auto const&                                   nbt = block->mSerializationId.get();
    std::vector<std::pair<std::string_view, int>> all;
    std::vector<std::string>                      names;
    if (nbt.contains("states", Tag::Type::Compound)) {
        for (auto const& [key, value] : nbt["states"].get<CompoundTag>()) names.push_back(key);
    }
    for (auto const& n : names) {
        int v = game::intState(*block, n).value_or(0);
        for (auto const& [k, nv] : states)
            if (k == n) v = nv;
        all.emplace_back(n, v);
    }
    auto const* next = game::blockWith(block->getTypeName(), all);
    if (!next || next == block) return false;
    return game::setBlock(region, pos, *next);
}

bool powered(BlockSource& region, BlockPos const& pos) { return common::redstone::powered(region, pos); }

bool solidFace(BlockSource& region, BlockPos const& pos, int face) {
    auto const* block = loadedBlock(region, pos);
    if (!block || block->isAir()) return false;
    return block->canProvideSupport(static_cast<uchar>(face), BlockSupportType::Center);
}

bool replaceableAt(BlockSource& region, BlockPos const& pos) {
    if (!loadedBlock(region, pos)) return false;
    return game::replaceable(region, pos);
}

ItemActor* spawnItem(BlockSource& region, ItemStack const& stack, Vec3 const& pos, Vec3 const& velocity) {
    auto level = ll::service::getLevel();
    if (!level || items::isEmpty(stack)) return nullptr;
    auto* actor = level->getSpawner().spawnItem(region, stack, nullptr, pos, 10);
    if (actor) const_cast<Vec3&>(actor->getPosDelta()) = velocity;
    return actor;
}

std::vector<ItemActor*> itemsIn(BlockSource& region, Vec3 const& min, Vec3 const& max) {
    std::vector<ItemActor*> out;
    for (auto* a : region.getEntities(AABB{min, max}, 0.f, ActorType::ItemEntity)) {
        if (!a || !a->isAlive() || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto const& p = a->getPosition();
        if (p.x < min.x || p.y < min.y || p.z < min.z || p.x > max.x || p.y > max.y || p.z > max.z) continue;
        out.push_back(static_cast<ItemActor*>(a));
    }
    return out;
}

void playSound(BlockSource& region, BlockPos const& pos, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(pos, region.getDimensionId());
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

Json const* docOf(GlobalPos const& pos) {
    auto const* d = blockdata::get(pos);
    if (!d) return nullptr;
    auto it = d->find("funnels");
    return it == d->end() || !it->is_object() ? nullptr : &*it;
}

Json& editDoc(GlobalPos const& pos) {
    auto& d = blockdata::edit(pos);
    if (!d["funnels"].is_object()) d["funnels"] = Json::object();
    return d["funnels"];
}

ItemStack extractCounted(
    BlockSource&                                 region,
    BlockPos const&                              pos,
    int                                          face,
    std::function<bool(ItemStack const&)> const& filter,
    int                                          amount,
    bool                                         exact,
    bool                                         simulate
) {
    if (amount <= 0) return items::empty();
    if (!exact) return items::extract(region, pos, face, filter, amount, simulate);
    // Create's EXACTLY mode: only a kind with at least `amount` items (capped to its stack size)
    std::vector<ItemStack> tooFew;
    for (int attempt = 0; attempt < 256; ++attempt) {
        auto pred = [&](ItemStack const& s) {
            if (!filter(s)) return false;
            for (auto const& k : tooFew)
                if (items::sameItemSameData(k, s)) return false;
            return true;
        };
        auto found = items::extract(region, pos, face, pred, amount, true);
        if (items::isEmpty(found)) return items::empty();
        int need = std::min(amount, static_cast<int>(found.getMaxStackSize()));
        if (items::countOf(found) >= need) {
            if (simulate) return items::withCount(found, need);
            ItemStack kind = found;
            return items::extract(region, pos, face, [kind](ItemStack const& s) { return items::sameItemSameData(kind, s); }, need, false);
        }
        tooFew.push_back(found);
    }
    return items::empty();
}

int filterMaxStack(GlobalPos const& pos) {
    // Create's FilteringBehaviour.getMaxStackSize: the filter's max stack size, 64 without one
    auto f = common::filterslot::get(pos, "filter");
    return items::isEmpty(f) ? 64 : std::max(1, static_cast<int>(f.getMaxStackSize()));
}

cf::AmountSetting amountOf(GlobalPos const& pos) {
    cf::AmountSetting s;
    int               max = filterMaxStack(pos);
    s.count               = max;
    if (auto const* d = docOf(pos)) {
        s.count = std::clamp(d->value("count", max), 1, max);
        s.upTo  = d->value("upTo", true);
    }
    return s;
}

std::string amountCountLabel(GlobalPos const& pos) {
    // Create's getCountLabelForValueBox: "*" for "up to" the max stack size, else the number
    if (filterMaxStack(pos) <= 1) return {};
    auto s = amountOf(pos);
    return s.upTo && s.count == filterMaxStack(pos) ? std::string("*") : std::to_string(s.count);
}

void clampAmountToFilter(GlobalPos const& pos) {
    // Create's setFilter: an "exactly" count is capped at the new filter's max stack size
    auto const* d = docOf(pos);
    if (!d || !d->contains("count")) return;
    int max = filterMaxStack(pos);
    if (!d->value("upTo", true) && d->value("count", 64) > max) editDoc(pos)["count"] = max;
}

void setAmount(GlobalPos const& pos, cf::AmountSetting s) {
    auto& d   = editDoc(pos);
    d["count"] = s.count;
    d["upTo"]  = s.upTo;
}

void registerAmountBox(std::string const& blockId, std::function<std::optional<std::array<double, 3>>(Block const&, int face)> centre) {
    common::valuebox::BoxSpec spec;
    spec.id            = "amount";
    spec.label         = "create.logistics.filter.extracted_amount";
    spec.labelFallback = "Extracted Amount";
    spec.board         = [](Block const&) {
        core::common::BoardSpec b;
        b.maxValue          = 64;
        b.milestoneInterval = 16;
        b.rows              = {tr("create.logistics.filter.up_to", "Up to"), tr("create.logistics.filter.exactly", "Exactly")};
        return b;
    };
    // Create's FilteringBehaviour.createBoard: up to the filter's max stack size; no count for
    // filters that don't stack (isCountVisible)
    spec.boardAt = [](GlobalPos const& p, Block const&) {
        core::common::BoardSpec b;
        b.maxValue          = filterMaxStack(p);
        b.milestoneInterval = 16;
        b.rows              = {tr("create.logistics.filter.up_to", "Up to"), tr("create.logistics.filter.exactly", "Exactly")};
        return b;
    };
    spec.activeAt = [](GlobalPos const& p) { return filterMaxStack(p) > 1; };
    spec.formatAt = [](GlobalPos const& p, Block const&, core::common::ValueSettings v) {
        return cf::amountLabel(cf::AmountSetting{v.value, v.row == 0}, filterMaxStack(p), tr("create.logistics.filter.any_amount_short", "Any"));
    };
    spec.centre = std::move(centre);
    spec.get    = [](GlobalPos const& p) {
        auto s = amountOf(p);
        return core::common::ValueSettings{s.upTo ? 0 : 1, s.count};
    };
    spec.set = [](GlobalPos const& p, core::common::ValueSettings v, Player&) {
        setAmount(p, cf::AmountSetting{std::clamp(v.value, 1, filterMaxStack(p)), v.row == 0});
    };
    spec.format = [](Block const&, core::common::ValueSettings v) {
        // (the board's labels are built per position: "Any" is the top of an "up to" row)
        return cf::amountLabel(cf::AmountSetting{v.value, v.row == 0}, 64, tr("create.logistics.filter.any_amount_short", "Any"));
    };
    common::valuebox::registerBox(blockId, std::move(spec));
}

void dropLater(GlobalPos const& pos, std::vector<ItemStack> stacks) {
    std::erase_if(stacks, [](ItemStack const& s) { return items::isEmpty(s); });
    if (!stacks.empty()) st().drops.emplace_back(pos, std::move(stacks));
}

void defer(std::function<void()> fn) { st().deferred.push_back(std::move(fn)); }

} // namespace detail

// ---- placement / interaction ------------------------------------------------------------------------

namespace {

using namespace detail;

/// Applies the placement rules of a freshly placed block (Create's getStateForPlacement).
void onPlaced(GlobalPos const& pos, Kind kind, std::optional<Intent> intent) {
    auto* region = regionOf(pos.dim);
    if (!region || kindAt(*region, toBlockPos(pos)) != kind) return;
    Player* player = intent ? playerById(intent->player) : nullptr;
    bool    sneak  = intent && intent->sneaking;
    int     face   = intent ? intent->face : cf::kUp;
    Vec3    look   = intent ? intent->look : Vec3{0, -1, 0};
    switch (kind) {
    case Kind::AndesiteFunnel:
    case Kind::BrassFunnel:
        if (intent) placeFunnel(player, *region, pos, sneak, look);
        else setStates(*region, toBlockPos(pos), {{kPowered, powered(*region, toBlockPos(pos)) ? 1 : 0}});
        break;
    case Kind::Chute:
    case Kind::SmartChute: placeChute(player, *region, pos, intent ? face : -1, sneak); break;
    case Kind::Vault: placeVault(player, *region, pos, intent ? face : -1, sneak); break;
    case Kind::AndesiteTunnel:
    case Kind::BrassTunnel: {
        // Create's BeltTunnelItem.updateCustomBlockEntityTag: an uncased segment below takes the
        // tunnel's casing
        auto below = offset(pos, cf::kDown);
        if (auto seg = common::beltlink::segmentAt(*region, toBlockPos(below)); seg && !seg->casing)
            belts::setCasing(below, kind == Kind::AndesiteTunnel ? 1 : 2);
        updateTunnelShape(*region, pos);
        break;
    }
    default: break;
    }
    if (player) log().info("funnels: {} placed at {} by {}", game::blockIdAt(*region, toBlockPos(pos)), pos.key(), player->getRealName());
}

/// A right-click on one of our blocks; true when it was handled (no placement / item use).
bool onUse(Player& player, BlockSource& region, BlockPos const& at, int face, Vec3 const& click, ItemStack& held) {
    auto const* block = loadedBlock(region, at);
    if (!block) return false;
    Kind kind = kindOf(block->getTypeName());
    if (kind == Kind::None) return false;
    std::string heldId = items::isEmpty(held) ? std::string{} : items::idOf(held);
    bool        sneak  = player.isSneaking();
    // a sneaking player with an item in hand places / uses the item (Java's secondary use)
    if (sneak && !heldId.empty()) return false;
    // filter slots and value boxes (U3) take precedence
    auto hit = localHit(at, click);
    if (common::filterslot::slotAt(*block, face, hit)) return false;
    if ((heldId.empty() || heldId == kWrench) && common::valuebox::boxAt(*block, face, hit)) return false;

    auto pos = toGlobal(static_cast<int>(player.getDimensionId()), at);
    if (heldId == kWrench) {
        if (sneak) return false; // dismantling is the wrench's own behaviour (U8)
        switch (kind) {
        case Kind::AndesiteFunnel:
        case Kind::BrassFunnel: return wrenchFunnel(player, region, pos, *block);
        case Kind::Chute:
        case Kind::SmartChute: return wrenchChute(player, region, pos, *block);
        case Kind::AndesiteTunnel:
        case Kind::BrassTunnel: return wrenchTunnel(player, region, pos, *block);
        case Kind::Vault: return wrenchVault(player, region, pos, *block, face);
        default: return false;
        }
    }
    switch (kind) {
    case Kind::AndesiteFunnel:
    case Kind::BrassFunnel: return useFunnel(player, region, pos, *block, face, held);
    case Kind::Chute:
    case Kind::SmartChute: return useChute(player, region, pos, *block, held);
    case Kind::AndesiteTunnel:
    case Kind::BrassTunnel: return useTunnel(player, region, pos, *block, held);
    default: return false;
    }
}

std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>>& handledUses() {
    static std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> m;
    return m;
}
bool recentlyHandled(std::int64_t uid, GlobalPos const& pos) {
    auto it = handledUses().find(uid);
    return it != handledUses().end() && it->second.first == pos && st().tick - it->second.second < 5;
}
void markHandled(std::int64_t uid, GlobalPos const& pos) { handledUses()[uid] = {pos, st().tick}; }

} // namespace

namespace ia = common::interactions;

namespace {

/// Right-clicks on our blocks, and the placement intent of our block items.
ia::Result useOnHandler(ia::UseOnContext const& c) {
    auto& player = c.player;
    auto& region = player.getDimensionBlockSource();
    auto  uid    = player.getOrCreateUniqueID().rawID;
    auto  at     = toBlockPos(c.pos);
    if (kindOf(c.blockId) != Kind::None) {
        // Bedrock repeats the use while the button is held: one action per click, and the
        // repeats of a handled click must not place the held block either
        bool handled = recentlyHandled(uid, c.pos);
        Vec3 hit{float(c.hit[0]), float(c.hit[1]), float(c.hit[2])}; // block-local (localHit keeps it)
        if (!handled && onUse(player, region, at, c.face, hit, ia::mutableItem(c.item))) {
            markHandled(uid, c.pos);
            handled = true;
        }
        if (handled) {
            player.sendInventory(false);
            return ia::Result::Consume;
        }
    }
    if ((c.itemId == kAndesiteTunnel || c.itemId == kBrassTunnel) && (c.sneaking || !c.block.getBlockType().isInteractiveBlock())) {
        // Create's BeltTunnelItem.canPlace: only on a horizontal belt
        auto target = placementPos(region, at, static_cast<uchar>(c.face));
        auto seg    = common::beltlink::segmentAt(region, offset(target, cf::kDown));
        if (!seg || !seg->horizontal) {
            if (c.firstEvent || player.isSimulatedPlayer()) log().info("funnels: {} can't be placed at {} (no horizontal belt below)", c.itemId, game::globalPos(region, target).key());
            return ia::Result::Deny;
        }
    }
    if (c.itemId == kChute && (c.sneaking || !c.block.getBlockType().isInteractiveBlock() || kindOf(c.blockId) != Kind::None)) {
        if (auto placed = chuteItemPlace(player, region, at, c.face, c.sneaking)) {
            if (*placed) markHandled(uid, c.pos);
            player.sendInventory(false);
            return *placed ? ia::Result::Consume : ia::Result::Deny;
        }
        if (!chuteCanSurvive(region, placementPos(region, at, static_cast<uchar>(c.face)))) {
            if (c.firstEvent || player.isSimulatedPlayer()) log().info("funnels: a chute can't be placed under a diagonal chute");
            return ia::Result::Deny;
        }
    }
    if (!c.itemId.empty() && ourBlockItem(c.itemId)) {
        Intent in;
        in.player   = uid;
        in.face     = c.face;
        in.sneaking = c.sneaking;
        in.look     = player.getViewVector();
        in.tick     = st().tick;
        st().intents[game::globalPos(region, placementPos(region, at, static_cast<uchar>(c.face)))] = in;
    }
    return ia::Result::Pass;
}

} // namespace

// ---- module -------------------------------------------------------------------------------------------

class FunnelsModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            installFunnels();
            installChutes();
            installTunnels();
            installVaults();
            installCrate();
            game::db::onWorldReady([] { loadIndex(); });
            game::db::onWorldSave([] { saveIndex(); });
            game::db::onWorldClosed([] {
                st() = State{};
                forgetVaults();
            });
            blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
                onErase(pos, doc, previous);
            });
        }
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { onTick(); }));
        mUseOn = ia::onUseOn({}, useOnHandler);
        if (game::db::available() && !st().loaded) loadIndex(); // BDS: the world exists already
    }

    void uninstall() {
        ia::remove(mUseOn);
        mUseOn = 0;
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveIndex();
    }

private:
    static void onErase(GlobalPos const& pos, Json const& doc, std::string const& previous) {
        Kind kind = kindOf(previous);
        if (kind == Kind::None) {
            // explicit erase: only care about our own documents
            if (!doc.contains("funnels")) return;
        }
        // the contents are dropped: forget the cached copies too (an explicit erase keeps the block)
        forgetFunnel(pos);
        forgetChute(pos);
        forgetTunnel(pos);
        if (doc.contains("funnels") && doc["funnels"].contains("items")) vaultChanged(pos);
        std::vector<ItemStack> drops;
        if (auto it = doc.find("funnels"); it != doc.end() && it->is_object()) {
            if (it->contains("item")) drops.push_back(items::fromJson((*it)["item"]));
            if (it->contains("stack")) drops.push_back(items::fromJson((*it)["stack"]));
            if (auto v = it->find("items"); v != it->end() && v->is_array())
                for (auto const& e : *v) drops.push_back(items::fromJson(e));
        }
        // (filter items in the slots are dropped centrally by the filter slot code, U55)
        dropLater(pos, std::move(drops));
    }

    static void onBlockChanged(ll::event::BlockChangedEvent& ev) {
        if (ev.layer() != 0) return;
        auto& region = ev.blockSource();
        if (region.getLevel().isClientSide()) return;
        auto const& now   = ev.newBlock();
        auto const& prev  = ev.previousBlock();
        std::string nowId  = now.getTypeName();
        std::string prevId = prev.getTypeName();
        Kind        nk     = kindOf(nowId);
        Kind        pk     = kindOf(prevId);
        auto        pos    = game::globalPos(region, ev.pos());
        bool        typeChanged = nowId != prevId;

        if (pk != Kind::None && typeChanged) {
            st().index.erase(pos);
            st().indexDirty = true;
            if (isFunnel(pk)) forgetFunnel(pos);
            if (isChute(pk)) forgetChute(pos);
            if (isTunnel(pk)) forgetTunnel(pos);
            if (pk == Kind::Vault) vaultChanged(pos);
        }
        if (nk != Kind::None && typeChanged) {
            st().index[pos] = nk;
            st().indexDirty = true;
            std::optional<Intent> intent;
            if (auto it = st().intents.find(pos); it != st().intents.end()) {
                if (st().tick - it->second.tick <= 20) intent = it->second;
                st().intents.erase(it);
            }
            defer([pos, nk, intent] { onPlaced(pos, nk, intent); });
            if (nk == Kind::Vault) vaultChanged(pos);
        } else if (nk == Kind::Vault && !typeChanged && game::intState(now, kAxis) != game::intState(prev, kAxis)) {
            vaultChanged(pos);
        }
        // neighbours that depend on this block: chute and tunnel shapes
        if (st().index.empty()) return;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0 && !typeChanged) continue;
                    GlobalPos n{pos.dim, pos.x + dx, pos.y + dy, pos.z + dz};
                    auto      it = st().index.find(n);
                    if (it == st().index.end()) continue;
                    if (isChute(it->second) || isTunnel(it->second) || it->second == Kind::None) st().shapeUpdates.insert(n);
                    // a funnel whose block below changed may get on / off a belt (Create's updateShape, DOWN)
                    else if (isFunnel(it->second) && dx == 0 && dy == 1 && dz == 0) st().shapeUpdates.insert(n);
                }
    }

    static void onTick() {
        auto& s = st();
        ++s.tick;
        if (!s.loaded && game::db::available()) loadIndex();
        // deferred actions and drops
        if (!s.deferred.empty()) {
            auto work = std::move(s.deferred);
            s.deferred.clear();
            for (auto& fn : work) {
                try {
                    fn();
                } catch (std::exception const& e) {
                    log().error("funnels: deferred action failed: {}", e.what());
                }
            }
        }
        if (!s.drops.empty()) {
            auto drops = std::move(s.drops);
            s.drops.clear();
            for (auto const& [pos, stacks] : drops) {
                auto* region = regionOf(pos.dim);
                if (!region) continue;
                for (auto const& stack : stacks)
                    spawnItem(*region, stack, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f}, Vec3{0, 0, 0});
            }
        }
        // stale placement intents (the item was used for something else)
        std::erase_if(s.intents, [&](auto const& e) { return s.tick - e.second.tick > 40; });

        // shape updates
        if (!s.shapeUpdates.empty()) {
            auto updates = std::move(s.shapeUpdates);
            s.shapeUpdates.clear();
            for (auto const& pos : updates) {
                auto* region = regionOf(pos.dim);
                if (!region) continue;
                Kind k = kindAt(*region, toBlockPos(pos));
                if (isChute(k)) updateChuteShape(*region, pos);
                else if (isTunnel(k)) updateTunnelShape(*region, pos);
                else if (isFunnel(k)) updateFunnelShape(*region, pos);
            }
        }

        beginChuteTick();
        std::vector<std::pair<GlobalPos, Kind>> entries(s.index.begin(), s.index.end());
        for (auto& [pos, kind] : entries) {
            auto* region = regionOf(pos.dim);
            if (!region) continue;
            auto bp = toBlockPos(pos);
            auto const* block = loadedBlock(*region, bp);
            if (!block) continue; // chunk not loaded: keep it in the index
            Kind actual = kindOf(block->getTypeName());
            if (actual != kind) {
                if (actual == Kind::None) {
                    s.index.erase(pos);
                    s.indexDirty = true;
                    continue;
                }
                s.index[pos] = actual; // first tick after loading the index
                kind = actual;
                if (kind == Kind::Vault) vaultChanged(pos);
            }
            try {
                switch (kind) {
                case Kind::AndesiteFunnel:
                case Kind::BrassFunnel: tickFunnel(*region, pos, *block, s.tick); break;
                case Kind::Chute:
                case Kind::SmartChute: tickChute(*region, pos, *block, s.tick); break;
                case Kind::AndesiteTunnel:
                case Kind::BrassTunnel: tickTunnel(*region, pos, *block, s.tick); break;
                default: break;
                }
            } catch (std::exception const& e) {
                log().error("funnels: tick of {} failed: {}", pos.key(), e.what());
            }
        }
        tickVaults(s.tick);
        flushDisplays();
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    ia::HandlerId                       mUseOn = 0;
};

} // namespace lv::feature::funnels

LV_REGISTER_MODULE(lv::feature::funnels::FunnelsModule);
