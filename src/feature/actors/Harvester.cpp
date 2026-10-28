// Mechanical harvester (Create's HarvesterMovementBehaviour). No stationary effect.
//
// Each block its active area (0.45 blocks in front of it) enters is checked: mature crops are
// harvested and replanted (harvesterReplants), sugar cane / kelp / vines / bush-type plants are
// cut, leaves are sheared (45 % chance per drop). Drops go to contraption storage or drop.

#include <unordered_map>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/util/Random.h"

#include "feature/actors/Actors.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "mod/Config.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::ext;

namespace {

/// A crop: the state holding its age, the mature value, the state it replants at and the item
/// one of which is kept back as the replanted seed (Create: the block's own item).
struct Crop {
    char const* state;
    int         max;
    int         replantAge;
    char const* seed;
};

std::unordered_map<std::string, Crop> const& crops() {
    // Bedrock ids and states of Java's CropBlock and the empty-collision blocks with an "age"
    static std::unordered_map<std::string, Crop> const t{
        {"minecraft:wheat",            {"growth", 7, 0, "minecraft:wheat_seeds"}     },
        {"minecraft:carrots",          {"growth", 7, 0, "minecraft:carrot"}          },
        {"minecraft:potatoes",         {"growth", 7, 0, "minecraft:potato"}          },
        {"minecraft:beetroot",         {"growth", 7, 0, "minecraft:beetroot_seeds"}  },
        {"minecraft:torchflower_crop", {"growth", 1, 0, "minecraft:torchflower_seeds"}},
        {"minecraft:melon_stem",       {"growth", 7, 0, "minecraft:melon_seeds"}     },
        {"minecraft:pumpkin_stem",     {"growth", 7, 0, "minecraft:pumpkin_seeds"}   },
        {"minecraft:nether_wart",      {"age", 3, 0, "minecraft:nether_wart"}        },
        {"minecraft:sweet_berry_bush", {"growth", 3, 1, "minecraft:sweet_berries"}   },
        {"minecraft:cocoa",            {"age", 2, 0, "minecraft:cocoa_beans"}        },
    };
    return t;
}

bool endsWith(std::string_view s, std::string_view suffix) { return s.ends_with(suffix); }

bool isLeaves(std::string_view id) { return endsWith(id, "_leaves") || id == "minecraft:azalea_leaves_flowered"; }

/// Java GrowingPlantBlock (kelp, vines) and sugar cane: cut whole.
bool isGrowingPlant(std::string_view id) {
    static std::unordered_set<std::string_view> const s{
        "minecraft:reeds",         "minecraft:kelp",          "minecraft:weeping_vines", "minecraft:twisting_vines",
        "minecraft:cave_vines",    "minecraft:cave_vines_body_with_berries", "minecraft:cave_vines_head_with_berries",
    };
    return s.contains(id);
}

/// Java BushBlock (no age) with an empty collision shape: flowers, grass, ferns, saplings,
/// roots, fungi, seagrass... Mushrooms are excluded as in Create.
bool isBush(std::string_view id) {
    static std::unordered_set<std::string_view> const s{
        "minecraft:short_grass", "minecraft:tall_grass", "minecraft:fern", "minecraft:large_fern", "minecraft:deadbush",
        "minecraft:dandelion", "minecraft:poppy", "minecraft:blue_orchid", "minecraft:allium", "minecraft:azure_bluet",
        "minecraft:red_tulip", "minecraft:orange_tulip", "minecraft:white_tulip", "minecraft:pink_tulip",
        "minecraft:oxeye_daisy", "minecraft:cornflower", "minecraft:lily_of_the_valley", "minecraft:wither_rose",
        "minecraft:torchflower", "minecraft:sunflower", "minecraft:lilac", "minecraft:rose_bush", "minecraft:peony",
        "minecraft:pitcher_plant", "minecraft:pink_petals", "minecraft:wildflowers", "minecraft:open_eyeblossom",
        "minecraft:closed_eyeblossom", "minecraft:seagrass", "minecraft:crimson_roots", "minecraft:warped_roots",
        "minecraft:nether_sprouts", "minecraft:crimson_fungus", "minecraft:warped_fungus", "minecraft:mangrove_propagule",
        "minecraft:short_dry_grass", "minecraft:tall_dry_grass", "minecraft:bush", "minecraft:firefly_bush",
        "minecraft:small_dripleaf_block", "minecraft:leaf_litter", "minecraft:cactus_flower",
    };
    return s.contains(id) || endsWith(id, "_sapling");
}

/// Plants that stand in water: cutting leaves water behind (Java: their fluid state).
bool inWater(std::string_view id) { return id == "minecraft:kelp" || id == "minecraft:seagrass"; }

int intOf(Block const& b, char const* state) { return game::intState(b, state).value_or(0); }

/// Create's isValidCrop.
bool validCrop(Block const& block, Crop const& crop) {
    auto const& cfg     = config::get<Config>();
    int         age     = intOf(block, crop.state);
    std::string id      = block.getTypeName();
    bool        partial = cfg.harvestPartiallyGrown;
    bool        replant = cfg.harvesterReplants;
    if (id == "minecraft:cocoa") return age >= crop.max;
    if (id == "minecraft:sweet_berry_bush") {
        if (!partial) return age >= crop.max;
        return replant ? age > 1 : true;
    }
    if (!partial) return age >= crop.max;
    return replant ? age > 0 : true;
}

/// Create's isValidOther.
bool validOther(std::string const& id) {
    if (crops().contains(id)) return false;
    return id == "minecraft:reeds" || isLeaves(id) || isGrowingPlant(id) || isBush(id);
}

void visit(con::MovementContext& ctx, GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto bp = toBlockPos(pos);
    if (!region->hasChunksAt(bp, 0, false)) return;
    auto const& block = region->getBlock(bp);
    if (block.isAir()) return;
    std::string id    = block.getTypeName();
    auto        cropIt = crops().find(id);
    bool        notCropButCuttable = false;
    if (cropIt == crops().end() || !validCrop(block, cropIt->second)) {
        if (!validOther(id)) return;
        notCropButCuttable = true;
    }
    auto const& cfg = config::get<Config>();
    auto        level = ll::service::getLevel();
    if (!level) return;

    // drops (leaves: as with shears, each drop with 45 % chance)
    bool        leaves = isLeaves(id);
    ItemStack   tool   = leaves ? makeItem("minecraft:shears") : ItemStack::EMPTY_ITEM();
    float       chance = leaves ? .45f : 1.f;
    auto        drops  = dropsOf(*region, bp, block, tool);
    bool        seedTaken = notCropButCuttable;
    char const* seed      = cropIt != crops().end() ? cropIt->second.seed : "";
    for (auto& stack : drops) {
        if (chance < 1.f && level->getRandom().nextFloat() >= chance) continue;
        if (cfg.harvesterReplants && !seedTaken && stack.getTypeName() == seed) {
            seedTaken = true;
            if (stack.mCount <= 1) continue;
            stack.mCount = static_cast<uchar>(stack.mCount - 1);
        }
        collectOrDrop(ctx, stack);
    }

    // what stays: the replanted crop, or air / water
    Block const* next = nullptr;
    if (cfg.harvesterReplants && cropIt != crops().end() && !notCropButCuttable) {
        next = ext::withStates(block, cc::States{{cropIt->second.state, std::int64_t{cropIt->second.replantAge}}});
    } else {
        next = game::blockWith(inWater(id) ? "minecraft:water" : "minecraft:air");
    }
    if (!next) next = game::blockWith("minecraft:air");
    if (next) game::setBlock(*region, bp, *next);
    logAction("harvester harvested " + id + " at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z));
}

cc::Vec3d areaOffset(con::MovementContext const& ctx) { return cc::Vec3d::of(cc::offset(facingOf(ctx.states))) * .45; }

struct Registrar {
    Registrar() {
        con::MovementBehaviour b;
        b.isActive = [](con::MovementContext const& ctx) {
            // Create: not while moving towards its back
            return !pointingTowards(relativeMotion(ctx), cc::opposite(facingOf(ctx.states)));
        };
        b.tick = [](con::MovementContext& ctx) {
            if (auto p = newAreaPos(ctx, areaOffset(ctx))) visit(ctx, *p);
        };
        registerOwnActor(std::string(kHarvester), std::move(b));
        con::registerMovementRules(
            std::string(kHarvester),
            con::MovementRules{
                nullptr,
                [](cc::States const&) { return true; },
                [](cc::States const& s) { return cc::bit(cc::opposite(facingOf(s))); },
                nullptr,
            }
        );
    }
};
Registrar const gRegistrar;

} // namespace

} // namespace lv::feature::actors
