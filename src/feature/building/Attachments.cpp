// Building unit: blocks that hold an item (BlockData["building"]):
// - placard: shows one item; a matching item inserted emits a 19-tick redstone pulse; punching
//   takes the item back (Create's PlacardBlock). The client mod draws the item on the plate
//   (display item, feature/itemrender).
// - copycat panel / step: the applied block ("material") and the item it consumed; the wrench
//   resets it (Create's CopycatBlock).
// - brackets: metal / wooden brackets applied to shafts, cogwheels and pipes (Create's
//   BracketedBlockEntityBehaviour), dropped when the block goes.

#include <map>
#include <set>

#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/CachedComponentData.h"

#include "feature/building/Building.h"
#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace bd    = common::blockdata;
namespace items = common::items;

namespace {

std::map<GlobalPos, int>& pulses() {
    static std::map<GlobalPos, int> p;
    return p;
}

nlohmann::json const* unitData(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d || !d->contains(kDataKey)) return nullptr;
    return &(*d)[kDataKey];
}

ItemStack heldItemOf(GlobalPos const& pos, char const* key) {
    auto const* d = unitData(pos);
    if (!d || !d->contains(key)) return {};
    return items::fromJson((*d)[key]);
}

void setUnitValue(GlobalPos const& pos, char const* key, nlohmann::json value) {
    auto& doc = bd::edit(pos);
    auto& u   = doc[kDataKey];
    if (value.is_null()) u.erase(key);
    else u[key] = std::move(value);
    if (u.empty()) doc.erase(kDataKey);
    itemrender::refresh(pos); // the placard's item for the client mod
}

/// The placard's item for the client mod (Create's PlacardRenderer): on the front of the plate
/// (3 px thick, against the block it hangs on), facing out, half a block big.
void placardDisplay(GlobalPos const& pos, Block const& block, std::vector<std::pair<std::int64_t, itemrender::DisplayItem>>& out) {
    ItemStack item = heldItemOf(pos, "item");
    if (item.isNull()) return;
    static constexpr int kNormal[6][3]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    int  facing = std::clamp(stateOf(block, "levitate:facing", 2), 0, 5);
    auto n      = kNormal[facing];
    double const front = -.5 + 3 / 16.0 + .01; // from the centre along the facing
    itemrender::DisplayItem d;
    d.stack    = item;
    d.at       = itemrender::facingFace(facing);
    d.at.x     = pos.x + .5 + n[0] * front;
    d.at.y     = pos.y + .5 + n[1] * front;
    d.at.z     = pos.z + .5 + n[2] * front;
    d.at.scale = .5f;
    out.emplace_back(0, std::move(d));
}

struct PlacardItems {
    PlacardItems() { itemrender::registerProvider(kPlacardId, placardDisplay); }
} const gPlacardItems;

/// The block a block item places, nullptr if the item isn't a block item.
Block const* blockOfItem(ItemStack const& stack) {
    if (stack.isNull()) return nullptr;
    auto const* block = stack.mBlock;
    if (!block || block->isAir()) return nullptr;
    return block;
}

/// Create's CopycatBlock.getAcceptedBlockState: full, solid, non-entity blocks (not copycats).
bool acceptableMaterial(Block const& b) {
    if (infoOf(b) && (infoOf(b)->kind == Kind::CopycatPanel || infoOf(b)->kind == Kind::CopycatStep)) return false;
    auto const& type = b.getBlockType();
    if (type.isContainerBlock() || type.isCraftingBlock()) return false;
    if (!b.mCachedComponentData->mIsSolid) return false;
    for (int face = 0; face < 6; ++face)
        if (!b.canProvideSupport(static_cast<uchar>(face), BlockSupportType::Center)) return false;
    return true;
}

} // namespace

// ---- placard ------------------------------------------------------------------------------------------

std::optional<std::string> placardItem(GlobalPos const& pos) {
    auto item = heldItemOf(pos, "item");
    if (item.isNull()) return std::nullopt;
    return item.getTypeName();
}

bool usePlacard(Player& player, ItemStack const& inHand, GlobalPos const& pos) {
    auto const* block = blockAt(pos);
    if (!block || block->getTypeName() != kPlacardId) return false;
    ItemStack inBlock = heldItemOf(pos, "item");
    if (!player.canUseAbility(AbilitiesIndex::Build) || inHand.isNull() || !inBlock.isNull()) {
        if (inBlock.isNull() || inHand.isNull()) return true; // FAIL: nothing happens
        if (stateOf(*block, "levitate:powered")) {
            ensurePlacardPulse(pos); // powered when the world was saved: finish the pulse
            return true;
        }
        bool test = common::filter::isFilterItem(inBlock) ? common::filter::matches(inBlock, inHand) : items::sameItemSameData(inHand, inBlock);
        if (!test) {
            playSound(pos, "note.bass", 1.f, 0.6f); // Create's DENY
            log().info("building: placard at {} {} {} denied {}", pos.x, pos.y, pos.z, inHand.getTypeName());
            return true;
        }
        playSound(pos, "random.orb", 1.f, 1.2f); // Create's CONFIRM
        set(pos, *withStates(*block, {{"levitate:powered", 1}}));
        pulses()[pos] = 19;
        log().info("building: placard at {} {} {} matched {}, pulse", pos.x, pos.y, pos.z, inHand.getTypeName());
        return true;
    }
    playSound(pos, "block.itemframe.add_item");
    setUnitValue(pos, "item", items::toJson(items::withCount(inHand, 1)));
    consumeHeld(player);
    log().info("building: placard at {} {} {} holds {}", pos.x, pos.y, pos.z, inHand.getTypeName());
    return true;
}

bool attackPlacard(Player& player, GlobalPos const& pos) {
    auto const* block = blockAt(pos);
    if (!block || block->getTypeName() != kPlacardId) return false;
    ItemStack held = heldItemOf(pos, "item");
    if (held.isNull()) return false;
    give(player, held);
    playSound(pos, "block.itemframe.remove_item");
    setUnitValue(pos, "item", nullptr);
    log().info("building: placard at {} {} {} emptied by {}", pos.x, pos.y, pos.z, player.getRealName());
    return true;
}

void ensurePlacardPulse(GlobalPos const& pos) { pulses().try_emplace(pos, 19); }

void placardTick() {
    auto& p = pulses();
    for (auto it = p.begin(); it != p.end();) {
        if (--it->second > 0) {
            ++it;
            continue;
        }
        auto const* b = blockAt(it->first);
        if (b && b->getTypeName() == kPlacardId && stateOf(*b, "levitate:powered")) set(it->first, *withStates(*b, {{"levitate:powered", 0}}));
        it = p.erase(it);
    }
}

// ---- copycats ------------------------------------------------------------------------------------------

std::optional<std::string> copycatMaterial(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("material")) return std::nullopt;
    return (*d)["material"].get<std::string>();
}

bool useCopycat(Player& player, ItemStack const& item, GlobalPos const& pos) {
    auto const* block = blockAt(pos);
    auto const* info  = block ? infoOf(*block) : nullptr;
    if (!info || (info->kind != Kind::CopycatPanel && info->kind != Kind::CopycatStep)) return false;
    // the wrench resets the material (Create's onWrenched)
    if (!item.isNull() && item.getTypeName() == kWrenchId) {
        if (!copycatMaterial(pos)) return false;
        ItemStack consumed = heldItemOf(pos, "consumed");
        setUnitValue(pos, "material", nullptr);
        setUnitValue(pos, "consumed", nullptr);
        if (!player.isCreative()) give(player, consumed);
        playSound(pos, "block.itemframe.remove_item", 0.75f, 1.f);
        log().info("building: {} at {} {} {} reset", info->id, pos.x, pos.y, pos.z);
        return true;
    }
    auto const* material = blockOfItem(item);
    if (!material || !acceptableMaterial(*material)) return false;
    auto current = copycatMaterial(pos);
    if (current && *current == material->getTypeName()) {
        // Create cycles the material's own orientation states; Bedrock materials are applied in
        // their default state, so there is nothing to cycle
        return false;
    }
    if (current) return false; // PASS: another material is already applied
    setUnitValue(pos, "material", material->getTypeName());
    setUnitValue(pos, "consumed", items::toJson(items::withCount(item, 1)));
    playSound(pos, "use.stone", 1.f, 0.8f);
    if (!player.isCreative()) consumeHeld(player);
    log().info("building: {} at {} {} {} takes the look of {}", info->id, pos.x, pos.y, pos.z, material->getTypeName());
    return true;
}

// ---- brackets ---------------------------------------------------------------------------------------------

std::optional<std::string> bracketAt(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("bracket")) return std::nullopt;
    return (*d)["bracket"].value("id", std::string());
}

bool useBracket(Player& player, ItemStack const& item, GlobalPos const& pos, int face) {
    if (item.isNull()) return false;
    std::string id = item.getTypeName();
    if (id != "levitate:metal_bracket" && id != "levitate:wooden_bracket") return false;
    // the bracket is never placed as a block: it only goes onto bracketable blocks
    std::string target = idAt(pos);
    static std::set<std::string, std::less<>> const bracketable{
        "levitate:shaft", "levitate:cogwheel", "levitate:large_cogwheel", "levitate:fluid_pipe", "levitate:smart_fluid_pipe", "levitate:glass_fluid_pipe"};
    if (!bracketable.contains(target) || bracketAt(pos)) return true;
    setUnitValue(pos, "bracket", {{"id", id}, {"facing", face}});
    consumeHeld(player);
    playSound(pos, id == "levitate:wooden_bracket" ? "use.scaffolding" : "use.netherite");
    log().info("building: {} applied to {} at {} {} {}", id, target, pos.x, pos.y, pos.z);
    markDirty(pos); // girders next to it may attach
    return true;
}

namespace detail {

/// Drops what a building block held when the block goes (registered with BlockData).
void registerDrops() {
    bd::onErase([](GlobalPos const& pos, bd::Json const& doc, std::string const& previousId) {
        if (!doc.contains(kDataKey)) return;
        auto const& u = doc[kDataKey];
        if (u.contains("item")) drop(pos, items::fromJson(u["item"]));
        if (u.contains("consumed")) drop(pos, items::fromJson(u["consumed"]));
        if (u.contains("bracket")) drop(pos, makeItem(u["bracket"].value("id", std::string("levitate:metal_bracket"))));
        (void)previousId;
    });
}

void clearPulses() { pulses().clear(); }

} // namespace detail

} // namespace lv::feature::building
