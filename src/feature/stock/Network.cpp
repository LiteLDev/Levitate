// Stock unit: logistics networks (Create's GlobalLogisticsManager / LogisticsManager), the
// packager seam, item tuning and the shared helpers of the unit.

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items     = common::items;
namespace blockdata = common::blockdata;

namespace {
constexpr char kDbNetworks[] = "lv:stock:networks";

std::uint64_t& tickCounter() {
    static std::uint64_t t = 0;
    return t;
}

std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> v;
    return v;
}

std::optional<PackagerProvider>& provider() {
    static std::optional<PackagerProvider> p;
    return p;
}

std::unordered_map<std::string, NetworkInfo>& networks() {
    static std::unordered_map<std::string, NetworkInfo> m;
    return m;
}
bool& networksDirty() {
    static bool d = false;
    return d;
}

struct CachedStock {
    std::uint64_t tick = 0;
    Stock         stock;
};
std::unordered_map<std::string, CachedStock>& summaries(bool accurate) {
    static std::unordered_map<std::string, CachedStock> normal, exact;
    return accurate ? exact : normal;
}
/// Last seen contents of each packager (Create's availableItems, for new arrivals).
std::map<GlobalPos, cs::Summary>& lastPackagerStock() {
    static std::map<GlobalPos, cs::Summary> m;
    return m;
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

constexpr int kDx[6]{0, 0, 0, 0, -1, 1}, kDy[6]{-1, 1, 0, 0, 0, 0}, kDz[6]{0, 0, -1, 1, 0, 0};

} // namespace

// ---- world helpers -------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

Block const* blockAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return nullptr;
    auto bp = game::blockPos(p);
    if (!region->hasChunksAt(bp, 0, false)) return nullptr;
    return &region->getBlock(bp);
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? std::string(b->getTypeName()) : std::string{};
}

int stateOr(GlobalPos const& p, char const* state, int fallback) {
    auto const* b = blockAt(p);
    if (!b) return fallback;
    return game::intState(*b, state).value_or(fallback);
}

bool setStates(GlobalPos const& p, std::vector<std::pair<std::string_view, int>> const& states) {
    auto* region = regionOf(p.dim);
    auto const* b = blockAt(p);
    if (!region || !b) return false;
    std::string id = b->getTypeName();
    std::vector<std::pair<std::string_view, int>> all;
    for (auto name : {kStateFace, kStateFacing, kStatePowered, kStateAxis, kStatePanels}) {
        if (auto v = game::intState(*b, name)) all.emplace_back(name, *v);
    }
    bool changed = false;
    for (auto const& [k, v] : states) {
        auto it = std::find_if(all.begin(), all.end(), [&](auto const& e) { return e.first == k; });
        if (it == all.end()) continue;
        if (it->second != v) changed = true;
        it->second = v;
    }
    if (!changed) return true;
    auto const* next = game::blockWith(id, all);
    if (!next) return false;
    return game::setBlock(*region, game::blockPos(p), *next);
}

GlobalPos offset(GlobalPos const& p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

std::uint64_t currentTick() { return tickCounter(); }

int receivedPower(GlobalPos const& p, int except) {
    auto* region = regionOf(p.dim);
    if (!region) return 0;
    auto bp   = game::blockPos(p);
    int  best = 0;
    for (int d = 0; d < 6 && best < 15; ++d) {
        if (d == except) continue;
        best = std::max(best, common::redstone::signalFrom(*region, {bp.x + kDx[d], bp.y + kDy[d], bp.z + kDz[d]}, bp));
    }
    return best;
}

Json const* dataOf(GlobalPos const& p) {
    auto const* d = blockdata::get(p);
    if (!d) return nullptr;
    auto it = d->find(kDataKey);
    return it == d->end() || !it->is_object() ? nullptr : &*it;
}

Json& editData(GlobalPos const& p) {
    auto& d = blockdata::edit(p);
    auto& s = d[kDataKey];
    if (!s.is_object()) s = Json::object();
    return s;
}

cs::AttachFace faceOf(GlobalPos const& p) { return static_cast<cs::AttachFace>(std::clamp(stateOr(p, kStateFace, 0), 0, 2)); }
int            facingOf(GlobalPos const& p) { return std::clamp(stateOr(p, kStateFacing, 2), 0, 3); }
GlobalPos      supportOf(GlobalPos const& p) {
    auto o = cs::supportOffset(faceOf(p), facingOf(p));
    return offset(p, o[0], o[1], o[2]);
}

// ---- players -------------------------------------------------------------------------------------

std::string uuidOf(Player const& player) { return player.getUuid().asString(); }

Player* playerByUuid(std::string const& uuid) {
    auto level = ll::service::getLevel();
    if (!level || uuid.empty()) return nullptr;
    return level->getPlayer(mce::UUID::fromString(uuid));
}

void status(Player& player, std::string const& text) {
    TextPacket                     pkt{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void statusKey(Player& player, char const* key, char const* fallback, char const* colour) {
    status(player, std::string(colour) + trFor(player, key, fallback));
}

void playSound(GlobalPos const& p, char const* sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{p.x * 8 + 4, p.y * 8 + 4, p.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(game::blockPos(p), DimensionType(p.dim));
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

void dropAt(GlobalPos const& p, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(p.dim);
    if (!level || !region) return;
    level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}, 10);
}

bool inReach(Player& player, GlobalPos const& p, double range) {
    if (static_cast<int>(player.getDimensionId()) != p.dim) return false;
    auto const& pos = player.getPosition();
    double      dx = pos.x - (p.x + 0.5), dy = pos.y - (p.y + 0.5), dz = pos.z - (p.z + 0.5);
    return dx * dx + dy * dy + dz * dz <= range * range;
}

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

// ---- items ---------------------------------------------------------------------------------------

bool isTableCloth(std::string_view id) { return id.starts_with("levitate:") && id.ends_with("_table_cloth"); }

bool isLinkedItem(std::string_view id) {
    return id == kStockLink || id == kStockTicker || id == kFactoryGauge || id == kRedstoneRequester;
}

std::string itemKey(ItemStack const& stack) {
    if (items::isEmpty(stack)) return {};
    std::string key = items::idOf(stack) + "|" + std::to_string(stack.getAuxValue());
    if (auto const* tag = items::userData(stack)) {
        auto snbt = tag->toSnbt(SnbtFormat::Minimize, 0);
        if (snbt != "{}") key += "|" + snbt;
    }
    return key;
}

std::vector<BigStack> Stock::byCount() const {
    std::vector<BigStack> out;
    for (auto const& e : summary.byCount()) {
        auto it = stacks.find(e.key);
        if (it == stacks.end()) continue;
        out.push_back({it->second, e.count});
    }
    return out;
}

std::string networkOfItem(ItemStack const& stack) {
    auto j = items::getData(stack, kFreqData);
    return j.is_object() ? j.value("freq", std::string{}) : std::string{};
}

void tuneItem(ItemStack& stack, std::string const& network) {
    items::setData(stack, kFreqData, network.empty() ? Json(nullptr) : Json{{"freq", network}});
    updateLore(stack);
}

void clearTuning(ItemStack& stack) {
    items::setData(stack, kFreqData, nullptr);
    items::setData(stack, kRequestData, nullptr);
    updateLore(stack);
}

void updateLore(ItemStack& stack) {
    std::vector<std::string> lines;
    auto                     id    = items::idOf(stack);
    bool                     tuned = !networkOfItem(stack).empty();
    auto                     req   = items::getData(stack, kRequestData);
    if (tuned) lines.push_back("§6" + tr("create.logistically_linked.tooltip", "Frequency configured"));
    if (req.is_object()) {
        for (auto const& e : orderFromJson(req.value("order", Json::array())))
            lines.push_back("§7" + items::displayName(e.stack) + " x" + std::to_string(e.count));
    }
    if (tuned || req.is_object())
        lines.push_back((req.is_object() ? "§8" : "§7") + tr("create.logistically_linked.tooltip_clear", "Place in crafting grid to reset"));
    (void)id;
    if (lines.empty()) stack.clearCustomLore();
    else stack.setCustomLore(lines);
}

std::string newNetworkId() {
    std::uniform_int_distribution<int> hex(0, 15);
    static char const*                 digits = "0123456789abcdef";
    std::string                        s;
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) s += '-';
        int v = hex(rng());
        if (i == 12) v = 4;                 // version 4
        if (i == 16) v = 8 | (v & 3);       // variant
        s += digits[v];
    }
    return s;
}

ItemStack stackFromJson(Json const& j) { return items::fromJson(j); }
Json      stackToJson(ItemStack const& s) { return items::toJson(s); }

Json orderToJson(std::vector<BigStack> const& order) {
    Json j = Json::array();
    for (auto const& e : order)
        if (!items::isEmpty(e.stack) && e.count > 0) j.push_back({{"item", items::toJson(items::withCount(e.stack, 1))}, {"count", e.count}});
    return j;
}

std::vector<BigStack> orderFromJson(Json const& j) {
    std::vector<BigStack> out;
    if (!j.is_array()) return out;
    for (auto const& e : j) {
        if (!e.is_object()) continue;
        auto s = items::fromJson(e.value("item", Json()));
        int  n = e.value("count", 0);
        if (items::isEmpty(s) || n <= 0) continue;
        out.push_back({items::withCount(s, 1), n});
    }
    return out;
}

// ---- packager seam -------------------------------------------------------------------------------

void registerPackagerProvider(PackagerProvider p) {
    provider() = std::move(p);
    invalidateSummaries();
    log().info("stock: packager provider registered");
}
std::optional<PackagerProvider> currentPackagerProvider() { return provider(); }
void                            setPackagerProvider(std::optional<PackagerProvider> p) {
    provider() = std::move(p);
    invalidateSummaries();
    lastPackagerStock().clear();
}

namespace {
bool isPackagerAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region || !provider() || !provider()->isPackager || !blockAt(p)) return false;
    return provider()->isPackager(*region, game::blockPos(p));
}
} // namespace

std::optional<GlobalPos> linkOfPackager(GlobalPos const& packager) {
    for (int d = 0; d < 6; ++d) {
        auto q = offset(packager, kDx[d], kDy[d], kDz[d]);
        if (idAt(q) != kStockLink) continue;
        if (supportOf(q) == packager) return q;
    }
    return std::nullopt;
}

std::optional<GlobalPos> packagerOfLink(GlobalPos const& link) {
    if (idAt(link) != kStockLink) return std::nullopt;
    auto const* d = dataOf(link);
    if (d && d->value("power", 0) >= 15) return std::nullopt;
    auto support = supportOf(link);
    if (!isPackagerAt(support)) return std::nullopt;
    return support;
}

std::optional<Stock> packagerStock(GlobalPos const& packager) {
    auto* region = regionOf(packager.dim);
    if (!region || !provider() || !provider()->availableItems) return std::nullopt;
    auto items = provider()->availableItems(*region, game::blockPos(packager));
    if (!items) return std::nullopt;
    Stock s;
    for (auto const& e : *items) {
        auto key = itemKey(e.stack);
        if (key.empty() || e.count == 0) continue;
        s.summary.add(key, e.count);
        s.stacks.try_emplace(key, common::items::withCount(e.stack, 1));
    }
    // Create's submitNewArrivals: what grew since the last look fulfils promises
    auto& last = lastPackagerStock();
    auto  it   = last.find(packager);
    if (it != last.end()) {
        std::vector<cs::Entry> arrivals;
        for (auto const& e : s.summary.entries()) {
            int before = it->second.countOf(e.key);
            if (e.count > before && e.count < kInfinite) arrivals.push_back({e.key, e.count - before});
        }
        if (!arrivals.empty()) {
            if (auto link = linkOfPackager(packager)) {
                auto const* d = dataOf(*link);
                if (auto* net = d ? networkInfo(d->value("freq", std::string{})) : nullptr; net && !net->promises.empty())
                    for (auto const& a : arrivals) net->promises.itemEnteredSystem(a.key, a.count);
            }
            restockerArrivals(packager, arrivals);
        }
    }
    last[packager] = s.summary;
    return s;
}

std::string packagerInventory(GlobalPos const& packager) {
    auto* region = regionOf(packager.dim);
    std::string id;
    if (region && provider() && provider()->inventoryId) id = provider()->inventoryId(*region, game::blockPos(packager));
    // without an identity the packager itself is the inventory (two links on it count once)
    return id.empty() ? "packager:" + packager.key() : id;
}

bool packagerAt(GlobalPos const& p) { return isPackagerAt(p); }

bool packagerTooBusy(GlobalPos const& packager, RequestType type) {
    auto* region = regionOf(packager.dim);
    if (!region || !provider()) return false;
    if (provider()->tooBusy) return provider()->tooBusy(*region, game::blockPos(packager), type);
    if (!provider()->queuedPackages) return false;
    return provider()->queuedPackages(*region, game::blockPos(packager)) >= cs::busyLimit(type);
}

// ---- networks ------------------------------------------------------------------------------------

NetworkInfo* networkInfo(std::string const& id) {
    auto it = networks().find(id);
    return it == networks().end() ? nullptr : &it->second;
}

NetworkInfo& ensureNetwork(std::string const& id) {
    auto [it, fresh] = networks().try_emplace(id);
    if (fresh) {
        it->second.promises.onChanged = [] { markNetworksDirty(); };
        markNetworksDirty();
    }
    return it->second;
}

void linkAdded(std::string const& network, GlobalPos const& pos, std::string const& placedBy) {
    if (network.empty()) return;
    auto& n = ensureNetwork(network);
    n.links.insert(pos);
    if (!placedBy.empty() && n.owner.empty()) n.owner = placedBy;
    markNetworksDirty();
    invalidateSummaries();
}

void linkRemoved(std::string const& network, GlobalPos const& pos) {
    auto* n = networkInfo(network);
    if (!n) return;
    n->links.erase(pos);
    if (n->links.empty()) networks().erase(network);
    markNetworksDirty();
    invalidateSummaries();
}

void setLocked(std::string const& network, bool locked) {
    auto* n = networkInfo(network);
    if (!n) return;
    n->locked = locked;
    markNetworksDirty();
}

void markNetworksDirty() { networksDirty() = true; }

int unloadedLinkCount(std::string const& network) {
    auto* n = networkInfo(network);
    if (!n) return 0;
    int unloaded = 0;
    for (auto const& p : n->links)
        if (!blockAt(p)) ++unloaded;
    return unloaded;
}

bool isLockable(std::string const& network) { return networkInfo(network) != nullptr; }
bool isLocked(std::string const& network) {
    auto* n = networkInfo(network);
    return n && n->locked;
}

bool mayInteract(std::string const& network, Player const& player) {
    auto* n = networkInfo(network);
    return !n || n->owner.empty() || !n->locked || n->owner == uuidOf(player);
}

bool mayAdministrate(std::string const& network, Player const& player) {
    auto* n = networkInfo(network);
    return !n || n->owner.empty() || n->owner == uuidOf(player);
}

void invalidateSummaries() {
    summaries(false).clear();
    summaries(true).clear();
}

namespace {
/// The present links of a network (Create's getAllPresent): loaded, still a stock link of this
/// network, not powered with 15.
std::vector<GlobalPos> presentLinks(std::string const& network) {
    std::vector<GlobalPos> out;
    auto*                  n = networkInfo(network);
    if (!n) return out;
    for (auto const& p : n->links) {
        if (idAt(p) != kStockLink) continue;
        auto const* d = dataOf(p);
        if (!d || d->value("freq", std::string{}) != network || d->value("power", 0) >= 15) continue;
        out.push_back(p);
    }
    return out;
}

Stock buildSummary(std::string const& network) {
    Stock                 out;
    std::set<std::string> seenInventories;
    for (auto const& link : presentLinks(network)) {
        auto packager = packagerOfLink(link);
        if (!packager) continue;
        auto inv = packagerInventory(*packager);
        if (!inv.empty() && !seenInventories.insert(inv).second) continue;
        auto s = packagerStock(*packager);
        if (!s) continue;
        if (!s->summary.empty()) {
            out.summary.contributingLinks++;
            out.summary.add(s->summary);
            for (auto& [k, v] : s->stacks) out.stacks.try_emplace(k, v);
        }
    }
    return out;
}
} // namespace

std::string networkAt(GlobalPos const& pos) {
    auto const* d = dataOf(pos);
    if (!d) return {};
    if (auto f = d->value("freq", std::string{}); !f.empty()) return f;
    if (auto it = d->find("panels"); it != d->end() && it->is_object())
        for (auto const& [k, v] : it->items())
            if (v.is_object() && v.value("active", false)) return v.value("freq", std::string{});
    return {};
}

Stock const& summaryOf(std::string const& network, bool accurate) {
    auto& cache = summaries(accurate);
    auto  it    = cache.find(network);
    int   ttl   = accurate ? 1 : 20;
    if (it != cache.end() && tickCounter() - it->second.tick < static_cast<std::uint64_t>(ttl)) return it->second.stock;
    auto& slot = cache[network];
    slot.tick  = tickCounter();
    slot.stock = buildSummary(network);
    return slot.stock;
}

int stockOfItem(std::string const& network, ItemStack const& stack, std::string const& ignoredInventory) {
    auto key = itemKey(stack);
    int  sum = 0;
    for (auto const& link : presentLinks(network)) {
        auto packager = packagerOfLink(link);
        if (!packager) continue;
        if (!ignoredInventory.empty() && packagerInventory(*packager) == ignoredInventory) continue;
        if (auto s = packagerStock(*packager)) sum = std::min<long long>(kInfinite, static_cast<long long>(sum) + s->summary.countOf(key));
    }
    return sum;
}

bool requestOrder(
    std::string const&           network,
    RequestType                  type,
    std::vector<BigStack> const& order,
    std::string const&           address,
    std::string const&           ignoredInventory,
    Json const&                  crafts
) {
    std::vector<cs::Entry>                     entries;
    std::unordered_map<std::string, ItemStack> stacks;
    for (auto const& e : order) {
        auto key = itemKey(e.stack);
        if (key.empty() || e.count <= 0) continue;
        entries.push_back({key, e.count});
        stacks.try_emplace(key, common::items::withCount(e.stack, 1));
    }
    if (entries.empty() || !provider() || !provider()->send) return false;

    std::vector<cs::LinkView> views;
    std::vector<GlobalPos>    packagers;
    for (auto const& link : presentLinks(network)) {
        auto packager = packagerOfLink(link);
        if (!packager) continue;
        auto inv = packagerInventory(*packager);
        if (!ignoredInventory.empty() && inv == ignoredInventory) continue;
        auto s = packagerStock(*packager);
        if (!s) continue;
        cs::LinkView v;
        v.id        = static_cast<int>(packagers.size());
        v.inventory = inv;
        v.power     = dataOf(link) ? dataOf(link)->value("power", 0) : 0;
        v.available = s->summary;
        views.push_back(std::move(v));
        packagers.push_back(*packager);
    }
    auto reqs = cs::distributeOrder(entries, views, [](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng()); });

    std::map<GlobalPos, std::vector<PackagingRequest>> byPackager;
    int  orderId = std::uniform_int_distribution<int>()(rng());
    Json context{{"stacks", orderToJson(order)}, {"crafts", crafts.is_array() ? crafts : Json::array()}};
    for (auto const& r : reqs) {
        if (r.count <= 0) continue;
        PackagingRequest pr;
        pr.item      = stacks[r.key];
        pr.count     = r.count;
        pr.address   = address;
        pr.orderId   = orderId;
        pr.linkIndex = r.linkIndex;
        pr.finalLink = r.finalLink;
        if (r.context) pr.orderContext = context;
        byPackager[packagers[static_cast<std::size_t>(r.link)]].push_back(std::move(pr));
    }
    for (auto const& [packager, list] : byPackager)
        if (packagerTooBusy(packager, type)) return false;
    for (auto const& [packager, list] : byPackager) {
        auto* region = regionOf(packager.dim);
        if (!region) continue;
        if (auto link = linkOfPackager(packager)) linkEffect(*link);
        provider()->send(*region, game::blockPos(packager), list);
    }
    // what left the inventories no longer counts (Create's deductFromAccurateSummary)
    summaries(true).erase(network);
    int total = 0;
    for (auto const& e : entries) total += e.count;
    log().info("stock: order of {} item(s) in {} stack(s) sent to '{}' through {} packager(s)", total, entries.size(), address, byPackager.size());
    return true;
}

void linkEffect(GlobalPos const& link) {
    playSound(link, "levitate.stock.link", 0.75f, 1.25f);
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(link.dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect("minecraft:villager_happy", Vec3{link.x + 0.5f, link.y + 0.6f, link.z + 0.5f}, dimension.get());
}

// ---- persistence ---------------------------------------------------------------------------------

void loadNetworks() {
    networks().clear();
    lastPackagerStock().clear();
    invalidateSummaries();
    auto j = game::db::load(kDbNetworks);
    if (!j || !j->is_object()) return;
    for (auto const& [id, v] : j->items()) {
        if (!v.is_object()) continue;
        auto& n  = ensureNetwork(id);
        n.owner  = v.value("owner", std::string{});
        n.locked = v.value("locked", false);
        for (auto const& p : v.value("links", Json::array()))
            if (auto gp = posFrom(p)) n.links.insert(*gp);
        n.promises           = cs::PromiseQueue::fromJson(v.value("promises", Json::array()));
        n.promises.onChanged = [] { markNetworksDirty(); };
    }
    networksDirty() = false;
}

void saveNetworks() {
    if (!networksDirty() || !game::db::available()) return;
    Json j = Json::object();
    for (auto const& [id, n] : networks()) {
        Json links = Json::array();
        for (auto const& p : n.links) links.push_back(posJson(p));
        j[id] = {{"owner", n.owner}, {"locked", n.locked}, {"links", links}, {"promises", n.promises.toJson()}};
    }
    game::db::save(kDbNetworks, j);
    networksDirty() = false;
}

void resetNetworks() {
    networks().clear();
    lastPackagerStock().clear();
    invalidateSummaries();
    deferred().clear();
    networksDirty() = false;
}

void tickNetworks() {
    ++tickCounter();
    if (!deferred().empty()) {
        auto run = std::exchange(deferred(), {});
        for (auto& fn : run) fn();
    }
    // Create ticks the network promise queues on the overworld tick
    for (auto& [id, n] : networks()) n.promises.tick();
    if (tickCounter() % 200 == 0) markNetworksDirty(); // promise ages are saved now and then
}

} // namespace lv::feature::stock
