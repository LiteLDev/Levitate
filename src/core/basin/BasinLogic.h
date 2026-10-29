#pragma once

// Basin processing (U14, SPEC §3.6.1): the numbers and state machines of Create 6.0.10's basin,
// mechanical press, mechanical mixer and blaze burner, independent of the game (gtest:
// tests/core/BasinTest.cpp).

#include <string>
#include <vector>

#include "core/common/Recipe.h"

namespace lv::core::basin {

using common::HeatLevel;

// ---- blaze burner (Create's BlazeBurnerBlockEntity) ----------------------------------------------

/// Create's FuelType (ordinal order matters: special fuel can't be replaced by normal fuel).
enum class Fuel : int { None = 0, Normal = 1, Special = 2 };

inline constexpr int kMaxHeatCapacity   = 10000; ///< MAX_HEAT_CAPACITY (ticks)
inline constexpr int kInsertionThreshold = 500;  ///< INSERTION_THRESHOLD (ticks)
inline constexpr int kSpecialBurnTime   = 3200;  ///< blaze cake (superheated fuel)
inline constexpr int kRegularTagBurnTime = 1600; ///< `#create:blaze_burner_fuel/regular` items

struct BurnerState {
    Fuel fuel     = Fuel::None;
    int  burnTime = 0; ///< remaining ticks
    bool creative = false;
    friend bool operator==(BurnerState const&, BurnerState const&) = default;
};

/// What an item offers as fuel: Fuel::None when it is no fuel.
struct FuelOffer {
    Fuel fuel     = Fuel::None;
    int  burnTime = 0;
};

/// Create's `getHeatLevel()`: seething on special fuel, kindled on normal fuel (fading under
/// 1.25 % of the capacity, i.e. < 125 ticks left), smouldering without fuel.
[[nodiscard]] HeatLevel burnerHeat(BurnerState const& s);
/// Create's `tryUpdateFuel`: whether the fuel is accepted (and, unless `simulate`, applied).
/// `forceOverflow` is true for players (refill above the threshold up to the capacity).
bool tryUpdateFuel(BurnerState& s, FuelOffer offer, bool forceOverflow, bool simulate);
/// One server tick of a burner (not creative): burn time runs down; special fuel then turns
/// into half a capacity of normal fuel; without fuel the burner goes back to smouldering.
void tickBurner(BurnerState& s);
/// Create's `HeatLevel.nextActiveLevel()` (none -> smouldering -> fading -> kindled -> seething
/// -> smouldering).
[[nodiscard]] HeatLevel nextActiveLevel(HeatLevel h);
/// The level a creative blaze cake switches to (fading is skipped).
[[nodiscard]] HeatLevel creativeNext(HeatLevel current);
/// Comparator output of a blaze burner (Create: heat ordinal - 1, at least 0).
[[nodiscard]] int burnerComparator(HeatLevel h);

// ---- mechanical press (Create's PressingBehaviour) ----------------------------------------------

inline constexpr int kPressCycle      = 240; ///< CYCLE
inline constexpr int kPressEntityScan = 10;  ///< ENTITY_SCAN (ticks between item entity scans)

enum class PressMode : int { World = 0, Belt = 1, Basin = 2 };
/// How far the head travels in each mode (blocks): world 1, belt 19/16, basin 22/16.
[[nodiscard]] float pressModeHeadOffset(PressMode m);
/// Running ticks added per tick at `speed` RPM: lerp(clamp(|speed| / 512), 1, 60), 0 when stopped.
[[nodiscard]] int pressRunningTickSpeed(float speed);
/// Create's `getRenderedHeadOffset` (0..1 of the mode's travel).
[[nodiscard]] float pressHeadOffset(bool running, int prevRunningTicks, int runningTicks, float partialTicks);

// ---- mechanical mixer (Create's MechanicalMixerBlockEntity) -------------------------------------

/// Ticks of one mixing step: max(floor(log2(int(512 / |speed|))) * ceil(recipeSpeed * 15) + 1, 1)
/// where recipeSpeed = processing_time / 100 (1 when the recipe has none).
[[nodiscard]] int mixerProcessingTicks(float speed, int processingTime);
/// Create's `getRenderedHeadOffset` in blocks below the rest position (7/16 at rest).
[[nodiscard]] float mixerHeadOffset(bool running, int runningTicks, float partialTicks);
/// Create's `getRenderedHeadRotationSpeed` (RPM of the whisk).
[[nodiscard]] float mixerHeadSpeed(float speed, bool running, int runningTicks);

// ---- basin tanks (Create's SmartFluidTankBehaviour, 2 tanks x 1000 mB, enforced variety) ----------

inline constexpr int kTankCapacity = 1000;
inline constexpr int kTankCount    = 2;
inline constexpr int kSlotCount    = 9;

struct Fluid {
    std::string id;
    int         amount = 0;
    [[nodiscard]] bool empty() const { return id.empty() || amount <= 0; }
    friend bool        operator==(Fluid const&, Fluid const&) = default;
};

/// Fills `tanks` with up to `stack.amount` of `stack.id`; returns the accepted amount. Create's
/// combined tanks with enforced variety: a fluid goes into the tank already holding it, else into
/// the first empty tank when no tank holds it.
int fillTanks(std::vector<Fluid>& tanks, Fluid const& stack, int capacity, bool simulate);
/// Drains up to `amount` of `id` ("" = the first non-empty tank's fluid); returns what was drained.
Fluid drainTanks(std::vector<Fluid>& tanks, std::string const& id, int amount, bool simulate);

// ---- automated crafting (Create's MechanicalPressBlockEntity.canCompress) ----------------------

/// Whether a crafting recipe compresses: 4 or 9 ingredients, all the same (each ingredient is
/// given as a sorted key of the items it accepts; an empty key is an empty slot).
[[nodiscard]] bool canCompress(std::vector<std::string> const& ingredientKeys);
/// Whether a recipe id is excluded from automation (Create: the id ends with "_manual_only").
[[nodiscard]] bool ignoredInAutomation(std::string const& recipeId);

/// Comparator output of an inventory (Create's ItemHelper.calcRedstoneFromInventory):
/// `counts[i] / maxStack[i]` summed over the slots, 0 when empty, else 1 + floor(fill * 14).
[[nodiscard]] int redstoneFromInventory(std::vector<int> const& counts, std::vector<int> const& maxStacks);

} // namespace lv::core::basin
