#pragma once

// Wrench contract (U8, docs/SPEC.md §3.6.8): Create's IWrenchable for Levitate blocks.
//
// Right-click with the wrench calls the block's `onWrenched` (default: Create's rotation of
// `levitate:facing` / `levitate:axis` / `levitate:horizontal_facing` / `levitate:horizontal_axis` /
// `levitate:axis_along_first` about the clicked face, core/goggles/Rotation.h), sneak + right-click
// calls `onSneakWrenched` (default: the block is picked up: its item goes to the player's inventory
// in survival, the block is removed with Create's "component breaks" sound).
//
// Every kinetic block (feature/kinetics/Kinetics.h) is wrenchable with the defaults, as every
// Create IRotate block is. Other blocks opt in:
//
//     #include "feature/goggles/Wrenchable.h"
//     namespace wrench = lv::feature::goggles::wrench;
//     LV_REGISTER_WRENCHABLE("levitate:sticker");                         // defaults
//     LV_REGISTER_WRENCHABLE("levitate:mechanical_bearing", {             // Create's BearingBlock
//         .onWrenched = [](wrench::Context& c) {
//             auto r = wrench::defaultOnWrenched(c);
//             if (r == wrench::Result::Success) disassembleBearing(c.pos);
//             return r;
//         },
//     });
//
// Server thread only. Handlers run inside the use-item call of the player (SimulatedPlayers
// included); block changes made there are kept.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/GlobalPos.h"
#include "mod/Registry.h"

class Actor;
class Block;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::goggles::wrench {

inline constexpr char const* kWrenchId = "levitate:wrench";

/// A wrench click on a block (Create's UseOnContext).
struct Context {
    Player&         player;
    BlockSource&    region;
    core::GlobalPos pos;
    Block const&    block;  ///< the block clicked
    int             face;   ///< clicked face (0 down, 1 up, 2 north, 3 south, 4 west, 5 east)
    double          hitX = 0.5, hitY = 0.5, hitZ = 0.5; ///< block-local click position
};

/// Create's InteractionResult: `Pass` lets the game handle the click normally.
enum class Result : std::uint8_t { Pass, Success };

struct Handler {
    /// Right-click (Create's onWrenched). Empty: `defaultOnWrenched`.
    std::function<Result(Context&)> onWrenched;
    /// Sneak + right-click (Create's onSneakWrenched). Empty: `defaultOnSneakWrenched`.
    std::function<Result(Context&)> onSneakWrenched;
    /// The state after a click on `face` (Create's getRotatedBlockState). Empty: `rotatedBlock`.
    std::function<Block const*(Block const& block, int face)> rotate;
};

/// Declares `blockId` wrenchable (replaces an earlier registration).
void registerBlock(std::string blockId, Handler handler = {});
/// The handler of a block id: the registered one, a default one for kinetic blocks, else nullptr.
[[nodiscard]] Handler const* handlerOf(std::string_view blockId);
[[nodiscard]] bool           isWrenchable(std::string_view blockId);

/// Create's IWrenchable.getRotatedBlockState on Levitate's state conventions (the same block
/// when nothing changes).
[[nodiscard]] Block const& rotatedBlock(Block const& block, int face);
/// Create's default onWrenched: rotates (with the block's `rotate`), sets the block (the kinetic
/// network re-attaches it) and plays the rotate sound when it changed. Always Success.
Result defaultOnWrenched(Context& ctx);
/// Create's default onSneakWrenched: survival players get the block's item (inventory, else
/// dropped at their feet), the block is removed without drops and the remove sound plays.
/// A contraption anchored at the block is disassembled first.
Result defaultOnSneakWrenched(Context& ctx);

/// Create's `#create:wrench_pickup` tag: vanilla blocks sneak-wrenching picks up (rails,
/// redstone wire, torches, repeaters, comparators, levers, buttons, pressure plates, observers,
/// pistons, tripwire, tripwire hooks, daylight detectors, targets, hoppers).
[[nodiscard]] bool canWrenchPickup(std::string_view blockId);

/// Plays Create's wrench sounds at a block ("levitate.wrench_rotate" / "levitate.wrench_remove").
void playRotateSound(core::GlobalPos const& pos);
void playRemoveSound(core::GlobalPos const& pos);

/// Right-click on an entity with the wrench (Create: minecart contraption pickup, train
/// relocation). Handlers are tried in registration order until one returns true.
using EntityHandler = std::function<bool(Player& player, Actor& target, bool sneaking)>;
void registerEntityHandler(EntityHandler handler);

namespace detail {
struct Registrar {
    explicit Registrar(std::string id, Handler h = {}) { registerBlock(std::move(id), std::move(h)); }
};
} // namespace detail

} // namespace lv::feature::goggles::wrench

/// Registers a wrenchable block at static init: `LV_REGISTER_WRENCHABLE("levitate:x"[, handler]);`
#define LV_REGISTER_WRENCHABLE(...)                                                                                    \
    static ::lv::feature::goggles::wrench::detail::Registrar LV_UNIQUE(lv_wrenchable_)(__VA_ARGS__)
