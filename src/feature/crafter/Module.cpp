// Crafter unit (U16): module, game hooks (block use, placement) and the tick loop.
//
// The crafter's use and the orientation of placed crafters, saws and drills come from the shared
// interaction dispatch (common/Interactions.h; network players and SimulatedPlayers alike).

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/crafter/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::crafter {

namespace k  = core::kinetics;
namespace ci = common::items;
namespace ia = common::interactions;

namespace {

std::uint64_t gTick = 0;
std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> gLastUse;

/// Bedrock repeats use-on packets while the button is held: one action per click.
bool debounced(Player& player, GlobalPos const& pos) {
    auto& last = gLastUse[player.getOrCreateUniqueID().rawID];
    bool  skip = last.first == pos && gTick - last.second < 5;
    last       = {pos, gTick};
    return skip;
}

Face nearestLooking(Player const& p) {
    Vec3   v = p.getViewVector();
    double ax = std::abs(v.x), ay = std::abs(v.y), az = std::abs(v.z);
    if (ay >= ax && ay >= az) return v.y > 0 ? Face::Up : Face::Down;
    if (ax >= az) return v.x > 0 ? Face::East : Face::West;
    return v.z > 0 ? Face::South : Face::North;
}

Face horizontalLooking(Player const& p) {
    switch (game::playerFacing(p)) {
    case 0:
        return Face::South;
    case 1:
        return Face::West;
    case 2:
        return Face::North;
    default:
        return Face::East;
    }
}

bool isOurBlock(std::string_view id) { return id == kCrafter || id == kSaw || id == kDrill; }

/// The crafter's own interactions: wrench edges (connect inputs), the front face (insert / take /
/// cover / eject), wrench on the front (cycle the arrow). Returns true if handled.
bool crafterUse(Player& player, GlobalPos const& pos, int face, std::array<double, 3> hit, ItemStack& held) {
    std::string heldId = ci::idOf(held);
    if (player.isSneaking() && !ci::isEmpty(held)) return false;
    Face clicked = static_cast<Face>(std::clamp(face, 0, 5));

    // EdgeInteractionHandler (wrench, not sneaking): connect / disconnect the neighbour's input
    if (heldId == kWrench && !player.isSneaking()) {
        std::vector<Face> candidates;
        for (Face d : k::kFaces) {
            if (k::axisOf(d) == k::axisOf(clicked)) continue;
            if (crafterlogic::shouldConnect(pos, clicked, d)) candidates.push_back(d);
        }
        if (auto dir = cr::edgeAt(clicked, hit[0], hit[1], hit[2], candidates)) {
            if (debounced(player, pos)) return true;
            crafterlogic::toggleConnection(pos, world::offset(pos, *dir));
            world::playSound(pos, "block.itemframe.add_item", .25f, .1f);
            return true;
        }
    }
    auto const* block = world::blockAt(pos);
    if (!block) return false;
    int facing = world::intState(*block, kFacing, 2);
    if (face != facing) return false;
    if (debounced(player, pos)) return true;
    if (crafterlogic::use(player, pos, face, hit[0], hit[1], hit[2], held)) return true;
    if (heldId == kWrench) {
        // onWrenched on the front: cycle the arrow
        int next = static_cast<int>(cr::cycle(static_cast<cr::Pointing>(std::clamp(world::intState(*block, kPointing, 0), 0, 3))));
        world::setStates(pos, {{kPointing, next}});
        crafterlogic::sendData(pos);
        world::playSound(pos, "random.click", .5f, 1.f);
        log().info("crafter at {}: arrow turned by {}", pos.key(), player.getRealName());
        return true;
    }
    return false;
}

} // namespace

std::uint64_t currentTick() { return gTick; }

namespace {

std::vector<ia::HandlerId> gHandlers;

ia::Result onUseOn(ia::UseOnContext const& c) {
    if (c.blockId != kCrafter) return ia::Result::Pass;
    try {
        // the dispatch passes the hook's (mutable) held stack as const: the crafter takes from it
        auto& held = const_cast<ItemStack&>(c.item);
        if (crafterUse(c.player, c.pos, c.face, c.hit, held)) {
            c.player.sendInventory(false); // undo the client's predicted use
            return ia::Result::Consume;
        }
    } catch (std::exception const& e) {
        log().error("crafter use failed: {}", e.what());
    }
    return ia::Result::Pass;
}

/// Placement: orient the block from how the player clicked and looked.
void onPlaced(ia::PlaceContext const& c) {
    if (c.itemId != c.blockId || !isOurBlock(c.blockId)) return;
    try {
        // Create: the block the new one was placed against (behind the clicked face)
        Face clickedFace = static_cast<Face>(std::clamp(c.face, 0, 5));
        auto placedOn    = world::offset(c.pos, k::opposite(clickedFace));
        if (c.blockId == kCrafter) crafterlogic::orientPlaced(c.pos, clickedFace, placedOn, horizontalLooking(c.player), c.sneaking);
        else machines::orientPlaced(c.pos, c.blockId, nearestLooking(c.player), horizontalLooking(c.player), c.sneaking);
    } catch (std::exception const& e) {
        log().error("crafter placement failed: {}", e.what());
    }
}

} // namespace

void installHooks() {
    gHandlers.push_back(ia::onUseOn({.block = std::string(kCrafter)}, onUseOn));
    gHandlers.push_back(ia::onPlace({}, onPlaced));
}
void uninstallHooks() {
    for (auto id : gHandlers) ia::remove(id);
    gHandlers.clear();
}

class CrafterModule {
public:
    void install() {
        crafterlogic::install();
        machines::install();
        installHooks();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            ++gTick;
            try {
                crafterlogic::tickAll(gTick);
                machines::tickAll(gTick);
            } catch (std::exception const& e) {
                log().error("crafter tick failed: {}", e.what());
            }
        });
        // any way a machine appears (players, commands, contraptions, other units) reaches the
        // kinetic network: track it from there
        mListeners.push_back(kinetics::onSpeedChanged(std::string(kCrafter), [](kinetics::SpeedChange const& c) {
            if (!c.removed) crafterlogic::track(c.pos);
        }));
        for (auto id : {kSaw, kDrill}) {
            mListeners.push_back(kinetics::onSpeedChanged(std::string(id), [](kinetics::SpeedChange const& c) {
                if (!c.removed) machines::track(c.pos);
            }));
        }
    }
    void uninstall() {
        uninstallHooks();
        ll::event::EventBus::getInstance().removeListener(mTick);
        for (auto id : mListeners) kinetics::removeListener(id);
        mListeners.clear();
        crafterlogic::uninstall();
        machines::uninstall();
    }

private:
    ll::event::ListenerPtr            mTick;
    std::vector<kinetics::ListenerId> mListeners;
};

} // namespace lv::feature::crafter

LV_REGISTER_MODULE(lv::feature::crafter::CrafterModule);
