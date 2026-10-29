#pragma once

// Shared, additive dispatch of player interactions (U32, SPEC §3.2).
//
// Every unit that reacts to right-clicks, item uses or block breaking registers a handler here
// instead of hooking GameMode itself (with separate hooks the first hook wins and the others never
// see the click). One hook per GameMode action calls the matching handlers by priority (higher
// first, then registration order) until one returns something other than `Pass`.
//
//     namespace ia = lv::feature::common::interactions;
//     ia::onUseOn({.block = "levitate:chassis"}, [](ia::UseOnContext const& c) {
//         if (!c.sneaking) return ia::Result::Pass;
//         ...;
//         return ia::Result::Consume;
//     });
//     ia::onWrench("levitate:belt", [](ia::UseOnContext const& c) { ...; return ia::Result::Consume; });
//     ia::onUse("levitate:filter", [](ia::UseContext const& c) { ...; return ia::Result::Consume; });
//
// - Server thread only; called for server players (network players and SimulatedPlayers alike),
//   never for the client's own prediction.
// - `Consume`: handled; vanilla does not run (no placement, no item use), the arm swings and the
//   player's inventory is resent (it undoes the client's prediction). `Deny`: like Consume
//   without the success (refused action). `Pass`: the next handler, then vanilla.
// - Bedrock repeats use-on while the button is held (`firstEvent == false` for the repeats):
//   handlers that act once per click return `Consume` for the repeats too, without acting.
// - Handlers may edit the world directly (unlike a cancelled PlayerInteractBlockEvent, nothing
//   is rolled back), but must not keep `Player&` past the call.
// - Wrench: `onWrench(blockId, fn)` is `onUseOn({blockId, kWrenchId}, fn)`; Create's
//   IWrenchable#onWrenched / onSneakWrenched are the handler with `sneaking` false / true. The
//   generic wrench behaviour (rotate / pick up, U8) registers for any block (`""`) with
//   `priority::Fallback`; value boxes (U3) open before it with `priority::Highest`.
//
// Additions (U36): every unit goes through this dispatch, none hooks GameMode itself.
// - `onPlace`: after vanilla placed a block from a use-on that no handler took (orienting the
//   new block, Create's getStateForPlacement / onPlace with the placer).
// - `onStartDestroy`: the first hit on a block (GameMode::startDestroyBlock): `Consume` / `Deny`
//   stop the mining (left-click actions, e.g. deselecting an arm point, punching a placard).
// - `onInteract` / `onAttack`: right-click / left-click on an entity (GameMode::interact /
//   GameMode::attack); `Match::block` is the entity's type id there.
// - `nextTick(fn)`: runs `fn` on the next server tick (world edits that must not happen inside
//   the click, e.g. assembling a contraption).
// - `UseContext::onBlock`: the item use comes from a use-on that no block took (Bedrock calls the
//   item use from there too); units that only want right-clicks in the air return `Pass` then.
// - `onUnusedUseOn`: after vanilla's use-on did not succeed (no block took the click and nothing
//   was placed): Create's item useOn that runs only when the block passes (buckets of custom
//   fluids, putting a package down). `Consume` / `Deny` make the click a handled one.
// - `onDestroyed` / `onAttacked`: after vanilla's destroyBlock / attack ran, with its result
//   (forgetting a removed block's data, knockback of a hit).
// - `mutableItem(ctx.item)`: the held stack is the player's hand slot; handlers that change it in
//   place (counts, item data) use this instead of a copy.

#include <functional>
#include <string>

#include "core/GlobalPos.h"
#include "core/common/ValueSettings.h"

class Actor;
class Block;
class ItemStack;
class Player;

namespace lv::feature::common::interactions {

using core::GlobalPos;
using core::common::Vec;

inline constexpr char const* kWrenchId = "levitate:wrench";

enum class Result { Pass, Consume, Deny };

namespace priority {
inline constexpr int Highest  = 1000; ///< value boxes and filter slots (U3)
inline constexpr int High     = 100;
inline constexpr int Normal   = 0;
inline constexpr int Low      = -100;
inline constexpr int Fallback = -1000; ///< generic behaviour (e.g. the wrench's rotate / pick up)
} // namespace priority

/// Which interactions a handler wants; an empty id matches anything ("" item = also an empty hand).
struct Match {
    std::string block; ///< block id ("levitate:chassis")
    std::string item;  ///< held item id ("levitate:wrench"); use `emptyHand` for an empty hand only
    bool        emptyHand = false; ///< only when the hand is empty
};

/// Right-click on a block (GameMode::useItemOn).
struct UseOnContext {
    Player&            player;
    ItemStack const&   item;   ///< the held stack (may be empty)
    std::string const& itemId; ///< "" for an empty hand
    Block const&       block;
    std::string const& blockId;
    GlobalPos          pos;
    int                face; ///< FacingID: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
    Vec                hit;  ///< block-local click position (0..1)
    bool               sneaking;
    bool               firstEvent; ///< false for the repeats while the button is held
};

/// Using an item (GameMode::baseUseItem): a right-click in the air, or on a block that did not
/// take the click (vanilla use-on passed).
struct UseContext {
    Player&            player;
    ItemStack const&   item;
    std::string const& itemId;
    bool               sneaking;
    bool               onBlock = false; ///< (U36) called from a use-on that no block took
};

/// A player breaks a block (GameMode::destroyBlock). `Consume` / `Deny` keep the block.
struct DestroyContext {
    Player&            player;
    ItemStack const&   item;
    std::string const& itemId;
    Block const&       block;
    std::string const& blockId;
    GlobalPos          pos;
    int                face;
    bool               sneaking;
};

/// A block placed by vanilla from a use-on that every handler passed (U36).
struct PlaceContext {
    Player&            player;
    std::string const& itemId;  ///< the held item before the placement used it
    Block const&       block;   ///< the placed block
    std::string const& blockId; ///< its id
    GlobalPos          pos;     ///< where it was placed
    GlobalPos          clicked; ///< the clicked block (== pos when that was replaceable)
    int                face;    ///< the clicked face
    Vec                hit;     ///< click position, local to the clicked block (0..1)
    bool               sneaking;
    ItemStack const*   itemBefore = nullptr; ///< a copy of the held stack before the placement (data, count)
};

/// Right- / left-click on an entity (U36).
struct EntityContext {
    Player&            player;
    ItemStack const&   item;
    std::string const& itemId;
    Actor&             entity;
    std::string const& entityId; ///< the entity's type id ("minecraft:minecart")
    Vec                at;       ///< world position of the click
    bool               sneaking;
};

using UseOnHandler   = std::function<Result(UseOnContext const&)>;
using UseHandler     = std::function<Result(UseContext const&)>;
using DestroyHandler = std::function<Result(DestroyContext const&)>;
using PlaceHandler   = std::function<void(PlaceContext const&)>;
using EntityHandler  = std::function<Result(EntityContext const&)>;
using DestroyedHandler = std::function<void(DestroyContext const&, bool destroyed)>;
using AttackedHandler  = std::function<void(EntityContext const&, bool hit)>;
using HandlerId      = int;

/// The held stack of a context as the mutable hand slot it is (use-on, use, destroy, entity).
inline ItemStack& mutableItem(ItemStack const& contextItem) { return const_cast<ItemStack&>(contextItem); }

HandlerId onUseOn(Match match, UseOnHandler fn, int priority = priority::Normal);
/// `itemId` "" = any item (an empty hand never uses an item in the air).
HandlerId onUse(std::string itemId, UseHandler fn, int priority = priority::Normal);
HandlerId onDestroy(Match match, DestroyHandler fn, int priority = priority::Normal);
/// Wrench right-click on `blockId` ("" = any block).
HandlerId onWrench(std::string blockId, UseOnHandler fn, int priority = priority::Normal);
/// (U36) After vanilla placed a block: `match.block` = the placed block, `match.item` = the held item.
HandlerId onPlace(Match match, PlaceHandler fn, int priority = priority::Normal);
/// (U36) First hit on a block (left-click); `Consume` / `Deny` stop the mining.
HandlerId onStartDestroy(Match match, DestroyHandler fn, int priority = priority::Normal);
/// (U36) Right-click on an entity; `match.block` = entity type id ("" = any).
HandlerId onInteract(Match match, EntityHandler fn, int priority = priority::Normal);
/// (U36) Attacking an entity; `Consume` = the attack is replaced, `Deny` = no attack.
HandlerId onAttack(Match match, EntityHandler fn, int priority = priority::Normal);
/// (U36) After vanilla's use-on did not succeed (and no handler took it): `firstEvent` etc. as
/// for onUseOn; `Consume` / `Deny` turn the click into a handled / refused one.
HandlerId onUnusedUseOn(Match match, UseOnHandler fn, int priority = priority::Normal);
/// (U36) After vanilla's destroyBlock ran (also when a handler did not stop it): `destroyed` is
/// its result. `ctx.block` / `ctx.blockId` are the block before the break.
HandlerId onDestroyed(Match match, DestroyedHandler fn, int priority = priority::Normal);
/// (U36) After vanilla's attack ran; `hit` is its result. `match.block` = entity type id.
HandlerId onAttacked(Match match, AttackedHandler fn, int priority = priority::Normal);
/// (U36) Runs `fn` on the next server tick (server thread).
void nextTick(std::function<void()> fn);
/// (U36) A player of the server level (not the client's own predicting LocalPlayer).
[[nodiscard]] bool isServerPlayer(Player& player);
/// Removes a handler (selftests, uninstall).
void remove(HandlerId id);

/// Runs the use-on handlers as the hook does (selftests, other entry points). Returns the result
/// of the handler that took it, `Pass` if none did.
Result dispatchUseOn(UseOnContext const& ctx);
Result dispatchUse(UseContext const& ctx);
Result dispatchDestroy(DestroyContext const& ctx);
void   dispatchPlace(PlaceContext const& ctx);
Result dispatchStartDestroy(DestroyContext const& ctx);
Result dispatchInteract(EntityContext const& ctx);
Result dispatchAttack(EntityContext const& ctx);

} // namespace lv::feature::common::interactions
