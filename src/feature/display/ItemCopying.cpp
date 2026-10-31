// Create's ItemCopyingRecipe (SupportsItemCopying: schedules, clipboards and filters): a written item
// crafted with blank ones of the same kind gives that many + 1 copies. Bedrock recipes can't look
// at item data, so `levitate:<item>_copying_<n>` recipes (n of the item -> n of it) make the craft
// and this hook gives the results the written item's data (without enchantments). Two written
// items in the grid are refused (Create's copyCheck finds no recipe for them).

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/PlayerUIContainer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/inventory/network/ItemStackNetResult.h"
#include "mc/world/inventory/network/crafting/ItemStackRequestActionCraftHandler.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"

#include "feature/common/Items.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::display {

namespace {

namespace items = common::items;

// (Bedrock clipboards stack to 1: no copying recipe can give several of them)
constexpr std::array<std::string_view, 4> kCopyable{
    "levitate:schedule",
    "levitate:filter",
    "levitate:attribute_filter",
    "levitate:package_filter",
};

bool copyable(std::string_view id) { return std::find(kCopyable.begin(), kCopyable.end(), id) != kCopyable.end(); }

/// Create's canCopyFromItem: the item carries its data component.
bool written(ItemStack const& s) {
    auto const* tag = items::userData(s);
    if (!tag) return false;
    for (auto const& [key, value] : *tag)
        if (key != "ench" && key != "display" && key != "RepairCost") return true;
    return false;
}

/// The crafting grid of the player's UI container (3x3 table or 2x2 inventory grid).
std::vector<ItemStack const*> grid(Player& player, bool table) {
    std::vector<ItemStack const*> out;
    auto&                         ui = player.mPlayerUIContainer.get();
    int                           from = table ? 32 : 28, to = table ? 40 : 31;
    for (int i = from; i <= to; ++i) out.push_back(&ui.getItem(i));
    return out;
}

struct Copy {
    ItemStack const* source  = nullptr;
    int              targets = 0;
    bool             refused = false;
};

/// Create's ItemCopyingRecipe.copyCheck over a grid holding only `id`.
std::optional<Copy> copyCheck(std::vector<ItemStack const*> const& slots, std::string const& id) {
    Copy c;
    int  count = 0;
    for (auto const* s : slots) {
        if (items::isEmpty(*s)) continue;
        if (items::idOf(*s) != id) return std::nullopt;
        ++count;
        if (written(*s)) {
            if (c.source) c.refused = true; // two written items: no recipe
            else c.source = s;
        } else {
            ++c.targets;
        }
    }
    if (count < 2) return std::nullopt;
    return c;
}

} // namespace

} // namespace lv::feature::display

LL_TYPE_INSTANCE_HOOK(
    DisplayItemCopyingCraftHook,
    ll::memory::HookPriority::Normal,
    ItemStackRequestActionCraftHandler,
    &ItemStackRequestActionCraftHandler::_initCraftResults,
    ItemStackNetResult,
    std::vector<ItemInstance> const& results,
    uchar                            numCrafts
) {
    namespace d = lv::feature::display;
    try {
        Player& player = mPlayer;
        if (!player.isClientSide() && results.size() == 1 && !results.front().isNull() && d::copyable(results.front().getTypeName())) {
            std::string id = results.front().getTypeName();
            for (bool table : {true, false}) {
                auto c = d::copyCheck(d::grid(player, table), id);
                if (!c) continue;
                if (c->refused) {
                    lv::log().info("display: {} tried to copy two written {} items (refused)", player.getRealName(), id);
                    return ItemStackNetResult::InvalidCraftResult;
                }
                if (!c->source || c->targets == 0) break; // blank items only: plain craft
                auto copy = results;
                if (auto const* tag = d::items::userData(*c->source)) {
                    auto data = tag->clone();
                    data->erase("ench"); // Create's createCopy drops the enchantments
                    copy.front().setUserData(std::move(data));
                }
                lv::log().info("display: {} copied a written {} onto {} blank one(s)", player.getRealName(), id, c->targets);
                return origin(copy, numCrafts);
            }
        }
    } catch (std::exception const& e) {
        lv::log().error("display: item copying failed: {}", e.what());
    }
    return origin(results, numCrafts);
}

namespace lv::feature::display {
namespace {
class ItemCopyingModule {
public:
    void install() { DisplayItemCopyingCraftHook::hook(); }
    void uninstall() { DisplayItemCopyingCraftHook::unhook(); }
};
} // namespace
} // namespace lv::feature::display

LV_REGISTER_MODULE(lv::feature::display::ItemCopyingModule);
