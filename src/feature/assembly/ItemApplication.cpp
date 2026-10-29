// Item application (Create's ManualApplicationRecipe): right-clicking a block with an item turns
// the block into the recipe's result block (andesite / copper / brass casing on stripped logs,
// railway casing, bound cardboard, ...). Works for players and deployers alike (both use
// GameMode). Recipes: `assets/recipes/item_application/*.json` (ingredient 0 = the block,
// ingredient 1 = the held item, first result = the new block, further results pop out).

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::assembly::application {

namespace items   = common::items;
namespace recipes = common::recipes;

bool tryApply(Player& player, BlockSource& region, BlockPos const& pos, ItemStack& held, bool isFakePlayer) {
    if (items::isEmpty(held)) return false;
    auto const& block = region.getBlock(pos);
    if (block.isAir()) return false;
    auto asItem = items::make(block.getTypeName());
    if (!asItem) return false;
    for (auto const& r : recipes::ofType("item_application")) {
        if (r.ingredients.size() < 2) continue;
        if (!recipes::matches(r.ingredients[0], *asItem) || !recipes::matches(r.ingredients[1], held)) continue;
        auto results = recipes::rollResults(r);
        if (results.empty()) return false;
        // the first result is the new block (Create's transformBlock), the others pop out
        if (auto const* next = game::blockWith(items::idOf(results.front()))) game::setBlock(region, pos, *next);
        for (std::size_t i = 1; i < results.size(); ++i) detail::dropItem(region, pos.x + .5, pos.y + .5, pos.z + .5, results[i]);
        int dim = static_cast<int>(player.getDimensionId());
        detail::playSound(dim, pos.x + .5, pos.y + .5, pos.z + .5, "dig.copper", 1.f, 1.45f);
        bool keep = r.raw.value("keep_held_item", r.raw.value("keepHeldItem", false)) || (!isFakePlayer && player.isCreative());
        if (!keep) {
            if (held.isDamageableItem()) {
                auto const* item = held.mItem.get();
                short       max  = item ? item->getMaxDamage() : 0;
                held.hurtAndBreak(1, &player);
                if (held.isNull() || (max > 0 && held.getDamageValue() >= max)) held = items::empty();
            } else {
                held = items::withCount(held, items::countOf(held) - 1);
            }
            player.setSelectedItem(held);
            if (!isFakePlayer) player.sendInventory(false);
        }
        log().info("item application {} at {} {} {} by {}", r.id, pos.x, pos.y, pos.z, player.getRealName());
        return true;
    }
    return false;
}

} // namespace lv::feature::assembly::application
