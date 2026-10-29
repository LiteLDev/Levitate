// Signals unit (U28): item use (Create's TrackTargetingBlockItem, the wrench on a signal).
//
// Clicks come from the shared interaction dispatch (network players and SimulatedPlayers alike).

#include <cmath>
#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/signals/SignalsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::signals {

namespace {

/// Create's TrackTargetingBlockItem: the block must be placed closer than 16 blocks to the track.
constexpr double kMaxDistance = 16;

void deny(Player& player, std::string const& key, std::string const& fallback) {
    actionbar(player, key, fallback, 'c');
    playSound(player, "levitate.tracks.deny", 1, 1);
    log().info("signals: {} was refused: {}", player.getRealName(), key);
}

} // namespace

bool onTargetingItemUse(Player& player, std::string const& item, BlockPos const& clicked, int face) {
    auto&       s    = state();
    std::string uuid = player.getUuid().asString();
    Kind        kind = item == api::kSignalId ? Kind::Signal : Kind::Observer;
    auto        sel  = s.selections.find(uuid);
    if (sel != s.selections.end() && sel->second.item != item) {
        s.selections.erase(sel); // a selection belongs to the item it was made with
        sel = s.selections.end();
    }

    // sneak-click with a selection clears it
    if (player.isSneaking() && sel != s.selections.end()) {
        s.selections.erase(sel);
        actionbar(player, "create.track_target.clear", "Cleared track selection", 'f');
        playSound(player, "random.click", 1, .5f);
        log().info("signals: {} cleared the track selection", player.getRealName());
        return true;
    }

    // clicking a track selects it
    if (auto target = targetOfClick(player, clicked)) {
        switch (check(*target, kind)) {
        case Overlap::Junction:
            deny(player, "create.track_target.no_junctions", "Targeted track cannot be an intersection");
            return true;
        case Overlap::Occupied:
            deny(player, "create.track_target.occupied", "Targeted track is occupied");
            return true;
        case Overlap::NoTrack:
            deny(player, "create.track_target.invalid", "Cannot target this track here");
            return true;
        case Overlap::Valid: break;
        }
        s.selections[uuid] = Selection{*target, item};
        actionbar(player, "create.track_target.set", "Targeted track selected", 'a');
        playSound(player, "random.click", 1, 1);
        log().info(
            "signals: {} selected the track at {},{},{}",
            player.getRealName(),
            target->track.x,
            target->track.y,
            target->track.z
        );
        return true;
    }

    if (sel == s.selections.end()) {
        deny(player, "create.track_target.missing", "Right-click the targeted train track first");
        return true;
    }

    // placing: where the block goes
    auto&    region = player.getDimensionBlockSource();
    BlockPos placed = clicked;
    if (!game::replaceable(region, clicked)) {
        static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
        if (face >= 0 && face < 6) placed = BlockPos{clicked.x + dx[face], clicked.y + dy[face], clicked.z + dz[face]};
    }
    auto const& t  = sel->second.target;
    double      ex = placed.x - t.track.x, ey = placed.y - t.track.y, ez = placed.z - t.track.z;
    if (t.dim != int(player.getDimensionId()) || std::sqrt(ex * ex + ey * ey + ez * ez) >= kMaxDistance) {
        deny(player, "create.track_target.too_far", "Targeted track is too far from here");
        return true;
    }
    s.intents[game::globalPos(region, placed)] = {uuid, sel->second};
    return false; // vanilla places the block; SignalsModule binds it (and clears the selection)
}

} // namespace lv::feature::signals

namespace lv::feature::signals {

namespace ia = common::interactions;

namespace {
std::vector<ia::HandlerId> gHandlers;
} // namespace

namespace wrench = goggles::wrench;

// Create's SignalBlock.onWrenched (U8's wrench contract; sneak-wrenching picks the block up)
LV_REGISTER_WRENCHABLE(
    std::string(api::kSignalId),
    {.onWrenched = [](wrench::Context& c) {
        if (cycleType(c.pos)) {
            auto const& rec = state().blocks.at(c.pos);
            if (rec.type == int(cs::SignalType::Cross))
                actionbar(c.player, "create.track_signal.mode_change.cross_signal", "-> Allow passage if section fully traversable", 'f');
            else actionbar(c.player, "create.track_signal.mode_change.entry_signal", "-> Allow passage if section unoccupied", 'f');
        } else {
            actionbar(c.player, "create.track_signal.cannot_change_mode", "Unable to switch mode of this Signal", 'f');
        }
        return wrench::Result::Success;
    }}
);
// Create's TrackObserverBlock is IWrenchable with the defaults (nothing to rotate; sneak: pick up)
LV_REGISTER_WRENCHABLE(std::string{api::kObserverId});

// U36: the targeting items go through the shared interaction dispatch
void installHooks() {
    for (std::string item : {std::string(api::kSignalId), std::string(api::kObserverId)}) {
        gHandlers.push_back(ia::onUseOn({{}, item},[](ia::UseOnContext const& c) {
            BlockPos at{c.pos.x, c.pos.y, c.pos.z};
            if (!onTargetingItemUse(c.player, c.itemId, at, c.face)) return ia::Result::Pass;
            c.player.sendInventory(false);
            return ia::Result::Consume;
        }));
    }
}

void uninstallHooks() {
    for (auto id : gHandlers) ia::remove(id);
    gHandlers.clear();
}

} // namespace lv::feature::signals
