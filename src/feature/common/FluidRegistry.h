#pragma once

// The fluid registry (added by U17 `pipes` next to the U3 capability contract; the only one since
// U35): what a fluid id *is*, for every unit that moves or shows fluids (pipes, tanks, spouts,
// drains, hose pulleys, basins, boilers, goggles, sequenced assembly).
//
// Fluids are identified by id (`fluids::FluidStack::fluid`): vanilla "minecraft:water",
// "minecraft:lava", "minecraft:milk", "minecraft:powder_snow" (built in); Create's
// ("levitate:honey", "levitate:chocolate", "levitate:tea" and the potion entry, registered by U19
// `fluids`); potions in two forms, "potion:<name>" with the Bedrock potion name
// ("potion:swiftness", "potion:long_swiftness", ...) and U19's "levitate:potion/<bottle>/<name>"
// (what its items produce). Both forms are potions here and share the "potion:" entry.
//
//     fluidtypes::registerFluid({.id = "levitate:honey", .nameKey = "fluid.create.honey",
//                                .nameFallback = "Honey", .sourceBlock = "levitate:honey",
//                                .color = 0xFFF8B533, .lavaInteraction = "levitate:limestone",
//                                .bucketItem = "levitate:honey_bucket",
//                                .bottleItem = "minecraft:honey_bottle"});
//
// Registering a fluid with a bucket / bottle item also registers that item with the U3 fluid
// capability (bucket <-> kBucket mB, bottle <-> kBottle mB).
//
// Open pipe ends apply a fluid's effect to the area in front of them (Create's
// OpenPipeEffectHandler): `registerEffect(id, fn)` (or a prefix ending in ':' such as "potion:").

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "feature/common/Capabilities.h"

class BlockSource;

namespace lv::feature::common::fluidtypes {

inline constexpr int kBucket = 1000; ///< mB in a bucket
inline constexpr int kBottle = 250;  ///< mB in a bottle (Create's PotionFluidHandler / honey bottle)

struct FluidType {
    std::string id;
    std::string nameKey;      ///< translation key of the fluid's name
    std::string nameFallback; ///< English name
    /// Block placed in the world for 1000 mB, also drained as 1000 mB when it is a source
    /// ("" = the fluid has no world block: open pipe ends consume it, as Create's virtual fluids).
    std::string sourceBlock;
    /// Further world blocks that count as this fluid when drained / met (flowing variants).
    std::vector<std::string> worldBlocks;
    std::uint32_t            color = 0xFFFFFFFF; ///< ARGB tint for renderers
    /// Texture variant in fluid renderers (glass pipes): 0 = generic, 1 water, 2 lava, 3 honey,
    /// 4 chocolate, 5 milk, 6 potion, 7 tea.
    int  renderSlot = 0;
    bool hot        = false; ///< lava-like (sets fire)
    /// Block made where this fluid meets lava (Create's `AllFluids.getLavaInteraction`), "" = none.
    std::string lavaInteraction;
    std::string bucketItem; ///< filled bucket (kBucket mB), "" = none
    std::string bottleItem; ///< filled bottle (kBottle mB), "" = none (potion bottles go by aux)
};

void registerFluid(FluidType type);
/// The registered fluid, the generic potion entry for potion ids (both forms), else nullptr.
[[nodiscard]] FluidType const* find(std::string_view id);
[[nodiscard]] std::vector<FluidType const*> all();
/// The fluid whose world block (source or flowing) is `blockId`, nullptr if none.
[[nodiscard]] FluidType const* byWorldBlock(std::string_view blockId);

/// The filled bucket of a fluid ("" when it has none; powder snow is no fluid for buckets).
[[nodiscard]] std::string bucketOf(std::string_view id);
/// Whether the fluid can be placed in the world as a block (water, lava, honey, chocolate).
[[nodiscard]] bool hasBlockState(std::string_view id);
/// Display name in `locale` ({} = the server's default): the registered name ("Honey"), potions
/// "Potion (long swiftness)", unknown ids made readable ("some_fluid" -> "Some Fluid").
[[nodiscard]] std::string displayName(std::string_view id, std::string_view locale = {});
/// ARGB tint of a fluid id (potions by their own colour; core::fluids::fluidColor, opaque).
[[nodiscard]] std::uint32_t colorOf(std::string_view id);

[[nodiscard]] bool        isPotion(std::string_view id);
/// "swiftness" for "potion:swiftness" and "levitate:potion/regular/swiftness" ("" for other ids).
[[nodiscard]] std::string potionName(std::string_view id);

/// The area an open pipe end affects (block coordinates, max exclusive), Create's
/// `OpenEndedPipe.aoe`.
struct Area {
    double minX, minY, minZ, maxX, maxY, maxZ;
};
using EffectHandler = std::function<void(BlockSource& region, Area const& area, fluids::FluidStack const& fluid, std::int64_t gameTime)>;
/// Registers the effect of fluid `id` (or every id starting with `id` when it ends with ':').
void registerEffect(std::string id, EffectHandler handler);
[[nodiscard]] EffectHandler const* effectFor(std::string_view id);

} // namespace lv::feature::common::fluidtypes
