// Building unit: blocks that hold an item (BlockData["building"]):
// - placard: shows one item; a matching item inserted emits a 19-tick redstone pulse; punching
//   takes the item back (Create's PlacardBlock). The client mod draws the item on the plate
//   (display item, feature/itemrender).
// - copycat panel / step: the applied block ("material") and the item it consumed; the wrench
//   resets it (Create's CopycatBlock).
// - brackets: metal / wooden brackets applied to shafts, cogwheels and pipes (Create's
//   BracketedBlockEntityBehaviour), dropped when the block goes.

#include <map>
#include <memory>
#include <set>

#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
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
//
// BlockData["building"]: "material" (block id), "state" (the material block with its states, SNBT)
// and "consumed" (the item it took). Bedrock names of Java's orientation properties:
//   FACING             facing_direction (int, Java order) / minecraft:facing_direction (string)
//   HORIZONTAL_FACING  minecraft:cardinal_direction (string) / direction (int: south west north east)
//   AXIS               pillar_axis (string)
//   LIT                the lit_ variant of the block (redstone lamp / ore)

namespace {

constexpr char const* kDirWords[6]{"down", "up", "north", "south", "west", "east"};
constexpr char const* kAxisWords[3]{"x", "y", "z"};

std::unique_ptr<CompoundTag> nbtOf(Block const& b) { return std::make_unique<CompoundTag>(*b.mSerializationId); }

Block const* fromNbt(CompoundTag const& nbt) {
    auto b = Block::tryGetFromRegistry(nbt);
    return b ? b.as_ptr() : nullptr;
}

bool hasState(CompoundTag const& nbt, std::string const& key) { return nbt.contains("states") && nbt["states"].contains(key); }
int  intOf(CompoundTag const& nbt, std::string const& key) {
    auto const& v = nbt["states"][key];
    if (v.hold(Tag::Type::Int)) return v.get<IntTag>().data;
    if (v.hold(Tag::Type::Byte)) return v.get<ByteTag>().data;
    return 0;
}
std::string strOf(CompoundTag const& nbt, std::string const& key) {
    auto const& v = nbt["states"][key];
    return v.hold(Tag::Type::String) ? std::string(v.get<StringTag>()) : std::string();
}
void setInt(CompoundTag& nbt, std::string const& key, int value) {
    auto& v = nbt["states"][key];
    if (v.hold(Tag::Type::Byte)) v = static_cast<uchar>(value);
    else v = value;
}
void setStr(CompoundTag& nbt, std::string const& key, std::string const& value) { nbt["states"][key] = value; }

int wordIndex(std::string const& s, char const* const* words, int n) {
    for (int i = 0; i < n; ++i)
        if (s == words[i]) return i;
    return -1;
}

/// Bedrock "direction" (int) of horizontal blocks: 0 south, 1 west, 2 north, 3 east.
int directionOf(int dir) { return dir == South ? 0 : dir == West ? 1 : dir == North ? 2 : 3; }
int dirOfDirection(int d) { return d == 0 ? South : d == 1 ? West : d == 2 ? North : East; }

/// Create's CopycatBlock.getAcceptedBlockState with a face: the default state with FACING /
/// HORIZONTAL_FACING / AXIS taken from the face.
Block const* orientToFace(Block const& material, int face) {
    auto nbt = nbtOf(material);
    if (hasState(*nbt, "facing_direction")) setInt(*nbt, "facing_direction", face);
    if (hasState(*nbt, "minecraft:facing_direction")) setStr(*nbt, "minecraft:facing_direction", kDirWords[face]);
    if (horizontal(face)) {
        if (hasState(*nbt, "minecraft:cardinal_direction")) setStr(*nbt, "minecraft:cardinal_direction", kDirWords[face]);
        if (hasState(*nbt, "direction")) setInt(*nbt, "direction", directionOf(face));
    }
    if (hasState(*nbt, "pillar_axis")) setStr(*nbt, "pillar_axis", kAxisWords[axisOf(face)]);
    auto const* b = fromNbt(*nbt);
    return b ? b : &material;
}

/// The lit / unlit twin of a block (Java LIT), "" when it has none.
std::string litTwin(std::string const& id) {
    static std::map<std::string, std::string, std::less<>> const twins{
        {"minecraft:redstone_lamp", "minecraft:lit_redstone_lamp"},
        {"minecraft:lit_redstone_lamp", "minecraft:redstone_lamp"},
        {"minecraft:redstone_ore", "minecraft:lit_redstone_ore"},
        {"minecraft:lit_redstone_ore", "minecraft:redstone_ore"},
        {"minecraft:deepslate_redstone_ore", "minecraft:lit_deepslate_redstone_ore"},
        {"minecraft:lit_deepslate_redstone_ore", "minecraft:deepslate_redstone_ore"},
    };
    auto it = twins.find(id);
    return it == twins.end() ? std::string() : it->second;
}

/// Create's CopycatBlockEntity.cycleMaterial: the next orientation of the material (first rule
/// that applies), nullptr when there is nothing to cycle.
Block const* cycled(Block const& material) {
    auto nbt = nbtOf(material);
    // 1. FACING: the next direction in Java's order
    if (hasState(*nbt, "facing_direction")) {
        setInt(*nbt, "facing_direction", (intOf(*nbt, "facing_direction") + 1) % 6);
        return fromNbt(*nbt);
    }
    if (hasState(*nbt, "minecraft:facing_direction")) {
        int i = wordIndex(strOf(*nbt, "minecraft:facing_direction"), kDirWords, 6);
        setStr(*nbt, "minecraft:facing_direction", kDirWords[(std::max(i, 0) + 1) % 6]);
        return fromNbt(*nbt);
    }
    // 2. HORIZONTAL_FACING: clockwise
    if (hasState(*nbt, "minecraft:cardinal_direction")) {
        int i = wordIndex(strOf(*nbt, "minecraft:cardinal_direction"), kDirWords, 6);
        setStr(*nbt, "minecraft:cardinal_direction", kDirWords[clockwise(i < 2 ? North : i)]);
        return fromNbt(*nbt);
    }
    if (hasState(*nbt, "direction")) {
        setInt(*nbt, "direction", directionOf(clockwise(dirOfDirection(intOf(*nbt, "direction") & 3))));
        return fromNbt(*nbt);
    }
    // 3. AXIS: x -> y -> z
    if (hasState(*nbt, "pillar_axis")) {
        int i = wordIndex(strOf(*nbt, "pillar_axis"), kAxisWords, 3);
        setStr(*nbt, "pillar_axis", kAxisWords[(std::max(i, 0) + 1) % 3]);
        return fromNbt(*nbt);
    }
    // 4. LIT
    if (auto twin = litTwin(material.getTypeName()); !twin.empty()) {
        auto const* t = game::blockWith(twin);
        return t;
    }
    return nullptr;
}

std::string snbtOf(Block const& b) { return b.mSerializationId->toSnbt(SnbtFormat::Minimize, 0); }

} // namespace

std::optional<std::string> copycatMaterial(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("material")) return std::nullopt;
    return (*d)["material"].get<std::string>();
}

Block const* copycatMaterialBlock(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("material")) return nullptr;
    if (d->contains("state") && (*d)["state"].is_string()) {
        if (auto nbt = CompoundTag::fromSnbt((*d)["state"].get<std::string>())) {
            if (auto const* b = fromNbt(*nbt)) return b;
        }
    }
    return game::blockWith((*d)["material"].get<std::string>());
}

namespace {

void setMaterial(GlobalPos const& pos, Block const* material, ItemStack const* consumed) {
    setUnitValue(pos, "material", material ? nlohmann::json(material->getTypeName()) : nlohmann::json());
    setUnitValue(pos, "state", material ? nlohmann::json(snbtOf(*material)) : nlohmann::json());
    if (consumed) setUnitValue(pos, "consumed", consumed->isNull() ? nlohmann::json() : items::toJson(items::withCount(*consumed, 1)));
    pushVisual(pos);
}

} // namespace

bool useCopycat(Player& player, ItemStack const& item, GlobalPos const& pos, int face) {
    auto const* block = blockAt(pos);
    auto const* info  = block ? infoOf(*block) : nullptr;
    if (!info || (info->kind != Kind::CopycatPanel && info->kind != Kind::CopycatStep)) return false;
    // the wrench resets the material (Create's onWrenched)
    if (!item.isNull() && item.getTypeName() == kWrenchId) {
        if (!copycatMaterial(pos)) return false;
        ItemStack consumed = heldItemOf(pos, "consumed");
        ItemStack none;
        setMaterial(pos, nullptr, &none);
        if (!player.isCreative()) give(player, consumed);
        particle(pos, "minecraft:breaking_item_terrain");
        playSound(pos, "block.itemframe.remove_item", 0.75f, 1.f);
        log().info("building: {} at {} {} {} reset", info->id, pos.x, pos.y, pos.z);
        return true;
    }
    auto const* material = blockOfItem(item);
    if (!material || !acceptableMaterial(*material)) return false;
    auto current = copycatMaterial(pos);
    if (current && *current == material->getTypeName()) {
        // Create's cycleMaterial: the same block again turns the material
        auto const* now  = copycatMaterialBlock(pos);
        auto const* next = now ? cycled(*now) : nullptr;
        if (!next || next == now) return false; // PASS: nothing to cycle
        setMaterial(pos, next, nullptr);
        playSound(pos, "block.itemframe.add_item", 0.75f, 0.95f);
        log().info("building: {} at {} {} {} turned its material to {}", info->id, pos.x, pos.y, pos.z, snbtOf(*next));
        return true;
    }
    if (current) return false; // PASS: another material is already applied
    Block const* applied = orientToFace(*game::blockWith(material->getTypeName()), face);
    // Create's setMaterial: a neighbour of the same copycat state with the same material block lends
    // its exact material state (orientations match)
    for (int d = 0; d < 6; ++d) {
        auto np = offset(pos, d);
        if (blockAt(np) != block) continue;
        auto const* other = copycatMaterialBlock(np);
        if (other && other->getTypeName() == material->getTypeName()) {
            applied = other;
            break;
        }
    }
    setMaterial(pos, applied, &item);
    playSound(pos, "use.stone", 1.f, 0.75f);
    if (!player.isCreative()) consumeHeld(player);
    log().info("building: {} at {} {} {} takes the look of {}", info->id, pos.x, pos.y, pos.z, material->getTypeName());
    return true;
}

// ---- brackets ---------------------------------------------------------------------------------------------
//
// BlockData["building"]["bracket"] = {id, facing, axis, cog} on shafts and cogwheels (Create's
// BracketedBlockEntityBehaviour; fluid pipes keep their own, U17). facing: the bracket's facing (its
// plate sits on the opposite side), axis: the shaft's axis, cog: the cogwheel model.

std::optional<std::string> bracketAt(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("bracket")) return std::nullopt;
    return (*d)["bracket"].value("id", std::string());
}

namespace {

bool isBracketItem(std::string const& id) { return id == "levitate:metal_bracket" || id == "levitate:wooden_bracket"; }

/// Create's BracketedBlockEntityBehaviour.isAvailable: simple kinetic blocks with an axis.
bool bracketableKinetic(std::string const& id) {
    return id == "levitate:shaft" || id == "levitate:cogwheel" || id == "levitate:large_cogwheel";
}

void bracketSound(GlobalPos const& pos, std::string const& id) {
    playSound(pos, id == "levitate:wooden_bracket" ? "use.scaffolding" : "use.netherite", 0.75f, 1.f);
}

} // namespace

bool useBracket(Player& player, ItemStack const& item, GlobalPos const& pos, int face) {
    if (item.isNull()) return false;
    std::string id = item.getTypeName();
    if (!isBracketItem(id)) return false;
    // the bracket is never placed as a block: it only goes onto bracketable blocks
    std::string target = idAt(pos);
    if (!bracketableKinetic(target)) return true;
    auto const* block = blockAt(pos);
    int         axis  = block ? stateOf(*block, "levitate:axis", 1) : 1;
    // Create's BracketBlockItem: the clicked face, or (an end of the shaft) the face towards the player
    int facing = face;
    if (axisOf(facing) == axis) facing = opposite(nearestLooking(player)[0]);
    if (axisOf(facing) == axis) return true;
    auto previous = bracketAt(pos);
    if (previous && *previous == id) return true;
    nlohmann::json value{{"id", id}, {"facing", facing}, {"axis", axis}, {"cog", target != "levitate:shaft"}};
    setUnitValue(pos, "bracket", value);
    if (previous && !player.isCreative()) give(player, makeItem(*previous)); // swapped
    consumeHeld(player);
    bracketSound(pos, id);
    pushVisual(pos);
    log().info("building: {} applied to {} at {} {} {} (facing {})", id, target, pos.x, pos.y, pos.z, kDirNames[facing]);
    markDirty(pos); // girders next to it may attach
    return true;
}

bool removeBracket(Player* player, GlobalPos const& pos) {
    auto id = bracketAt(pos);
    if (!id) return false;
    setUnitValue(pos, "bracket", nullptr);
    if (player && !player->isCreative()) give(*player, makeItem(*id));
    bracketSound(pos, *id);
    pushVisual(pos);
    markDirty(pos);
    log().info("building: {} removed from {} {} {}", *id, pos.x, pos.y, pos.z);
    return true;
}

nlohmann::json const* bracketData(GlobalPos const& pos) {
    auto const* d = unitData(pos);
    if (!d || !d->contains("bracket")) return nullptr;
    return &(*d)["bracket"];
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
        if (u.contains("material") || u.contains("bracket")) pushRemoved(pos);
        (void)previousId;
    });
    // a contraption puts the data back turned: bracket facing / axis and the copycat material's orientation
    bd::onRotate(kDataKey, [](bd::Json& u, int axis, int quarters) {
        if (u.contains("bracket") && u["bracket"].is_object()) {
            auto& b = u["bracket"];
            int   f = b.value("facing", 1), a = b.value("axis", 1);
            b["facing"] = rotateDir(f, axis, quarters);
            // the shaft axis turns as a direction along it
            b["axis"] = axisOf(rotateDir(a == 0 ? East : a == 1 ? Up : South, axis, quarters));
        }
        if (u.contains("state") && u["state"].is_string()) {
            if (auto nbt = CompoundTag::fromSnbt(u["state"].get<std::string>())) {
                auto& n = *nbt;
                if (hasState(n, "facing_direction")) setInt(n, "facing_direction", rotateDir(intOf(n, "facing_direction") % 6, axis, quarters));
                if (hasState(n, "minecraft:facing_direction")) {
                    int i = wordIndex(strOf(n, "minecraft:facing_direction"), kDirWords, 6);
                    if (i >= 0) setStr(n, "minecraft:facing_direction", kDirWords[rotateDir(i, axis, quarters)]);
                }
                if (hasState(n, "minecraft:cardinal_direction")) {
                    int i = wordIndex(strOf(n, "minecraft:cardinal_direction"), kDirWords, 6);
                    int r = i >= 2 ? rotateDir(i, axis, quarters) : i;
                    if (r >= 2) setStr(n, "minecraft:cardinal_direction", kDirWords[r]);
                }
                if (hasState(n, "direction")) {
                    int r = rotateDir(dirOfDirection(intOf(n, "direction") & 3), axis, quarters);
                    if (r >= 2) setInt(n, "direction", directionOf(r));
                }
                if (hasState(n, "pillar_axis")) {
                    int i = wordIndex(strOf(n, "pillar_axis"), kAxisWords, 3);
                    if (i >= 0) setStr(n, "pillar_axis", kAxisWords[axisOf(rotateDir(i == 0 ? East : i == 1 ? Up : South, axis, quarters))]);
                }
                u["state"] = n.toSnbt(SnbtFormat::Minimize, 0);
            }
        }
    });
}

void clearPulses() { pulses().clear(); }

} // namespace detail

} // namespace lv::feature::building
