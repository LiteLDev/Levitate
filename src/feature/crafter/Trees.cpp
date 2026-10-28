// Crafter unit: Create's TreeCutter on Bedrock blocks (which blocks a saw may cut, what falls).

#include <set>

#include "mc/world/level/block/Block.h"

#include "core/crafter/TreeCutter.h"
#include "feature/crafter/Internal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::crafter::trees {

namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isMushroomStem(Block const& b, std::string_view id) {
    if (id != "minecraft:brown_mushroom_block" && id != "minecraft:red_mushroom_block" && id != "minecraft:mushroom_stem") return false;
    if (id == "minecraft:mushroom_stem") return true;
    int bits = game::intState(b, "huge_mushroom_bits").value_or(0);
    return bits == 10 || bits == 15;
}

/// BlockTags.LOGS (+ Create's slimy logs, mushroom stems).
bool isLog(Block const& b, std::string_view id) {
    if (!id.starts_with("minecraft:")) return endsWith(id, "_log") || endsWith(id, "_wood");
    return endsWith(id, "_log") || endsWith(id, "_wood") || endsWith(id, "_stem") || endsWith(id, "_hyphae") || id == "minecraft:wood"
        || isMushroomStem(b, id);
}
bool isRoot(std::string_view id) { return id == "minecraft:mangrove_roots" || id == "minecraft:muddy_mangrove_roots"; }
bool isLeaf(std::string_view id) { return endsWith(id, "_leaves") || id == "minecraft:azalea_leaves_flowered" || id == "minecraft:leaves" || id == "minecraft:leaves2"; }
bool isVerticalPlant(std::string_view id) {
    return id == "minecraft:bamboo" || id == "minecraft:cactus" || id == "minecraft:sugar_cane" || id == "minecraft:reeds"
        || id == "minecraft:kelp";
}
bool isChorus(std::string_view id) { return id == "minecraft:chorus_plant" || id == "minecraft:chorus_flower"; }
/// Create's #create:tree_attachments ⚠ (bee nests, cocoa, vines, moss carpets, shroomlights).
bool isAttachment(std::string_view id) {
    static std::set<std::string_view> const ids{
        "minecraft:bee_nest", "minecraft:cocoa", "minecraft:vine", "minecraft:moss_carpet", "minecraft:shroomlight",
        "minecraft:pale_hanging_moss",
    };
    return ids.contains(id);
}
int nonDecaying(Block const& b, std::string_view id) {
    if (id == "minecraft:red_mushroom_block" && !isMushroomStem(b, id)) return 2;
    if (id == "minecraft:brown_mushroom_block" && !isMushroomStem(b, id)) return 3;
    if (id == "minecraft:nether_wart_block" || id == "minecraft:warped_wart_block") return 3;
    if (id == "minecraft:weeping_vines") return 3;
    return -1;
}

core::crafter::TreeBlock describe(Block const& b) {
    std::string_view          id = b.getTypeName();
    core::crafter::TreeBlock  t;
    t.nonDecaying = nonDecaying(b, id);
    if (isAttachment(id)) t.kind = core::crafter::TreeKind::Attachment;
    else if (isLog(b, id)) t.kind = core::crafter::TreeKind::Log;
    else if (isRoot(id)) t.kind = core::crafter::TreeKind::Root;
    else if (isLeaf(id)) t.kind = core::crafter::TreeKind::Leaf;
    else if (isVerticalPlant(id)) t.kind = core::crafter::TreeKind::VerticalPlant;
    else if (isChorus(id)) t.kind = core::crafter::TreeKind::Chorus;
    return t;
}

core::crafter::TreeBlock describeId(std::string_view id) {
    core::crafter::TreeBlock t;
    if (isAttachment(id)) t.kind = core::crafter::TreeKind::Attachment;
    else if (isRoot(id)) t.kind = core::crafter::TreeKind::Root;
    else if (isLeaf(id)) t.kind = core::crafter::TreeKind::Leaf;
    else if (isVerticalPlant(id)) t.kind = core::crafter::TreeKind::VerticalPlant;
    else if (isChorus(id)) t.kind = core::crafter::TreeKind::Chorus;
    else if (endsWith(id, "_log") || endsWith(id, "_wood") || endsWith(id, "_stem") || endsWith(id, "_hyphae")) t.kind = core::crafter::TreeKind::Log;
    return t;
}

} // namespace

bool sawable(Block const& b) {
    std::string_view id = b.getTypeName();
    if (endsWith(id, "_sapling") || id == "minecraft:sapling" || id == "minecraft:bamboo_sapling" || id == "minecraft:mangrove_propagule")
        return false;
    if (isLog(b, id) || isLeaf(id) || isRoot(id)) return true;
    if (id == "minecraft:bamboo") return true;
    if (id == "minecraft:melon_block" || id == "minecraft:pumpkin") return true; // Create's #solid_crops
    if (id == "minecraft:cactus" || id == "minecraft:sugar_cane" || id == "minecraft:reeds" || id == "minecraft:kelp") return true;
    if (id == "minecraft:chorus_plant") return true;
    return false;
}

void fell(GlobalPos const& cut, std::string const& brokenId, std::function<void(GlobalPos const&, ItemStack const&)> const& drop) {
    auto reader = [&](core::crafter::TreePos const& p) -> core::crafter::TreeBlock {
        auto const* b = world::blockAt({cut.dim, p.x, p.y, p.z});
        if (!b || b->isAir()) return {};
        return describe(*b);
    };
    auto broken = describeId(brokenId);
    if (brokenId == "minecraft:mushroom_stem") broken.kind = core::crafter::TreeKind::Log;
    auto tree = core::crafter::findTree(reader, {cut.x, cut.y, cut.z}, broken);
    if (tree.empty()) return;
    auto destroyAll = [&](std::vector<core::crafter::TreePos> const& list, float chance) {
        for (auto const& p : list) {
            GlobalPos gp{cut.dim, p.x, p.y, p.z};
            for (auto const& stack : world::destroyBlock(gp, true, chance)) drop(gp, stack);
        }
    };
    // Create's AbstractBlockBreakQueue.destroyBlocks: attachments, logs, leaves
    destroyAll(tree.attachments, 1 / 32.f);
    destroyAll(tree.logs, 1 / 2.f);
    destroyAll(tree.leaves, 1 / 8.f);
    log().info("saw felled a tree at {}: {} logs, {} leaves, {} attachments", cut.key(), tree.logs.size(), tree.leaves.size(),
               tree.attachments.size());
}

} // namespace lv::feature::crafter::trees
