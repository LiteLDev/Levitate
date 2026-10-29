#include "core/stock/Logistics.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lv::core::stock {

// ---- Summary -------------------------------------------------------------------------------------

namespace {
int saturatingAdd(int a, int b) {
    long long s = static_cast<long long>(a) + b;
    if (s >= kInfinite) return kInfinite;
    if (s <= -kInfinite) return -kInfinite;
    return static_cast<int>(s);
}
} // namespace

void Summary::add(std::string const& key, int count) {
    if (count == 0 || key.empty()) return;
    if (mTotal < kInfinite) mTotal = saturatingAdd(mTotal, count);
    for (auto& e : mEntries) {
        if (e.key != key) continue;
        if (e.count < kInfinite) e.count = saturatingAdd(e.count, count);
        return;
    }
    mEntries.push_back({key, std::min(count, kInfinite)});
}

void Summary::add(Summary const& other) {
    for (auto const& e : other.mEntries) add(e.key, e.count);
    contributingLinks += other.contributingLinks;
}

int Summary::countOf(std::string const& key) const {
    for (auto const& e : mEntries)
        if (e.key == key) return e.count;
    return 0;
}

std::vector<Entry> Summary::byCount() const {
    auto out = mEntries;
    std::stable_sort(out.begin(), out.end(), [](Entry const& a, Entry const& b) { return a.count > b.count; });
    return out;
}

bool Summary::erase(std::string const& key) {
    auto it = std::find_if(mEntries.begin(), mEntries.end(), [&](Entry const& e) { return e.key == key; });
    if (it == mEntries.end()) return false;
    mTotal -= it->count;
    mEntries.erase(it);
    return true;
}

nlohmann::json Summary::toJson() const {
    auto j = nlohmann::json::array();
    for (auto const& e : mEntries) j.push_back({{"key", e.key}, {"count", e.count}});
    return j;
}

Summary Summary::fromJson(nlohmann::json const& j) {
    Summary s;
    if (!j.is_array()) return s;
    for (auto const& e : j)
        if (e.is_object()) s.add(e.value("key", std::string{}), e.value("count", 0));
    return s;
}

// ---- PromiseQueue --------------------------------------------------------------------------------

void PromiseQueue::add(std::string const& key, int count, int ticks) {
    if (key.empty() || count <= 0) return;
    mPromises.push_back({key, count, ticks});
    changed();
}

int PromiseQueue::totalPromisedAndRemoveExpired(std::string const& key, int expiryTicks) {
    int  promised = 0;
    bool removed  = false;
    for (auto it = mPromises.begin(); it != mPromises.end();) {
        if (it->key != key) {
            ++it;
            continue;
        }
        if (expiryTicks != -1 && it->ticks >= expiryTicks) {
            it      = mPromises.erase(it);
            removed = true;
            continue;
        }
        promised += it->count;
        ++it;
    }
    if (removed) changed();
    return promised;
}

void PromiseQueue::forceClear(std::string const& key) {
    auto n = std::erase_if(mPromises, [&](Promise const& p) { return p.key == key; });
    if (n) changed();
}

void PromiseQueue::itemEnteredSystem(std::string const& key, int amount) {
    if (amount <= 0) return;
    bool removed = false;
    // Create keeps promises per item in insertion order, so the oldest is fulfilled first
    for (auto it = mPromises.begin(); it != mPromises.end() && amount > 0;) {
        if (it->key != key) {
            ++it;
            continue;
        }
        int take = std::min(amount, it->count);
        amount -= take;
        it->count -= take;
        if (it->count <= 0) {
            it      = mPromises.erase(it);
            removed = true;
            continue;
        }
        ++it;
    }
    if (removed) changed();
}

void PromiseQueue::tick() {
    for (auto& p : mPromises) ++p.ticks;
}

nlohmann::json PromiseQueue::toJson() const {
    auto j = nlohmann::json::array();
    for (auto const& p : mPromises) j.push_back({{"key", p.key}, {"count", p.count}, {"ticks", p.ticks}});
    return j;
}

PromiseQueue PromiseQueue::fromJson(nlohmann::json const& j) {
    PromiseQueue q;
    if (!j.is_array()) return q;
    for (auto const& e : j) {
        if (!e.is_object()) continue;
        auto key   = e.value("key", std::string{});
        int  count = e.value("count", 0);
        if (key.empty() || count <= 0) continue;
        q.mPromises.push_back({key, count, e.value("ticks", 0)});
    }
    return q;
}

// ---- order distribution --------------------------------------------------------------------------

std::vector<LinkRequest>
distributeOrder(std::vector<Entry> const& order, std::vector<LinkView> const& links, std::function<int(int)> const& pick) {
    std::vector<Entry> stacks;
    for (auto const& e : order)
        if (!e.key.empty() && e.count > 0) stacks.push_back(e);

    // links sorted by redstone power (stable), then one link per shared inventory
    std::vector<LinkView const*> sorted;
    for (auto const& l : links) sorted.push_back(&l);
    std::stable_sort(sorted.begin(), sorted.end(), [](LinkView const* a, LinkView const* b) { return a->power < b->power; });
    std::vector<LinkView const*>                      available;
    std::map<std::string, std::vector<LinkView const*>> byInventory;
    std::vector<std::string>                          inventoryOrder;
    for (auto const* l : sorted) {
        if (l->inventory.empty()) {
            available.push_back(l);
            continue;
        }
        auto [it, fresh] = byInventory.try_emplace(l->inventory);
        if (fresh) inventoryOrder.push_back(l->inventory);
        it->second.push_back(l);
    }
    for (auto const& inv : inventoryOrder) {
        auto const& group = byInventory[inv];
        int         idx   = pick ? std::clamp(pick(static_cast<int>(group.size())), 0, static_cast<int>(group.size()) - 1) : 0;
        available.push_back(group[static_cast<std::size_t>(idx)]);
    }

    std::vector<LinkRequest> out;
    std::vector<int>         usedLinks;
    bool                     context = true;
    bool                     satisfiedLast = false;
    for (std::size_t i = 0; i < stacks.size(); ++i) {
        auto const& entry     = stacks[i];
        int         remaining = entry.count;
        for (auto const* link : available) {
            auto used      = std::find(usedLinks.begin(), usedLinks.end(), link->id);
            int  usedIndex = used == usedLinks.end() ? -1 : static_cast<int>(used - usedLinks.begin());
            int  linkIndex = usedIndex == -1 ? static_cast<int>(usedLinks.size()) : usedIndex;
            int  have      = link->available.countOf(entry.key);
            if (have <= 0) continue;
            int take = std::min(remaining, have);
            out.push_back({link->id, entry.key, take, linkIndex, false, context && take > 0});
            if (take > 0 && usedIndex == -1) {
                context = false;
                usedLinks.push_back(link->id);
            }
            remaining -= take;
            if (remaining > 0) continue;
            if (i + 1 == stacks.size()) satisfiedLast = true;
            break;
        }
    }
    if (satisfiedLast && !usedLinks.empty()) {
        int last = static_cast<int>(usedLinks.size()) - 1;
        for (auto& r : out)
            if (r.linkIndex == last) r.finalLink = true;
    }
    return out;
}

int busyLimit(RequestType type) {
    switch (type) {
    case RequestType::Player: return 50;
    case RequestType::Redstone: return 20;
    case RequestType::Restock: return 10;
    }
    return 10;
}

// ---- factory gauge -------------------------------------------------------------------------------

int demandOf(int amount, bool upTo, int maxStackSize) {
    long long d = static_cast<long long>(amount) * (upTo ? 1 : std::max(1, maxStackSize));
    return static_cast<int>(std::min<long long>(d, kInfinite));
}

int restockAmount(int demand, int promised, int inStorage, int maxStackSize, int availableOnNetwork) {
    long long want = static_cast<long long>(demand) - promised - inStorage;
    long long cap  = static_cast<long long>(std::max(1, maxStackSize)) * 9;
    want           = std::clamp<long long>(want, 0, cap);
    return static_cast<int>(std::min<long long>(want, std::max(0, availableOnNetwork)));
}

int promiseExpiryTicks(int interval) {
    if (interval == -1) return -1;
    if (interval == 0) return 20 * 30;
    return interval * 20 * 60;
}

std::pair<int, int> facingVector(int facing2d) {
    switch (facing2d & 3) {
    case 0: return {0, 1};  // south
    case 1: return {-1, 0}; // west
    case 2: return {0, -1}; // north
    default: return {1, 0}; // east
    }
}

std::array<int, 3> supportOffset(AttachFace face, int facing2d) {
    switch (face) {
    case AttachFace::Floor: return {0, -1, 0};
    case AttachFace::Ceiling: return {0, 1, 0};
    case AttachFace::Wall: {
        auto [fx, fz] = facingVector(facing2d);
        return {-fx, 0, -fz};
    }
    }
    return {0, -1, 0};
}

namespace {
struct Plane {
    Vec3d base; ///< centre of the panel plane
    Vec3d u;    ///< right
    Vec3d v;    ///< up
};

Plane planeOf(AttachFace face, int facing2d) {
    auto [fx, fz] = facingVector(facing2d);
    double const d = 1.0 / 16.0;
    switch (face) {
    case AttachFace::Floor: return {{0.5, d, 0.5}, {double(-fz), 0, double(fx)}, {double(fx), 0, double(fz)}};
    case AttachFace::Ceiling: return {{0.5, 1 - d, 0.5}, {double(fz), 0, double(-fx)}, {double(fx), 0, double(fz)}};
    case AttachFace::Wall:
        return {{0.5 - fx * (0.5 - d), 0.5, 0.5 - fz * (0.5 - d)}, {double(fz), 0, double(-fx)}, {0, 1, 0}};
    }
    return {};
}
} // namespace

Vec3d panelCentre(AttachFace face, int facing2d, PanelSlot slot) {
    auto   p     = planeOf(face, facing2d);
    bool   left  = slot == PanelSlot::TopLeft || slot == PanelSlot::BottomLeft;
    bool   top   = slot == PanelSlot::TopLeft || slot == PanelSlot::TopRight;
    double du    = left ? -0.25 : 0.25;
    double dv    = top ? 0.25 : -0.25;
    return {p.base.x + p.u.x * du + p.v.x * dv, p.base.y + p.u.y * du + p.v.y * dv, p.base.z + p.u.z * du + p.v.z * dv};
}

PanelSlot targetedSlot(AttachFace face, int facing2d, Vec3d const& local) {
    PanelSlot best     = PanelSlot::BottomLeft;
    double    bestDist = 1e9;
    for (int s = 0; s < 4; ++s) {
        auto   c = panelCentre(face, facing2d, static_cast<PanelSlot>(s));
        double d = (c.x - local.x) * (c.x - local.x) + (c.y - local.y) * (c.y - local.y) + (c.z - local.z) * (c.z - local.z);
        if (d < bestDist) {
            bestDist = d;
            best     = static_cast<PanelSlot>(s);
        }
    }
    return best;
}

std::optional<std::string>
connectionIssue(PanelRef const& target, PanelRef const& input, bool alreadyConnected, int targetInputs) {
    if (alreadyConnected) return "create.factory_panel.already_connected";
    if (targetInputs >= kMaxPanelInputs) return "create.factory_panel.cannot_add_more_inputs";
    if (target.face != input.face || target.facing2d != input.facing2d) return "create.factory_panel.same_orientation";
    int  dx = input.x - target.x, dy = input.y - target.y, dz = input.z - target.z;
    auto n  = supportOffset(target.face, target.facing2d);
    if (n[0] * dx + n[1] * dy + n[2] * dz != 0) return "create.factory_panel.same_surface";
    if (dx * dx + dy * dy + dz * dz >= 16 * 16) return "create.factory_panel.too_far_apart";
    if (input.restocker) return "create.factory_panel.input_in_restock_mode";
    if (!target.hasItem || !input.hasItem) return "create.factory_panel.no_item";
    return std::nullopt;
}

} // namespace lv::core::stock
