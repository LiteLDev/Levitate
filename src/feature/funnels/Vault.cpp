// Item vaults (Create's ItemVaultBlock / ItemVaultBlockEntity / ConnectivityHandler,
// SPEC §3.6 U12).
//
// Vault blocks along the same horizontal axis join into multi-block vaults (square base up to
// 3x3, up to 3x the width long). Each block holds `vaultCapacity` (20) stacks; the structure
// exposes all of them as one inventory to funnels, chutes, arms, ... (no manual access), and
// comparators next to any of its blocks read the fill level of the whole vault.

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/ComparatorCapacitor.h"

#include "core/common/Transfer.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

struct VaultState {
    std::map<GlobalPos, std::shared_ptr<cf::VaultStructure>> structures; ///< every block -> its structure
    std::set<GlobalPos>                                      dirty;
    std::unordered_map<std::string, std::vector<ItemStack>>  inventories;
    /// comparator overrides: (circuit system, comparator position) -> signal
    std::map<std::pair<void const*, std::tuple<int, int, int>>, int> comparators;
    /// comparators that read a vault until now: their analog input is released once
    std::set<std::pair<void const*, std::tuple<int, int, int>>> released;
};
VaultState& vs() {
    static VaultState s;
    return s;
}

int capacity() { return std::clamp(config().vaultCapacity, 1, 2048); }

bool isVaultAt(BlockSource& region, BlockPos const& p) { return kindAt(region, p) == Kind::Vault; }

int vaultAxis(Block const& b) { return state(b, kAxis, 0) == 2 ? 2 : 0; }

std::vector<ItemStack>& inventory(GlobalPos const& pos) {
    auto [it, fresh] = vs().inventories.try_emplace(pos.key());
    auto& inv        = it->second;
    if (fresh) {
        if (auto const* d = docOf(pos); d && d->contains("items") && (*d)["items"].is_array()) {
            for (auto const& e : (*d)["items"]) inv.push_back(e.is_null() ? items::empty() : items::fromJson(e));
        }
        if (static_cast<int>(inv.size()) < capacity()) inv.resize(static_cast<std::size_t>(capacity()), items::empty());
    }
    return inv;
}

void saveInventory(GlobalPos const& pos) {
    auto& inv = inventory(pos);
    Json  arr = Json::array();
    bool  any = false;
    for (auto const& s : inv) {
        if (items::isEmpty(s)) arr.push_back(nullptr);
        else {
            arr.push_back(items::toJson(s));
            any = true;
        }
    }
    auto& d = editDoc(pos);
    if (any) d["items"] = std::move(arr);
    else d.erase("items");
}

/// Forms the structures of the vault component around `start` and updates the blocks' looks.
void formAround(BlockSource& region, int dim, BlockPos const& start) {
    if (!isVaultAt(region, start)) return;
    std::vector<cf::VaultCell> cells;
    std::set<cf::VaultCell>    seen;
    std::deque<BlockPos>       queue{start};
    seen.insert({start.x, start.y, start.z});
    while (!queue.empty() && cells.size() < 4096) {
        auto p = queue.front();
        queue.pop_front();
        cells.push_back({p.x, p.y, p.z});
        for (int f : cf::kAllFaces) {
            auto n = offset(p, f);
            if (seen.contains({n.x, n.y, n.z}) || !isVaultAt(region, n)) continue;
            seen.insert({n.x, n.y, n.z});
            queue.push_back(n);
        }
    }
    for (auto const& c : cells) vs().structures.erase({dim, c.x, c.y, c.z});
    auto formed = cf::formVaults(cells, [&](cf::VaultCell const& c) -> std::optional<int> {
        auto const* b = loadedBlock(region, BlockPos{c.x, c.y, c.z});
        if (!b || kindOf(b->getTypeName()) != Kind::Vault) return std::nullopt;
        return vaultAxis(*b);
    });
    for (auto const& s : formed) {
        auto shared = std::make_shared<cf::VaultStructure>(s);
        for (int i = 0; i < s.size(); ++i) {
            auto c = s.cell(i);
            vs().structures[{dim, c.x, c.y, c.z}] = shared;
            auto look = cf::vaultLook(s, c);
            setStates(region, BlockPos{c.x, c.y, c.z}, {{kLen, look.len}, {kU, look.u}, {kV, look.v}, {kLarge, look.large ? 1 : 0}});
        }
        if (s.size() > 1) log().info("funnels: item vault at {} {} {} formed ({}x{}x{})", s.origin.x, s.origin.y, s.origin.z, s.width, s.width, s.length);
    }
}

std::shared_ptr<cf::VaultStructure> structureAt(BlockSource& region, GlobalPos const& pos) {
    if (auto it = vs().structures.find(pos); it != vs().structures.end() && !vs().dirty.contains(pos)) return it->second;
    formAround(region, pos.dim, toBlockPos(pos));
    auto it = vs().structures.find(pos);
    if (it == vs().structures.end()) return nullptr;
    // formed now: no need to form it again before the next tick
    for (int i = 0; i < it->second->size(); ++i) {
        auto c = it->second->cell(i);
        vs().dirty.erase({pos.dim, c.x, c.y, c.z});
    }
    return it->second;
}

/// Slot `i` of the structure -> (block, slot in block).
std::pair<GlobalPos, int> slotRef(int dim, cf::VaultStructure const& s, int i) {
    auto c = s.cell(i / capacity());
    return {GlobalPos{dim, c.x, c.y, c.z}, i % capacity()};
}

common::items::Handler capability() {
    common::items::Handler h;
    h.slots = [](common::Access const& at) {
        auto s = structureAt(at.region, game::globalPos(at.region, at.pos));
        return s ? s->size() * capacity() : 0;
    };
    h.getSlot = [](common::Access const& at, int slot) -> ItemStack {
        int  dim = static_cast<int>(at.region.getDimensionId());
        auto s   = structureAt(at.region, toGlobal(dim, at.pos));
        if (!s || slot < 0 || slot >= s->size() * capacity()) return items::empty();
        auto [block, i] = slotRef(dim, *s, slot);
        return inventory(block)[static_cast<std::size_t>(i)];
    };
    h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        int  dim = static_cast<int>(at.region.getDimensionId());
        auto s   = structureAt(at.region, toGlobal(dim, at.pos));
        if (!s || items::isEmpty(stack)) return stack;
        int total = s->size() * capacity();
        int max   = std::max(1, static_cast<int>(stack.getMaxStackSize()));
        std::vector<core::common::SlotView> views(static_cast<std::size_t>(total));
        for (int i = 0; i < total; ++i) {
            auto [block, si] = slotRef(dim, *s, i);
            auto const& cur  = inventory(block)[static_cast<std::size_t>(si)];
            auto&       v    = views[static_cast<std::size_t>(i)];
            v.empty          = items::isEmpty(cur);
            v.stackable      = !v.empty && items::sameItemSameData(cur, stack);
            v.count          = items::countOf(cur);
            v.maxCount       = max;
        }
        auto plan = core::common::planInsert(views, items::countOf(stack));
        if (!simulate) {
            std::set<GlobalPos> changed;
            for (auto const& [i, add] : plan) {
                auto [block, si] = slotRef(dim, *s, i);
                auto& cur        = inventory(block)[static_cast<std::size_t>(si)];
                cur              = items::withCount(stack, items::countOf(cur) + add);
                changed.insert(block);
            }
            for (auto const& b : changed) saveInventory(b);
        }
        return items::withCount(stack, items::countOf(stack) - core::common::plannedTotal(plan));
    };
    h.extract = [](common::Access const& at, common::items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        int  dim = static_cast<int>(at.region.getDimensionId());
        auto s   = structureAt(at.region, toGlobal(dim, at.pos));
        if (!s || amount <= 0) return items::empty();
        ItemStack           kind = items::empty();
        int                 got  = 0;
        std::set<GlobalPos> changed;
        for (int i = 0; i < s->size() * capacity() && got < amount; ++i) {
            auto [block, si] = slotRef(dim, *s, i);
            auto& cur        = inventory(block)[static_cast<std::size_t>(si)];
            if (items::isEmpty(cur)) continue;
            if (items::isEmpty(kind)) {
                if (!filter(cur)) continue;
                kind = cur;
            } else if (!items::sameItemSameData(cur, kind)) {
                continue;
            }
            int take = std::min(amount - got, items::countOf(cur));
            got += take;
            if (!simulate) {
                cur = items::withCount(cur, items::countOf(cur) - take);
                changed.insert(block);
            }
        }
        for (auto const& b : changed) saveInventory(b);
        return items::withCount(kind, got);
    };
    return h;
}

/// The comparator's rear (input) side: Bedrock stores the direction it was placed facing.
std::optional<int> comparatorRear(Block const& b) {
    std::string id = b.getTypeName();
    if (id != "minecraft:unpowered_comparator" && id != "minecraft:powered_comparator") return std::nullopt;
    std::string dir;
    auto const& nbt = b.mSerializationId.get();
    if (nbt.contains("states", Tag::Type::Compound)) {
        auto const& st = nbt["states"].get<CompoundTag>();
        if (st.contains("minecraft:cardinal_direction", Tag::Type::String))
            dir = std::string(st["minecraft:cardinal_direction"].get<StringTag>());
    }
    if (dir.empty()) {
        static char const* names[]{"south", "west", "north", "east"};
        int d = game::intState(b, "direction").value_or(0);
        dir   = names[std::clamp(d, 0, 3)];
    }
    // Bedrock's cardinal direction of a comparator points from its output to its input (rear)
    if (dir == "north") return cf::kNorth;
    if (dir == "south") return cf::kSouth;
    if (dir == "west") return cf::kWest;
    if (dir == "east") return cf::kEast;
    return std::nullopt;
}

void const* circuitOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return dimension->mCircuitSystem.get();
}

} // namespace

std::optional<cf::VaultStructure> vaultStructure(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || !isVaultAt(*region, toBlockPos(pos))) return std::nullopt;
    auto s = structureAt(*region, pos);
    if (!s) return std::nullopt;
    return *s;
}

std::optional<int> vaultSignal(GlobalPos const& pos) {
    auto s = vaultStructure(pos);
    if (!s) return std::nullopt;
    std::vector<std::pair<int, int>> slots;
    for (int i = 0; i < s->size(); ++i) {
        auto c = s->cell(i);
        for (auto const& st : inventory({pos.dim, c.x, c.y, c.z}))
            if (!items::isEmpty(st)) slots.emplace_back(items::countOf(st), std::max(1, static_cast<int>(st.getMaxStackSize())));
    }
    return cf::comparatorSignal(slots, s->size() * capacity());
}

void vaultChanged(GlobalPos const& pos) {
    vs().dirty.insert(pos);
    vs().inventories.erase(pos.key()); // re-read from BlockData (erased with a removed block)
    for (int f : cf::kAllFaces) vs().dirty.insert(offset(pos, f));
}

void forgetVaults() { vs() = VaultState{}; }

void tickVaults(std::uint64_t tick) {
    if (!vs().dirty.empty()) {
        auto dirty = std::move(vs().dirty);
        vs().dirty.clear();
        for (auto const& p : dirty) {
            // the old structure of a changed block is gone: its blocks form again
            if (auto it = vs().structures.find(p); it != vs().structures.end()) {
                auto old = it->second;
                for (int i = 0; i < old->size(); ++i) {
                    auto c = old->cell(i);
                    vs().structures.erase({p.dim, c.x, c.y, c.z});
                    if (GlobalPos cp{p.dim, c.x, c.y, c.z}; cp != p) vs().dirty.insert(cp);
                }
            }
        }
        auto again = std::move(vs().dirty);
        vs().dirty.clear();
        dirty.insert(again.begin(), again.end());
        for (auto const& p : dirty) {
            if (vs().structures.contains(p)) continue;
            auto* region = regionOf(p.dim);
            if (!region) continue;
            formAround(*region, p.dim, toBlockPos(p));
        }
    }
    // comparators reading vaults (Bedrock asks the block behind a comparator for an analog
    // signal only for containers: the signal is given to the comparator's circuit component)
    if (tick % 2 != 0) return;
    auto previous = std::move(vs().comparators);
    vs().comparators.clear();
    std::set<cf::VaultStructure const*> done;
    std::map<cf::VaultStructure const*, int> signals;
    for (auto const& [pos, s] : vs().structures) {
        auto* region = regionOf(pos.dim);
        if (!region || !loadedBlock(*region, toBlockPos(pos))) continue;
        for (int f : cf::kHorizontals) {
            auto        cp = offset(toBlockPos(pos), f);
            auto const* cb = loadedBlock(*region, cp);
            if (!cb) continue;
            auto rear = comparatorRear(*cb);
            if (!rear || offset(cp, *rear) != toBlockPos(pos)) continue;
            auto sit = signals.find(s.get());
            if (sit == signals.end()) sit = signals.emplace(s.get(), vaultSignal(pos).value_or(0)).first;
            if (auto const* circuit = circuitOf(pos.dim)) vs().comparators[{circuit, {cp.x, cp.y, cp.z}}] = sit->second;
        }
    }
    for (auto const& [key, value] : previous)
        if (!vs().comparators.contains(key)) vs().released.insert(key);
}

bool wrenchVault(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, int face) {
    if (cf::isHorizontal(face)) return false;
    int next = vaultAxis(block) == 0 ? 2 : 0;
    setStates(region, toBlockPos(pos), {{kAxis, next}, {kLarge, 0}});
    playSound(region, toBlockPos(pos), "random.click", .5f, 1.2f);
    log().info("funnels: item vault at {} turned by {}", pos.key(), player.getRealName());
    return true;
}

void placeVault(Player* player, BlockSource& region, GlobalPos const& pos, int clickedFace, bool sneaking) {
    auto bp   = toBlockPos(pos);
    int  axis = -1;
    if (!sneaking && clickedFace >= 0) {
        auto const* on = loadedBlock(region, offset(bp, cf::oppositeFace(clickedFace)));
        if (on && kindOf(on->getTypeName()) == Kind::Vault) axis = vaultAxis(*on);
    }
    if (axis < 0 && player) {
        int f = game::playerFacing(*player); // 0 S, 1 W, 2 N, 3 E
        axis  = (f == 0 || f == 2) ? 2 : 0;
    }
    if (axis >= 0) setStates(region, bp, {{kAxis, axis}});
    vaultChanged(pos);
}

std::optional<int> comparatorOverride(void const* circuit, BlockPos const& pos) {
    auto it = vs().comparators.find({circuit, {pos.x, pos.y, pos.z}});
    if (it == vs().comparators.end()) return std::nullopt;
    return it->second;
}

/// The comparator no longer reads a vault: forget the analog input we gave it (once).
bool comparatorReleased(void const* circuit, BlockPos const& pos) { return vs().released.erase({circuit, {pos.x, pos.y, pos.z}}) > 0; }

void installVaults() { common::items::registerBlock(kItemVault, capability()); }

} // namespace lv::feature::funnels::detail

namespace lv::feature::funnels {

LL_TYPE_INSTANCE_HOOK(
    VaultComparatorHook,
    ll::memory::HookPriority::Normal,
    ComparatorCapacitor,
    &ComparatorCapacitor::$evaluate,
    bool,
    CircuitSystem&  system,
    BlockPos const& pos
) {
    if (auto v = detail::comparatorOverride(&system, pos)) {
        mRearAnalogStrength = *v;
        mHasAnalogBeenSet   = true;
    } else if (detail::comparatorReleased(&system, pos)) {
        mRearAnalogStrength = 0;
        mHasAnalogBeenSet   = false;
    }
    return origin(system, pos);
}

class VaultComparatorModule {
public:
    void install() { VaultComparatorHook::hook(); }
    void uninstall() { VaultComparatorHook::unhook(); }
};

} // namespace lv::feature::funnels

LV_REGISTER_MODULE(lv::feature::funnels::VaultComparatorModule);
