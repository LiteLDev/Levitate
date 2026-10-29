// Generators (U7): player interaction through the shared dispatch (common/Interactions.h; network
// players and SimulatedPlayers alike) rather than through the LeviLamina player events.
//
// - Hand crank: right-click turns it (sneaking: backwards); sneaking with an item uses the item.
// - Valve handle: right-click turns it by its angle (sneaking: the other way), a dye of another
//   colour dyes it; the wrench on its value box opens the board (U3), sneak + wrench is the wrench's.
// - Water wheels: right-click with planks of another wood changes the wheel's wood.
// - Placement: the placed block is turned as Create's getStateForPlacement; the large water
//   wheel is placed by the unit (room for its 3x3 structure, one block further if needed).

#include <memory>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/common/Interactions.h"
#include "feature/common/ValueBox.h"
#include "feature/generators/Generators.h"
#include "game/Blocks.h"

namespace lv::feature::generators {

namespace {

InteractionResult handled() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

/// The client predicted the held block being placed next to the clicked one: send it the real block.
void resendBlock(Player& player, BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return;
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::UpdateBlock);
    if (!packet) return;
    auto& pkt        = static_cast<UpdateBlockPacket&>(*packet);
    pkt.mPos         = pos;
    pkt.mLayer       = 0;
    pkt.mUpdateFlags = 0b1011; // neighbours | network | priority
    pkt.mRuntimeId   = region.getBlock(pos).mNetworkId;
    pkt.sendTo(player);
}

BlockPos neighbour(BlockPos const& p, uchar face) {
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face > 5) return p;
    return BlockPos{p.x + dx[face], p.y + dy[face], p.z + dz[face]};
}

/// Where a plain right-click with a block item puts the block.
BlockPos placementPos(BlockSource& region, BlockPos const& clicked, uchar face) {
    return game::replaceable(region, clicked) ? clicked : neighbour(clicked, face);
}

/// Block-local hit position from the click position (relative or absolute).
core::common::Vec localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

/// Handles a click on one of the unit's blocks; nullopt: the game handles it.
std::optional<InteractionResult> useOnBlock(Player& player, ItemStack& item, BlockPos const& at, uchar face, Vec3 const& hit, Block const& clicked) {
    Kind kind = kindOf(clicked);
    if (kind == Kind::None || player.isSpectator()) return std::nullopt;
    std::string held     = item.isNull() ? "" : item.getTypeName();
    bool        sneaking = player.isSneaking();
    auto&       region   = player.getDimensionBlockSource();
    auto        pos      = game::globalPos(region, at);
    auto        finish   = [&] {
        if (!held.empty()) {
            resendBlock(player, region, neighbour(at, face));
            player.sendInventory(false);
        }
        return handled();
    };
    switch (kind) {
    case Kind::Crank:
        // Java skips the block's use for sneaking players holding an item
        if (sneaking && !held.empty()) return std::nullopt;
        useCrank(player, pos, sneaking, held != kExtendoGrip);
        return finish();
    case Kind::Valve: {
        if (sneaking && held == kWrench) return std::nullopt; // the wrench's own use (U8)
        if (!player.canUseAbility(AbilitiesIndex::Build)) return std::nullopt;
        if (held == kWrench && !sneaking) {
            // the wrench on the value box edits the angle (U3's value box board)
            if (common::valuebox::boxAt(clicked, face, localHit(at, hit))) return std::nullopt;
        }
        useValve(player, pos, held, sneaking);
        return finish();
    }
    case Kind::WaterWheel:
    case Kind::LargeWheel:
    case Kind::Structure:
        if (sneaking && !held.empty()) return std::nullopt;
        if (applyMaterial(pos, held)) return finish();
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace

namespace ia = common::interactions;

namespace {

ia::Result toResult(InteractionResult const& r) { return r.mSuccess ? ia::Result::Consume : ia::Result::Deny; }

/// Clicks on the unit's blocks, and the large water wheel's placement (done by the unit).
ia::Result useOnHandler(ia::UseOnContext const& c) {
    auto&    player = c.player;
    auto&    region = player.getDimensionBlockSource();
    auto&    item   = ia::mutableItem(c.item);
    BlockPos at     = game::blockPos(c.pos);
    auto     face   = static_cast<uchar>(c.face);
    Vec3     hit{float(c.hit[0]), float(c.hit[1]), float(c.hit[2])}; // block-local (localHit keeps it)
    if (auto r = useOnBlock(player, item, at, face, hit, c.block)) return toResult(*r);

    Kind placing = item.isNull() ? Kind::None : kindOf(c.itemId);
    if (placing != Kind::LargeWheel || face > 5) return ia::Result::Pass;
    if (!player.canUseAbility(AbilitiesIndex::Build)) return ia::Result::Pass;
    BlockPos target = placementPos(region, at, face);
    bool     placed = placeLargeWheel(player, item, game::globalPos(region, target), static_cast<Face>(face));
    if (!placed) {
        resendBlock(player, region, target);
        player.sendInventory(false);
        return ia::Result::Deny;
    }
    return ia::Result::Consume;
}

/// After the game placed one of the unit's blocks: turn it as Create's getStateForPlacement.
void placeHandler(ia::PlaceContext const& c) {
    if (c.face > 5) return;
    Kind placing = kindOf(c.itemId);
    if (placing == Kind::None || kindOf(c.block) != placing) return;
    orientPlaced(c.player, c.pos, static_cast<Face>(c.face));
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

} // namespace

namespace {
std::vector<ll::event::ListenerPtr>& listeners() {
    static std::vector<ll::event::ListenerPtr> l;
    return l;
}
} // namespace

namespace detail {

void installHooks() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, useOnHandler, ia::priority::High));
    // Create: on a valve handle only the wrench edits the angle box; any other click turns the
    // valve, so it goes before U3's value boxes (priority::Highest)
    ids.push_back(ia::onUseOn(
        {},
        [](ia::UseOnContext const& c) {
            if (kindOf(c.block) != Kind::Valve || c.itemId == kWrench) return ia::Result::Pass;
            return useOnHandler(c);
        },
        ia::priority::Highest + 1
    ));
    ids.push_back(ia::onPlace({}, placeHandler));
    ids.push_back(ia::onDestroy({}, [](ia::DestroyContext const& c) {
        if (c.player.isCreative()) noteCreativeBreak(c.pos);
        return ia::Result::Pass;
    }));
    // If the interaction event reaches U3's value boxes before GameMode: a click on a valve
    // handle with anything but the wrench turns it (Create's short click) instead of opening the board.
    auto& bus = ll::event::EventBus::getInstance();
    listeners().push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>(
        [](ll::event::PlayerInteractBlockEvent& ev) {
            auto block = ev.block();
            if (!block || kindOf(*block) != Kind::Valve) return;
            auto const& held = ev.item();
            suppressValveBox = held.isNull() || held.getTypeName() != kWrench;
        },
        ll::event::EventPriority::Highest
    ));
    listeners().push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>(
        [](ll::event::PlayerInteractBlockEvent&) { suppressValveBox = false; },
        ll::event::EventPriority::Lowest
    ));
}

void uninstallHooks() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& l : listeners()) bus.removeListener(l);
    listeners().clear();
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace detail

} // namespace lv::feature::generators
