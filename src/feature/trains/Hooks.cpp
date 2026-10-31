// Trains unit: player interaction. GameMode is where every player action ends up (network
// players and SimulatedPlayers alike); the player's movement input (train controls) comes from
// the PlayerAuthInputPacket every client sends each tick.

#include <cmath>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/network/NetEventCallback.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/net/Net.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace items = common::items;
namespace ia    = common::interactions;

namespace {

/// Bedrock repeats a held use: one action per 4 ticks per player.
bool debounced(Player& p, bool isFirst) {
    auto& last = state().lastUse[uuidOf(p)];
    auto  now  = state().tick;
    if (!isFirst && now - last < 4 && last != 0) return true;
    last = now;
    return false;
}

GlobalPos placementPos(BlockSource& region, BlockPos const& clicked, uchar face) {
    GlobalPos p = game::globalPos(region, clicked);
    if (game::replaceable(region, clicked)) return p;
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face < 6) p = {p.dim, p.x + dx[face], p.y + dy[face], p.z + dz[face]};
    return p;
}

/// The look direction snapped to the track axis (Create: the targeted direction).
std::pair<int, int> trackDirection(Player& p, int axis) {
    auto v = p.getViewVector();
    if (axis == 0) return {v.x >= 0 ? 1 : -1, 0};
    return {0, v.z >= 0 ? 1 : -1};
}

/// Right-click on a station block.
bool useStation(Player& p, GlobalPos const& pos, ItemStack const& item) {
    auto* s = stationAt(pos);
    if (!s) return false;
    (void)item;
    // Create's StationBlock.useItemOn: the held auto-schedule comes back to the player; otherwise
    // the screen opens (with a schedule in hand too: schedules go in by funnels, hoppers, arms or
    // thrown onto the station)
    if (!s->autoSchedule.is_null()) {
        auto back = takeAutoSchedule(*s);
        if (!p.add(back)) p.drop(back, false);
        p.sendInventory(false);
        playSound(pos.dim, {pos.x + .5, pos.y + .5, pos.z + .5}, "random.pop", 0.2f, 1.4f);
        return true;
    }
    openStationScreen(p, pos);
    return true;
}

bool useStationItemOnTrack(Player& p, GlobalPos const& track) {
    int axis = straightTrackAxis(track);
    auto* region = regionOf(track.dim);
    if (!region) return false;
    auto shape = tracks::shapeAt(*region, game::blockPos(track));
    if (!shape) return false;
    if (core::tracks::isJunction(*shape)) {
        actionBar(p, trFor(p, "create.track_target.no_junctions", "Targeted track cannot be an intersection"));
        return true;
    }
    // diagonal and sloped tracks: the axis nearest to the look
    if (axis < 0) {
        auto v = p.getViewVector();
        axis   = std::abs(v.x) >= std::abs(v.z) ? 0 : 2;
    }
    auto [fx, fz] = trackDirection(p, axis);
    state().targets[uuidOf(p)] = {track, fx, fz};
    actionBar(p, trFor(p, "create.track_target.set", "Targeted track selected"));
    log().info("trains: {} targeted the track at {} {} {}", p.getRealName(), track.x, track.y, track.z);
    return true;
}

/// Train controls placed: Create's facing (towards the player; sneaking: away).
void orientControls(Player& p, GlobalPos const& pos) {
    if (idAt(pos) != kControls) return;
    int look = game::playerFacing(p);        // 0 S, 1 W, 2 N, 3 E: where the player looks
    int face = p.isSneaking() ? look : game::opposite(look);
    static int const toDir[] = {3, 4, 2, 5}; // Java 2D -> levitate:facing (south, west, north, east)
    setBlockAt(pos, kControls, {{"levitate:facing", toDir[face & 3]}});
}

} // namespace

// ---- interactions (U36: on the shared dispatch) --------------------------------------------------

namespace {

std::vector<ia::HandlerId> gHandlers;

/// Right-clicks on stations, tracks, bogeys, and with schedules, station and controls items.
ia::Result trainsUseOn(ia::UseOnContext const& c) {
    Player&            player = c.player;
    auto&              region = player.getDimensionBlockSource();
    std::string const& tid    = c.blockId;
    std::string const& held   = c.itemId;
    ItemStack const&   item   = c.item;
    GlobalPos          pos    = c.pos;
    BlockPos           at{pos.x, pos.y, pos.z};
    bool               sneak        = c.sneaking;
    bool               isFirstEvent = c.firstEvent;

    // a train's controls in front of the clicked block
    {
        auto   eye = player.getPosition(); // eye height (getHeadPos drifts with the body yaw of real players)
        double hx = at.x + c.hit[0], hy = at.y + c.hit[1], hz = at.z + c.hit[2];
        double dist = std::sqrt((hx - eye.x) * (hx - eye.x) + (hy - eye.y) * (hy - eye.y) + (hz - eye.z) * (hz - eye.z));
        if (!sneak && held != kSchedule && tryControls(player, dist + 0.3)) return ia::Result::Consume;
    }
    if (tid == kStation && !sneak) {
        if (debounced(player, isFirstEvent)) return ia::Result::Consume;
        if (useStation(player, pos, item)) {
            player.sendInventory(false);
            return ia::Result::Consume;
        }
    }
    if (tid == tracks::kTrackId) {
        if (held == kStation && !sneak) {
            if (debounced(player, isFirstEvent)) return ia::Result::Consume;
            useStationItemOnTrack(player, pos);
            player.sendInventory(false);
            return ia::Result::Consume;
        }
        if ((held == kRailwayCasing || held.empty()) && !sneak) {
            if (debounced(player, isFirstEvent)) return ia::Result::Consume;
            auto msg = useCasingOnTrack(&player, pos, held == kRailwayCasing || player.isCreative());
            if (!msg.empty()) {
                if (msg.starts_with("+")) {
                    msg = msg.substr(1);
                    if (!player.isCreative()) {
                        ItemStack copy = item;
                        copy.remove(1);
                        player.getInventory().setItem(player.getSelectedItemSlot(), copy.mCount > 0 ? copy : ItemStack{});
                    }
                }
                if (msg != "!") actionBar(player, msg);
                player.sendInventory(false);
                return ia::Result::Consume;
            }
        }
    }
    if (isBogey(tid) && held == kWrench) {
        if (debounced(player, isFirstEvent)) return ia::Result::Consume;
        int axisState = intStateAt(pos, kBogeyAxisState).value_or(0);
        if (sneak) {
            // Create: dismantling gives the drop (train casing)
            game::clearBlock(region, at);
            if (!player.isCreative())
                if (auto casing = items::make(kRailwayCasing)) {
                    if (!player.add(*casing)) player.drop(*casing, false);
                }
        } else {
            setBlockAt(pos, tid == kSmallBogey ? kLargeBogey : kSmallBogey, {{kBogeyAxisState, axisState}});
            actionBar(player, trFor(player, "create.bogey.style.updated_style_and_size", "Updated style and size"));
        }
        player.sendInventory(false);
        return ia::Result::Consume;
    }
    if (held == kSchedule && tid != kStation) {
        if (debounced(player, isFirstEvent)) return ia::Result::Consume;
        openScheduleEditor(player, player.getSelectedItemSlot());
        return ia::Result::Consume;
    }
    if (held == kStation && tid != tracks::kTrackId) {
        auto it = state().targets.find(uuidOf(player));
        if (it == state().targets.end()) {
            actionBar(player, trFor(player, "create.track_target.missing", "Right-click the targeted train track first"));
            player.sendInventory(false);
            return ia::Result::Consume;
        }
        GlobalPos   place = placementPos(region, at, static_cast<uchar>(c.face));
        auto const& tg    = it->second;
        double      dx = place.x - tg.track.x, dy = place.y - tg.track.y, dz = place.z - tg.track.z;
        if (tg.track.dim != place.dim || dx * dx + dy * dy + dz * dz > 16 * 16) {
            actionBar(player, trFor(player, "create.track_target.too_far", "Targeted track is too far from here"));
            player.sendInventory(false);
            return ia::Result::Consume;
        }
        state().pendingStations[place] = {tg, uuidOf(player)}; // bound once vanilla placed it
    }
    return ia::Result::Pass;
}

/// Right-clicks in the air: sneak-use of a station clears the selection, controls, schedules.
ia::Result trainsUse(ia::UseContext const& c) {
    if (c.onBlock) return ia::Result::Pass; // the use-on handler saw this click
    Player&            player = c.player;
    std::string const& held   = c.itemId;
    if (held == kStation && c.sneaking) {
        if (state().targets.erase(uuidOf(player)))
            actionBar(player, trFor(player, "create.track_target.clear", "Cleared track selection"));
        return ia::Result::Consume;
    }
    if (!c.sneaking && held != kSchedule && tryControls(player)) return ia::Result::Consume;
    if (held == kSchedule) {
        if (debounced(player, true)) return ia::Result::Consume;
        openScheduleEditor(player, player.getSelectedItemSlot());
        return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

/// A schedule handed to a conductor (an entity sitting behind a train's controls).
ia::Result trainsInteract(ia::EntityContext const& c) {
    Player& player = c.player;
    Actor&  entity = c.entity;
    if (entity.isPlayer()) return ia::Result::Pass;
    // Create's ScheduleItemEntityInteraction: only an entity seated on a conductor seat
    if (auto* t = conductorTrainOf(entity))
        if (handScheduleTo(player, *t, player.getSelectedItemSlot())) return ia::Result::Consume;
    if (c.itemId == kSchedule && seatedTrainOf(entity) && !conductorTrainOf(entity)) {
        // a schedule on a seated entity that isn't a conductor (Create's ScheduleItem.handScheduleTo)
        actionBar(player, trFor(player, "create.schedule.non_controlling_seat", "Conductor needs to sit in front of a Controls block"));
        return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TrainsAuthInputHook,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&     source,
    PlayerAuthInputPacket const& packet
) {
    origin(source, packet);
    auto* player = thisFor<NetEventCallback>()->_getServerPlayer(source, packet.mSenderSubId);
    if (!player) return;
    // the client mod sends Create's controls input on "trains.interact" (its key capture keeps the
    // movement keys out of this packet); until its first key message the packet still drives
    if (net::isCompanion(*player) && drivesByChannel(*player)) return;
    auto uuid = uuidOf(*player);
    bool controlling = false;
    for (auto const& [id, t] : state().trains) controlling = controlling || t.controller == uuid;
    if (!controlling) return;
    using Input       = PlayerAuthInputPacketPayload::InputData;
    auto const& in    = *packet.mInputData;
    Vec2        move  = *packet.mMove;
    int         keys  = move.y > 0.3f ? 1 : move.y < -0.3f ? -1 : 0;
    int         steer = move.x > 0.3f ? 1 : move.x < -0.3f ? -1 : 0; // +x: left (A)
    bool        jump  = in.contains(Input::JumpCurrentRaw) || in.contains(Input::Jumping) || in.contains(Input::JumpDown);
    bool        sneak = in.contains(Input::SneakDown) || in.contains(Input::StartSneaking);
    setInput(*player, keys, steer, jump, sneak);
    // Create's ControlsHandler / HonkPacket: the sprint key sounds the horn while held (vanilla
    // clients send it as their sprint input)
    // (as the client mod: {honk: true} every 5 ticks while held, {honk: false} on release)
    static std::map<std::string, std::pair<bool, std::uint64_t>> sprinting;
    bool sprint = in.contains(Input::SprintDown) || in.contains(Input::Sprinting) || in.contains(Input::StartSprinting);
    if (in.contains(Input::StopSprinting)) sprint = false;
    auto& [was, sent] = sprinting[uuid];
    if (sprint && (!was || state().tick - sent >= 5)) {
        setHonk(*player, true);
        sent = state().tick;
    } else if (!sprint && was) {
        setHonk(*player, false);
    }
    was = sprint;
}

void installHooks() {
    gHandlers.push_back(ia::onUseOn({}, trainsUseOn));
    // a placed station leaves the track selection behind
    gHandlers.push_back(ia::onPlace({std::string(kStation), std::string(kStation)}, [](ia::PlaceContext const& c) {
        state().targets.erase(uuidOf(c.player));
    }));
    gHandlers.push_back(ia::onPlace({std::string(kControls), std::string(kControls)}, [](ia::PlaceContext const& c) {
        orientControls(c.player, c.pos);
    }));
    gHandlers.push_back(ia::onUse({}, trainsUse));
    gHandlers.push_back(ia::onInteract({}, trainsInteract));
    TrainsAuthInputHook::hook();
    // companion clients (Create's controls packets):
    // "trains.interact" {} right-click with an empty hand in the air (toggles the controls looked
    // at), {pressed: [6 bools]} the held keys while controlling, {stop: true} Esc;
    // "trains.throttle" {throttle} the HUD's mouse wheel, {honk} the sprint key (horn)
    net::on("trains.interact", [](Player& p, net::Json const& msg) {
        if (msg.contains("pressed")) controlsInput(p, msg["pressed"]);
        else if (msg.value("stop", false)) controlsStop(p);
        else tryControls(p);
    });
    net::on("trains.throttle", [](Player& p, net::Json const& msg) {
        if (msg.contains("throttle") && msg["throttle"].is_number()) setThrottle(p, msg["throttle"].get<double>());
        if (msg.contains("honk") && msg["honk"].is_boolean()) setHonk(p, msg["honk"].get<bool>());
    });
}

void uninstallHooks() {
    net::off("trains.throttle");
    net::off("trains.interact");
    TrainsAuthInputHook::unhook();
    for (auto id : gHandlers) ia::remove(id);
    gHandlers.clear();
}

} // namespace lv::feature::trains
