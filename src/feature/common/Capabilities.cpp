#include "feature/common/Capabilities.h"

#include <unordered_map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/Container.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/ComposterBlock.h"
#include "mc/world/level/block/actor/DropperBlockActor.h"

#include "core/common/Transfer.h"
#include "feature/common/Items.h"
#include "game/Blocks.h"

namespace lv::feature::common {

namespace {
template <class Handler, class Provider>
struct Registry {
    std::unordered_map<std::string, Handler> byId;
    std::vector<Provider>                    providers;
    std::function<std::optional<Handler>(Access const&)> fallback; ///< after every provider

    std::optional<Handler> find(Access const& at) {
        if (!at.region.hasChunksAt(at.pos, 0, false)) return std::nullopt;
        auto const& block = at.region.getBlock(at.pos);
        if (auto it = byId.find(block.getTypeName()); it != byId.end()) return it->second;
        for (auto const& p : providers) {
            if (auto h = p(at, block)) return h;
        }
        return fallback ? fallback(at) : std::nullopt;
    }
};
} // namespace

// ---- items -------------------------------------------------------------------------------------
namespace items {

namespace {
Registry<Handler, Provider>& registry() {
    static Registry<Handler, Provider> r;
    return r;
}

int opposite(int face) { return face < 0 ? face : (face ^ 1); }

Container* containerAt(Access const& at) {
    return DropperBlockActor::getContainerAt(at.region, Vec3{at.pos.x + 0.5f, at.pos.y + 0.5f, at.pos.z + 0.5f});
}

bool furnaceLike(std::string const& id) {
    return id == "minecraft:furnace" || id == "minecraft:lit_furnace" || id == "minecraft:blast_furnace"
        || id == "minecraft:lit_blast_furnace" || id == "minecraft:smoker" || id == "minecraft:lit_smoker";
}

/// Java's WorldlyContainer sides for furnaces (as hoppers use them): top -> input (0), sides ->
/// fuel (1), bottom -> result (2) for extraction. Other containers: every slot from every side,
/// further restricted by the container's own push/pull rules (Bedrock passes the direction of
/// travel, i.e. the opposite of the accessed side).
bool canInsertAt(Access const& at, std::string const& id, Container& c, int slot, ItemStack const& item) {
    if (furnaceLike(id)) {
        if (at.face == 1 || at.face < 0) return slot == 0;
        if (at.face >= 2) return slot == 1 && c.canPushInItem(1, opposite(at.face), item);
        return false;
    }
    return at.face < 0 || c.canPushInItem(slot, opposite(at.face), item);
}

bool canExtractAt(Access const& at, std::string const& id, Container& c, int slot, ItemStack const& item) {
    if (furnaceLike(id)) return (at.face == 0 || at.face < 0) && slot == 2;
    return at.face < 0 || c.canPullOutItem(slot, opposite(at.face), item);
}

Handler containerHandler() {
    Handler h;
    h.slots = [](Access const& at) {
        auto* c = containerAt(at);
        return c ? c->getContainerSize() : 0;
    };
    h.getSlot = [](Access const& at, int slot) -> ItemStack {
        auto* c = containerAt(at);
        if (!c || slot < 0 || slot >= c->getContainerSize()) return empty();
        return c->getItem(slot);
    };
    h.insert = [](Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto* c = containerAt(at);
        if (!c || isEmpty(stack)) return stack;
        auto id   = game::blockIdAt(at.region, at.pos);
        int  size = c->getContainerSize();
        std::vector<core::common::SlotView> views(static_cast<std::size_t>(size));
        int itemMax = std::min<int>(stack.getMaxStackSize(), c->getMaxStackSize());
        for (int i = 0; i < size; ++i) {
            auto& v = views[static_cast<std::size_t>(i)];
            if (!canInsertAt(at, id, *c, i, stack)) {
                v = {false, false, 0, 0};
                continue;
            }
            auto const& cur = c->getItem(i);
            v.empty         = isEmpty(cur);
            v.stackable     = !v.empty && sameItemSameData(cur, stack);
            v.count         = countOf(cur);
            v.maxCount      = itemMax;
        }
        auto plan = core::common::planInsert(views, countOf(stack));
        if (!simulate) {
            for (auto const& [slot, add] : plan) {
                auto const& cur = c->getItem(slot);
                c->setItem(slot, withCount(stack, countOf(cur) + add));
            }
            if (!plan.empty()) c->setContainerChanged(plan.front().first);
        }
        return withCount(stack, countOf(stack) - core::common::plannedTotal(plan));
    };
    h.extract = [](Access const& at, ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto* c = containerAt(at);
        if (!c || amount <= 0) return empty();
        auto      id   = game::blockIdAt(at.region, at.pos);
        int       size = c->getContainerSize();
        ItemStack kind = empty();
        int       got  = 0;
        for (int i = 0; i < size && got < amount; ++i) {
            ItemStack const& cur = c->getItem(i);
            if (isEmpty(cur)) continue;
            if (isEmpty(kind)) {
                if (!filter(cur) || !canExtractAt(at, id, *c, i, cur)) continue;
                kind = cur;
            } else if (!sameItemSameData(cur, kind) || !canExtractAt(at, id, *c, i, cur)) {
                continue;
            }
            int take = std::min(amount - got, countOf(cur));
            got += take;
            if (!simulate) {
                c->setItem(i, withCount(cur, countOf(cur) - take));
                c->setContainerChanged(i);
            }
        }
        return withCount(kind, got);
    };
    return h;
}

/// Composter: compostables go in from the top while it is not full (one item per insertion
/// like a hopper, with the item's compost chance); bone meal comes out at the bottom when ready.
Handler composterHandler() {
    Handler h;
    h.slots   = [](Access const&) { return 1; };
    h.getSlot = [](Access const& at, int) -> ItemStack {
        auto level = game::intState(at.region.getBlock(at.pos), "composter_fill_level").value_or(0);
        if (level < 8) return empty();
        return make("minecraft:bone_meal").value_or(empty());
    };
    h.insert = [](Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        if (isEmpty(stack) || (at.face != 1 && at.face >= 0)) return stack;
        auto const& block = at.region.getBlock(at.pos);
        auto        level = game::intState(block, "composter_fill_level").value_or(0);
        if (level >= 7 || ComposterBlock::getFillChance(stack) <= 0) return stack;
        if (!simulate) ComposterBlock::addItems(stack, 1, at.region, block, at.pos);
        return withCount(stack, countOf(stack) - 1);
    };
    h.extract = [](Access const& at, ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        if (amount <= 0 || (at.face != 0 && at.face >= 0)) return empty();
        auto const& block = at.region.getBlock(at.pos);
        if (game::intState(block, "composter_fill_level").value_or(0) < 8) return empty();
        auto meal = make("minecraft:bone_meal");
        if (!meal || !filter(*meal)) return empty();
        if (!simulate) return ComposterBlock::extractItem(at.region, block, at.pos);
        return *meal;
    };
    return h;
}
} // namespace

ItemPredicate any() {
    return [](ItemStack const&) { return true; };
}

void registerBlock(std::string const& blockId, Handler handler) { registry().byId[blockId] = std::move(handler); }
void registerProvider(Provider provider) { registry().providers.push_back(std::move(provider)); }

std::optional<Handler> find(Access const& at) {
    static bool init = [] {
        registerBlock("minecraft:composter", composterHandler());
        // last resort: any block whose block actor is a vanilla container
        registry().fallback = [](Access const& at) -> std::optional<Handler> {
            if (!containerAt(at)) return std::nullopt;
            return containerHandler();
        };
        return true;
    }();
    (void)init;
    return registry().find(at);
}

ItemStack insert(BlockSource& region, BlockPos const& pos, int face, ItemStack const& stack, bool simulate) {
    Access at{region, pos, face};
    auto   h = find(at);
    if (!h || !h->insert) return stack;
    return h->insert(at, stack, simulate);
}

ItemStack extract(BlockSource& region, BlockPos const& pos, int face, ItemPredicate const& filter, int amount, bool simulate) {
    Access at{region, pos, face};
    auto   h = find(at);
    if (!h || !h->extract) return empty();
    return h->extract(at, filter ? filter : any(), amount, simulate);
}

int slotCount(BlockSource& region, BlockPos const& pos, int face) {
    Access at{region, pos, face};
    auto   h = find(at);
    return h && h->slots ? h->slots(at) : 0;
}

bool has(BlockSource& region, BlockPos const& pos, int face) { return find(Access{region, pos, face}).has_value(); }

} // namespace items

// ---- fluids ------------------------------------------------------------------------------------
namespace fluids {

namespace {
Registry<Handler, Provider>& registry() {
    static Registry<Handler, Provider> r;
    return r;
}

struct FluidItem {
    FluidStack  contents;
    std::string emptyItem;
};
std::unordered_map<std::string, FluidItem>& fluidItems() {
    static std::unordered_map<std::string, FluidItem> m{
        {"minecraft:water_bucket",       {{"minecraft:water", 1000}, "minecraft:bucket"}      },
        {"minecraft:lava_bucket",        {{"minecraft:lava", 1000}, "minecraft:bucket"}       },
        {"minecraft:powder_snow_bucket", {{"minecraft:powder_snow", 1000}, "minecraft:bucket"}},
        {"minecraft:milk_bucket",        {{"minecraft:milk", 1000}, "minecraft:bucket"}       },
    };
    return m;
}

/// Block states through the block's serialisation id (works for string states too).
CompoundTag const* statesOf(Block const& block) {
    auto const& id = block.mSerializationId.get();
    if (!id.contains("states", Tag::Type::Compound)) return nullptr;
    return &id["states"].get<CompoundTag>();
}

int stateInt(Block const& block, char const* key) {
    auto const* st = statesOf(block);
    if (!st || !st->contains(key, Tag::Type::Int)) return -1;
    return (*st)[key].get<IntTag>().data;
}

std::string stateString(Block const& block, char const* key) {
    auto const* st = statesOf(block);
    if (!st || !st->contains(key, Tag::Type::String)) return {};
    return std::string((*st)[key].get<StringTag>());
}

/// `id` with `fill_level` / `cauldron_liquid` set (nullptr if the game doesn't know it).
Block const* cauldronWith(char const* id, int level, std::string const& liquid) {
    auto def = Block::tryGetFromRegistry(HashedString(id));
    if (!def) return nullptr;
    CompoundTag nbt = def->mSerializationId.get();
    if (!nbt.contains("states", Tag::Type::Compound)) return nullptr;
    auto& states = nbt["states"].get<CompoundTag>();
    states.putInt("fill_level", level);
    states.putString("cauldron_liquid", liquid);
    auto b = Block::tryGetFromRegistry(nbt);
    if (!b || stateInt(*b, "fill_level") != level || stateString(*b, "cauldron_liquid") != liquid) return nullptr;
    return b.as_ptr();
}

/// A full cauldron of `fluid` (nullptr for fluids a cauldron can't hold).
Block const* fullCauldron(std::string const& fluid) {
    if (fluid == "minecraft:water") return cauldronWith("minecraft:cauldron", 6, "water");
    if (fluid == "minecraft:lava") return cauldronWith("minecraft:lava_cauldron", 6, "lava");
    if (fluid == "minecraft:powder_snow") return cauldronWith("minecraft:cauldron", 6, "powder_snow");
    return nullptr;
}

Block const* emptyCauldron() { return cauldronWith("minecraft:cauldron", 0, "water"); }

/// The fluid of a full cauldron ("" when it is not full or holds something else).
std::string cauldronFluid(Block const& block) {
    auto id = block.getTypeName();
    if (id != "minecraft:cauldron" && id != "minecraft:lava_cauldron") return {};
    if (stateInt(block, "fill_level") != 6) return {};
    auto liquid = stateString(block, "cauldron_liquid");
    if (liquid == "water" || liquid == "lava" || liquid == "powder_snow") return "minecraft:" + liquid;
    return {};
}

bool isEmptyCauldron(Block const& block) {
    return block.getTypeName() == "minecraft:cauldron" && stateInt(block, "fill_level") == 0;
}

/// Cauldrons hold one bucket and only move whole buckets (as NeoForge's cauldron wrapper):
/// water, lava and powder snow.
Handler cauldronHandler() {
    Handler h;
    h.tanks = [](Access const& at) {
        auto fluid = cauldronFluid(at.region.getBlock(at.pos));
        return std::vector<Tank>{{FluidStack{fluid, fluid.empty() ? 0 : 1000}, 1000}};
    };
    h.fill = [](Access const& at, FluidStack const& stack, bool simulate) -> int {
        if (stack.amount < 1000) return 0;
        auto const& block = at.region.getBlock(at.pos);
        if (!isEmptyCauldron(block)) return 0;
        auto const* full = fullCauldron(stack.fluid);
        if (!full) return 0;
        if (!simulate) game::setBlock(at.region, at.pos, *full);
        return 1000;
    };
    h.drain = [](Access const& at, FluidStack const& request, bool simulate) -> FluidStack {
        if (request.amount < 1000) return {};
        auto fluid = cauldronFluid(at.region.getBlock(at.pos));
        if (fluid.empty() || (!request.fluid.empty() && request.fluid != fluid)) return {};
        if (!simulate) {
            if (auto const* e = emptyCauldron()) game::setBlock(at.region, at.pos, *e);
        }
        return {fluid, 1000};
    };
    return h;
}
} // namespace

void registerBlock(std::string const& blockId, Handler handler) { registry().byId[blockId] = std::move(handler); }
void registerProvider(Provider provider) { registry().providers.push_back(std::move(provider)); }

std::optional<Handler> find(Access const& at) {
    static bool init = [] {
        registerBlock("minecraft:cauldron", cauldronHandler());
        registerBlock("minecraft:lava_cauldron", cauldronHandler());
        return true;
    }();
    (void)init;
    return registry().find(at);
}

int fill(BlockSource& region, BlockPos const& pos, int face, FluidStack const& stack, bool simulate) {
    if (stack.empty()) return 0;
    Access at{region, pos, face};
    auto   h = find(at);
    return h && h->fill ? h->fill(at, stack, simulate) : 0;
}

FluidStack drain(BlockSource& region, BlockPos const& pos, int face, FluidStack const& request, bool simulate) {
    Access at{region, pos, face};
    auto   h = find(at);
    return h && h->drain ? h->drain(at, request, simulate) : FluidStack{};
}

std::vector<Tank> tanks(BlockSource& region, BlockPos const& pos, int face) {
    Access at{region, pos, face};
    auto   h = find(at);
    return h && h->tanks ? h->tanks(at) : std::vector<Tank>{};
}

FluidStack fluidInItem(ItemStack const& stack) {
    auto id = items::idOf(stack);
    if (id == "minecraft:potion" && stack.getAuxValue() == 0) return {"minecraft:water", 250};
    auto it = fluidItems().find(id);
    return it == fluidItems().end() ? FluidStack{} : it->second.contents;
}

void registerFluidItem(std::string const& itemId, FluidStack contents, std::string const& emptyItemId) {
    fluidItems()[itemId] = {std::move(contents), emptyItemId};
}

} // namespace fluids

// ---- heat --------------------------------------------------------------------------------------
namespace heat {

namespace {
std::unordered_map<std::string, Provider>& providers() {
    static std::unordered_map<std::string, Provider> m;
    return m;
}
} // namespace

void registerBlock(std::string const& blockId, Provider provider) { providers()[blockId] = std::move(provider); }

bool isPassiveHeater(Block const& block) {
    auto id = block.getTypeName();
    if (id == "minecraft:fire" || id == "minecraft:soul_fire" || id == "minecraft:lava" || id == "minecraft:flowing_lava"
        || id == "minecraft:magma")
        return true;
    if (id == "minecraft:campfire" || id == "minecraft:soul_campfire")
        return game::intState(block, "extinguished").value_or(0) == 0;
    return false;
}

HeatLevel level(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return HeatLevel::None;
    auto const& block = region.getBlock(pos);
    if (auto it = providers().find(block.getTypeName()); it != providers().end()) return it->second(region, pos, block);
    return isPassiveHeater(block) ? HeatLevel::Smouldering : HeatLevel::None;
}

} // namespace heat

} // namespace lv::feature::common
