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

InteractionResult handled() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

/// The look direction snapped to the track axis (Create: the targeted direction).
std::pair<int, int> trackDirection(Player& p, int axis) {
    auto v = p.getViewVector();
    if (axis == 0) return {v.x >= 0 ? 1 : -1, 0};
    return {0, v.z >= 0 ? 1 : -1};
}

/// Right-click on a station block.
bool useStation(Player& p, GlobalPos const& pos, ItemStack& item) {
    auto* s = stationAt(pos);
    if (!s) return false;
    if (auto sch = scheduleOf(item)) {
        // Create: a schedule put into the station becomes its auto-schedule
        if (s->autoSchedule.is_null()) {
            s->autoSchedule = items::toJson(item);
            markStationsDirty();
            if (!p.isCreative()) {
                p.getInventory().setItem(p.getSelectedItemSlot(), ItemStack{});
                p.sendInventory(false);
            }
            playSound(pos.dim, {pos.x + .5, pos.y + .5, pos.z + .5}, "random.levelup", 0.5f, 1.5f);
            log().info("trains: station '{}' holds an auto-schedule", s->name);
            return true;
        }
    }
    if (!s->autoSchedule.is_null()) {
        auto back = items::fromJson(s->autoSchedule);
        s->autoSchedule = nullptr;
        markStationsDirty();
        if (!p.add(back)) p.drop(back, false);
        p.sendInventory(false);
        log().info("trains: auto-schedule taken from station '{}'", s->name);
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

LL_TYPE_INSTANCE_HOOK(
    TrainsUseItemOnHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItemOn,
    InteractionResult,
    ItemStack&      item,
    BlockPos const& at,
    uchar           face,
    Vec3 const&     hit,
    HandSlot        handSlot,
    Block const*    targetBlock,
    bool            isFirstEvent
) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&        region = player.getDimensionBlockSource();
    Block const& target = targetBlock ? *targetBlock : region.getBlock(at);
    std::string  tid    = target.getTypeName();
    std::string  held   = item.isNull() ? std::string() : std::string(item.getTypeName());
    GlobalPos    pos    = game::globalPos(region, at);
    bool         sneak  = player.isSneaking();

    // a train's controls in front of the clicked block
    {
        auto   eye  = player.getHeadPos();
        double dist = std::sqrt(double((hit.x - eye.x) * (hit.x - eye.x) + (hit.y - eye.y) * (hit.y - eye.y) + (hit.z - eye.z) * (hit.z - eye.z)));
        if (!sneak && held != kSchedule && tryControls(player, dist + 0.3)) return handled();
    }
    if (tid == kStation && !sneak) {
        if (debounced(player, isFirstEvent)) return handled();
        if (useStation(player, pos, item)) {
            player.sendInventory(false);
            return handled();
        }
    }
    if (tid == tracks::kTrackId) {
        if (held == kStation && !sneak) {
            if (debounced(player, isFirstEvent)) return handled();
            useStationItemOnTrack(player, pos);
            player.sendInventory(false);
            return handled();
        }
        if ((held == kRailwayCasing || held.empty()) && !sneak) {
            if (debounced(player, isFirstEvent)) return handled();
            auto msg = useCasingOnTrack(&player, pos, held == kRailwayCasing);
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
                return handled();
            }
        }
    }
    if (isBogey(tid) && held == kWrench) {
        if (debounced(player, isFirstEvent)) return handled();
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
        return handled();
    }
    if (held == kSchedule && tid != kStation) {
        if (debounced(player, isFirstEvent)) return handled();
        openScheduleEditor(player, player.getSelectedItemSlot());
        return handled();
    }
    if (held == kStation && tid != tracks::kTrackId) {
        auto it = state().targets.find(uuidOf(player));
        if (it == state().targets.end()) {
            actionBar(player, trFor(player, "create.track_target.missing", "Right-click the targeted train track first"));
            player.sendInventory(false);
            return handled();
        }
        GlobalPos place = placementPos(region, at, face);
        auto const& tg  = it->second;
        double      dx = place.x - tg.track.x, dy = place.y - tg.track.y, dz = place.z - tg.track.z;
        if (tg.track.dim != place.dim || dx * dx + dy * dy + dz * dz > 16 * 16) {
            actionBar(player, trFor(player, "create.track_target.too_far", "Targeted track is too far from here"));
            player.sendInventory(false);
            return handled();
        }
        state().pendingStations[place] = {tg, uuidOf(player)};
        auto r = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        if (idAt(place) == kStation) state().targets.erase(uuidOf(player));
        return r;
    }
    if (held == kControls) {
        GlobalPos place = placementPos(region, at, face);
        auto      r     = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        orientControls(player, place);
        return r;
    }
    return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
}

LL_TYPE_INSTANCE_HOOK(TrainsUseItemHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$useItem, bool, ItemStack& item, HandSlot handSlot) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(item, handSlot);
    std::string held = item.isNull() ? std::string() : std::string(item.getTypeName());
    if (held == kStation && player.isSneaking()) {
        if (state().targets.erase(uuidOf(player)))
            actionBar(player, trFor(player, "create.track_target.clear", "Cleared track selection"));
        return true;
    }
    if (!player.isSneaking() && held != kSchedule && tryControls(player)) return true;
    if (held == kSchedule) {
        if (debounced(player, true)) return true;
        openScheduleEditor(player, player.getSelectedItemSlot());
        return true;
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(
    TrainsInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&      entity,
    Vec3 const& location,
    HandSlot    handSlot
) {
    Player& player = mPlayer;
    if (player.isClientSide() || entity.isPlayer()) return origin(entity, location, handSlot);
    // a conductor: an entity sitting behind a train's controls
    auto f = entity.getFeetPos();
    for (auto& [id, t] : state().trains) {
        if (t.invalid || t.dim != int(player.getDimensionId())) continue;
        for (auto const& c : t.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [local, rel] : c.controls) {
                cc::Pos seat = local + cc::Pos{t.ax * rel, 0, t.az * rel};
                auto    w    = con->toWorld(cc::Vec3d::center(seat));
                if (std::abs(f.x - w.x) > .7 || std::abs(f.z - w.z) > .7 || std::abs(f.y - w.y) > 1) continue;
                if (handScheduleTo(player, t, player.getSelectedItemSlot())) return true;
            }
        }
    }
    auto const& held = player.getSelectedItem();
    if (!held.isNull() && items::idOf(held) == kSchedule) {
        // a schedule on an entity that isn't a conductor (Create's message for seated mobs)
        for (auto const& id : contraption::all()) {
            auto* con = contraption::find(id);
            if (!con || con->kind() != "train") continue;
            auto o = con->transform().toWorld(cc::Vec3d{0, 0, 0});
            if (std::abs(o.x - f.x) < 32 && std::abs(o.z - f.z) < 32) {
                actionBar(player, trFor(player, "create.schedule.non_controlling_seat", "Conductor needs to sit in front of a Controls block"));
                return true;
            }
        }
    }
    return origin(entity, location, handSlot);
}

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
}

void installHooks() {
    TrainsUseItemOnHook::hook();
    TrainsUseItemHook::hook();
    TrainsInteractHook::hook();
    TrainsAuthInputHook::hook();
    // companion clients: right-click in the air at train controls, HUD throttle
    net::on("trains.interact", [](Player& p, net::Json const&) { tryControls(p); });
    net::on("trains.throttle", [](Player& p, net::Json const& msg) {
        double v = std::clamp(msg.value("throttle", 1.0), 1 / 18.0, 1.0);
        for (auto& [id, t] : state().trains)
            if (t.controller == uuidOf(p)) t.throttle = v;
    });
}

void uninstallHooks() {
    net::off("trains.throttle");
    net::off("trains.interact");
    TrainsAuthInputHook::unhook();
    TrainsInteractHook::unhook();
    TrainsUseItemHook::unhook();
    TrainsUseItemOnHook::unhook();
}

} // namespace lv::feature::trains
