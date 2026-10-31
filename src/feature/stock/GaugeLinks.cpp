// Stock unit (U56): the factory gauge's pending clicks (Create's FactoryPanelConnectionHandler),
// its connections to redstone links and display links (Create's FactoryPanelSupportBehaviour) and
// moving a panel to another spot ("Move this gauge", Create's FactoryPanelBehaviour.moveTo).
//
// - "Add new connection" starts a connection from a panel. The next right-click decides: another
//   panel connects as an input; a redstone link or display link on the same surface, facing the
//   same way and closer than 16 blocks gets connected to the panel; sneaking or clicking any other
//   block cancels ("Input connection aborted"), and so does walking 16 blocks away or the panel
//   going away.
// - A *receiving* redstone link with a signal pauses the panel (Create's redstonePowered); a
//   *transmitting* one sends 15 while a connected panel with a target is satisfied (the redstone
//   unit asks through link::setBlockTransmitSource). A display link connected to panels offers
//   their status through the gauge's "Factory gauge status" source and refreshes when a panel's
//   satisfied state changes.
// - Data: the panel keeps `targetedByLinks: [{pos, slot, arrow}]`; the link keeps
//   `stock.linkedGauges: [{pos, slot}]` in its BlockData.

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/display/Display.h"
#include "feature/redstone/RedstoneLink.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items = common::items;
namespace ia    = common::interactions;

namespace {

constexpr int kMaxLinkDistance     = 16; ///< Create: diff.closerThan(ZERO, 16)
constexpr int kMaxRelocateDistance = 24; ///< Create's moveTo: every connection closer than 24

std::map<std::string, PendingConnection>& pending() {
    static std::map<std::string, PendingConnection> m;
    return m;
}

/// The click a pending connection consumed (Bedrock repeats the use while the button is held).
struct Consumed {
    GlobalPos     pos;
    std::uint64_t tick = 0;
};
std::map<std::string, Consumed>& consumed() {
    static std::map<std::string, Consumed> m;
    return m;
}

std::vector<ia::HandlerId>& handlers() {
    static std::vector<ia::HandlerId> h;
    return h;
}

bool isLinkId(std::string const& id) { return id == kRedstoneLinkId || id == kDisplayLinkId; }
bool isGaugeAt(GlobalPos const& p) { return idAt(p) == kFactoryGauge; }
bool activePanel(Json const& panel) { return panel.value("active", false); }

std::optional<std::string> stringState(Block const& block, char const* name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return std::nullopt;
    auto const& states = nbt["states"];
    std::string key(name);
    if (!states.contains(key) || !states[key].hold(Tag::Type::String)) return std::nullopt;
    return std::string(states[key].get<StringTag>());
}

/// The direction a link block points out of its support (WrenchableDirectionalBlock.FACING) as a
/// FacingID, nullopt for other blocks.
std::optional<int> linkFacing(GlobalPos const& link) {
    auto const* b = blockAt(link);
    if (!b) return std::nullopt;
    auto id = game::blockId(*b);
    if (id == kRedstoneLinkId) return game::intState(*b, "levitate:facing").value_or(1);
    if (id == kDisplayLinkId) {
        static constexpr char const* names[6]{"down", "up", "north", "south", "west", "east"};
        auto face = stringState(*b, "minecraft:block_face").value_or("up");
        for (int i = 0; i < 6; ++i)
            if (face == names[i]) return i;
        return 1;
    }
    return std::nullopt;
}

int distSqr(GlobalPos const& a, GlobalPos const& b) {
    int dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/// The component of `diff` along the axis of the gauge's support (Create's
/// connectedDirection.getAxis().choose(diff)).
int alongAxis(GlobalPos const& gauge, GlobalPos const& other) {
    auto s = cs::supportOffset(faceOf(gauge), facingOf(gauge));
    if (s[0] != 0) return other.x - gauge.x;
    if (s[1] != 0) return other.y - gauge.y;
    return other.z - gauge.z;
}

std::string blockName(std::string const& id) {
    auto stack = items::make(id);
    return stack ? items::displayName(*stack) : id;
}

void markConsumed(Player& player, GlobalPos const& pos) { consumed()[uuidOf(player)] = {pos, currentTick()}; }

Json linkList(GlobalPos const& link) {
    auto const* d = dataOf(link);
    if (!d) return Json::array();
    auto it = d->find("linkedGauges");
    return it != d->end() && it->is_array() ? *it : Json::array();
}

void setLinkList(GlobalPos const& link, Json const& list) {
    if (!blockAt(link)) return;
    auto& d = editData(link);
    if (list.empty()) d.erase("linkedGauges");
    else d["linkedGauges"] = list;
}

/// Replaces (gauge, slot) in a link's list by `to` (nullopt: removes it).
void rewriteLinkEntry(GlobalPos const& link, GlobalPos const& gauge, int slot, std::optional<std::pair<GlobalPos, int>> to) {
    Json kept = Json::array();
    for (auto e : linkList(link)) {
        if (posFrom(e.value("pos", Json())) == gauge && e.value("slot", 0) == slot) {
            if (!to) continue;
            e = {{"pos", posJson(to->first)}, {"slot", to->second}};
        }
        kept.push_back(e);
    }
    setLinkList(link, kept);
}

void abortConnection(Player& player, std::map<std::string, PendingConnection>::iterator it) {
    bool relocating = it->second.relocating;
    pending().erase(it);
    if (relocating) statusKey(player, "create.factory_panel.relocation_aborted", "Gauge relocation aborted");
    else statusKey(player, "create.factory_panel.connection_aborted", "Input connection aborted");
}

/// Create's clientTick (relocating): the spot a click on `c` would move the panel to.
std::optional<std::pair<GlobalPos, int>> relocationTarget(PendingConnection const& from, ia::UseOnContext const& c) {
    static constexpr int kNormal[6][3]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    if (c.face < 0 || c.face > 5 || !isGaugeAt(from.pos)) return std::nullopt;
    double    wx = c.pos.x + c.hit[0] + kNormal[c.face][0] / 32.0;
    double    wy = c.pos.y + c.hit[1] + kNormal[c.face][1] / 32.0;
    double    wz = c.pos.z + c.hit[2] + kNormal[c.face][2] / 32.0;
    GlobalPos pos{from.pos.dim, static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)), static_cast<int>(std::floor(wz))};
    auto      face   = faceOf(from.pos);
    int       facing = facingOf(from.pos);
    auto      slot   = cs::targetedSlot(face, facing, {wx - pos.x, wy - pos.y, wz - pos.z});
    if (alongAxis(from.pos, pos) != 0) return std::nullopt;
    // FACTORY_GAUGE.canSurvive: a solid support; no packager behind (that would make a restocker)
    auto s       = cs::supportOffset(face, facing);
    auto support = offset(pos, s[0], s[1], s[2]);
    auto const* b = blockAt(support);
    if (!b || b->isAir() || !b->_isSolid()) return std::nullopt;
    if (packagerAt(support)) return std::nullopt;
    return std::pair{pos, static_cast<int>(slot)};
}

ia::Result pendingUseOn(ia::UseOnContext const& c) {
    auto key = uuidOf(c.player);
    if (auto cit = consumed().find(key); cit != consumed().end()) {
        if (cit->second.pos == c.pos && currentTick() - cit->second.tick < 5) return ia::Result::Consume;
        consumed().erase(cit);
    }
    auto it = pending().find(key);
    if (it == pending().end() || c.offhand) return ia::Result::Pass;
    auto from = it->second;
    if (from.relocating) {
        // Create's onRightClick while relocating: any click ends it (sneaking cancels)
        auto target = c.sneaking ? std::nullopt : relocationTarget(from, c);
        pending().erase(it);
        markConsumed(c.player, c.pos);
        if (!target || !movePanel(&c.player, from.pos, from.slot, target->first, target->second))
            statusKey(c.player, "create.factory_panel.relocation_aborted", "Gauge relocation aborted");
        return ia::Result::Consume;
    }
    if (isLinkId(c.blockId)) {
        pending().erase(it);
        markConsumed(c.player, c.pos);
        (void)connectPanelToLink(&c.player, from.pos, from.slot, c.pos);
        return ia::Result::Consume;
    }
    if (c.blockId == kFactoryGauge && !c.sneaking) return ia::Result::Pass; // panelClicked (useGauge)
    abortConnection(c.player, it);
    markConsumed(c.player, c.pos);
    return ia::Result::Consume;
}

ia::Result pendingUse(ia::UseContext const& c) {
    if (c.onBlock) return ia::Result::Pass;
    // Create: a click into the air ends a relocation (no target); a connection stays pending
    auto it = pending().find(uuidOf(c.player));
    if (it == pending().end() || !it->second.relocating) return ia::Result::Pass;
    abortConnection(c.player, it);
    return ia::Result::Consume;
}

/// A link was removed: the panels it was connected to forget it (Create's
/// FactoryPanelSupportBehaviour.destroy).
void onLinkErased(GlobalPos const& link, Json const& doc, std::string const& previousId) {
    if (!previousId.empty() && !isLinkId(previousId)) return;
    auto sit = doc.find(kDataKey);
    if (sit == doc.end() || !sit->is_object()) return;
    for (auto const& e : sit->value("linkedGauges", Json::array())) {
        auto gauge = posFrom(e.value("pos", Json()));
        int  slot  = e.value("slot", 0);
        if (!gauge || !blockAt(*gauge) || !isGaugeAt(*gauge)) continue;
        auto panel = readGaugePanel(*gauge, slot);
        if (!activePanel(panel)) continue;
        Json kept = Json::array();
        for (auto const& l : panel.value("targetedByLinks", Json::array()))
            if (posFrom(l.value("pos", Json())) != link) kept.push_back(l);
        panel["targetedByLinks"] = kept;
        writeGaugePanel(*gauge, slot, panel);
    }
}

/// Create's shouldBePoweredTristate for a transmitting redstone link: 15 while a connected panel
/// with a target is satisfied, -1 while a connected panel is not loaded.
int linkTransmit(GlobalPos const& link) {
    auto list = linkList(link);
    for (auto const& e : list) {
        auto gauge = posFrom(e.value("pos", Json()));
        if (!gauge) continue;
        if (!blockAt(*gauge)) return -1;
        if (!isGaugeAt(*gauge)) continue;
        auto panel = readGaugePanel(*gauge, e.value("slot", 0));
        if (activePanel(panel) && panel.value("satisfied", false) && panel.value("count", 0) != 0) return 15;
    }
    return 0;
}

} // namespace

// ---- pending connections -------------------------------------------------------------------------

void startConnection(Player& player, GlobalPos const& p, int slot) {
    pending()[uuidOf(player)] = {p, slot, false, currentTick()};
    statusKey(player, "create.factory_panel.click_second_panel", "Click a second gauge to connect...");
    log().info("stock: {} started a gauge connection at {} {} {} slot {}", player.getRealName(), p.x, p.y, p.z, slot);
}

void startRelocation(Player& player, GlobalPos const& p, int slot) {
    pending()[uuidOf(player)] = {p, slot, true, currentTick()};
    statusKey(player, "create.factory_panel.click_to_relocate", "Click a spot to relocate this panel to...");
    log().info("stock: {} started moving the gauge panel at {} {} {} slot {}", player.getRealName(), p.x, p.y, p.z, slot);
}

std::optional<PendingConnection> pendingConnectionOf(Player const& player) {
    auto it = pending().find(uuidOf(player));
    if (it == pending().end()) return std::nullopt;
    return it->second;
}

bool pendingPanelClicked(Player& player, GlobalPos const& p, int slot) {
    auto it = pending().find(uuidOf(player));
    if (it == pending().end() || it->second.relocating) return false;
    auto from = it->second;
    pending().erase(it);
    if ((from.pos == p && from.slot == slot) || !isGaugeAt(from.pos) || !activePanel(readGaugePanel(from.pos, from.slot))) {
        status(player, "");
        return true;
    }
    (void)connectPanels(&player, from.pos, from.slot, p, slot);
    return true;
}

void tickPendingConnections() {
    auto tick = currentTick();
    for (auto it = pending().begin(); it != pending().end();) {
        auto& pc     = it->second;
        auto* player = playerByUuid(it->first);
        if (!player) {
            it = pending().erase(it);
            continue;
        }
        auto const& at   = player->getPosition();
        GlobalPos   feet{static_cast<int>(player->getDimensionId()), static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y)), static_cast<int>(std::floor(at.z))};
        bool        gone = blockAt(pc.pos) && (!isGaugeAt(pc.pos) || !activePanel(readGaugePanel(pc.pos, pc.slot)));
        if (gone || feet.dim != pc.pos.dim || distSqr(feet, pc.pos) >= kMaxLinkDistance * kMaxLinkDistance) {
            status(*player, "");
            log().info("stock: the pending gauge connection of {} was dropped", player->getRealName());
            it = pending().erase(it);
            continue;
        }
        // Create shows the prompt every tick; the action bar keeps it for a while
        if ((tick - pc.since) % 20 == 19) {
            if (pc.relocating) statusKey(*player, "create.factory_panel.click_to_relocate", "Click a spot to relocate this panel to...");
            else statusKey(*player, "create.factory_panel.click_second_panel", "Click a second gauge to connect...");
        }
        ++it;
    }
}

void resetPendingConnections() {
    pending().clear();
    consumed().clear();
}

// ---- links -----------------------------------------------------------------------------------------

std::vector<std::pair<GlobalPos, int>> linkedPanelsOf(GlobalPos const& link) {
    std::vector<std::pair<GlobalPos, int>> out;
    for (auto const& e : linkList(link))
        if (auto g = posFrom(e.value("pos", Json()))) out.emplace_back(*g, e.value("slot", 0));
    return out;
}

bool connectPanelToLink(Player* player, GlobalPos const& gauge, int slot, GlobalPos const& link) {
    auto panel = readGaugePanel(gauge, slot);
    auto fail  = [&](char const* key, char const* fallback) {
        if (player) {
            status(*player, "§c" + trFor(*player, key, fallback));
            playSound(game::globalPos(player->getDimensionBlockSource(), BlockPos{player->getPosition()}), "levitate.stock.deny", 1.0f, 1.0f);
        }
        log().info("stock: gauge to link connection refused: {}", key);
        return false;
    };
    if (!isGaugeAt(gauge) || !activePanel(panel)) return fail("create.factory_panel.connection_aborted", "Input connection aborted");
    auto facing = linkFacing(link);
    if (!facing) return fail("create.factory_panel.connection_aborted", "Input connection aborted");
    if (*facing != gaugePanelFace(gauge)) return fail("create.factory_panel.same_orientation", "Blocks must have the same orientation");
    if (link.dim != gauge.dim) return fail("create.factory_panel.too_far_apart", "Blocks are too far away from each other");
    if (alongAxis(gauge, link) != 0) return fail("create.factory_panel.same_surface", "Blocks must be on the same surface");
    if (distSqr(gauge, link) >= kMaxLinkDistance * kMaxLinkDistance) return fail("create.factory_panel.too_far_apart", "Blocks are too far away from each other");
    // the link's "slot" is the one nearest to the panel (only where the arrow starts)
    auto   face   = faceOf(gauge);
    int    gf     = facingOf(gauge);
    auto   source = cs::panelCentre(face, gf, static_cast<cs::PanelSlot>(slot));
    int    best   = 0;
    double bestD  = 1e18;
    for (int s = 0; s < 4; ++s) {
        auto   c  = cs::panelCentre(face, gf, static_cast<cs::PanelSlot>(s));
        double dx = link.x + c.x - gauge.x - source.x, dy = link.y + c.y - gauge.y - source.y, dz = link.z + c.z - gauge.z - source.z;
        double d  = dx * dx + dy * dy + dz * dz;
        if (d <= bestD) {
            bestD = d;
            best  = s;
        }
    }
    // Create's addConnection: one connection per link position
    Json links = Json::array();
    for (auto const& l : panel.value("targetedByLinks", Json::array()))
        if (posFrom(l.value("pos", Json())) != link) links.push_back(l);
    links.push_back({{"pos", posJson(link)}, {"slot", best}, {"arrow", 0}});
    panel["targetedByLinks"] = links;
    writeGaugePanel(gauge, slot, panel);
    // FactoryPanelSupportBehaviour.connect
    auto list    = linkList(link);
    bool already = std::any_of(list.begin(), list.end(), [&](Json const& e) { return posFrom(e.value("pos", Json())) == gauge && e.value("slot", 0) == slot; });
    if (!already) list.push_back({{"pos", posJson(gauge)}, {"slot", slot}});
    setLinkList(link, list);
    auto id = idAt(link);
    if (player) status(*player, "§a" + trFor(*player, "create.factory_panel.link_connected", "Gauge connected to {}", blockName(id)));
    playSound(gauge, "levitate.stock.connect", 0.5f, 0.5f);
    if (id == kDisplayLinkId) display::refreshLink(link);
    log().info("stock: gauge at {} {} {} slot {} connected to the {} at {} {} {}", gauge.x, gauge.y, gauge.z, slot, id, link.x, link.y, link.z);
    return true;
}

void disconnectPanelLinks(GlobalPos const& gauge, int slot, Json& panel) {
    for (auto const& l : panel.value("targetedByLinks", Json::array())) {
        auto link = posFrom(l.value("pos", Json()));
        if (!link || !blockAt(*link)) continue;
        rewriteLinkEntry(*link, gauge, slot, std::nullopt);
        if (idAt(*link) == kDisplayLinkId) display::refreshLink(*link);
    }
    if (panel.is_object()) panel["targetedByLinks"] = Json::array();
}

void checkPanelRedstoneInput(GlobalPos const& gauge, int slot, Json& panel, bool& changed) {
    bool power = false;
    for (auto const& l : panel.value("targetedByLinks", Json::array())) {
        auto link = posFrom(l.value("pos", Json()));
        if (!link) continue;
        if (!blockAt(*link)) return; // not loaded: keep the state (Create returns)
        if (idAt(*link) != kRedstoneLinkId) continue;
        // FactoryPanelSupportBehaviour.shouldPanelBePowered: a receiver with a signal
        if (stateOr(*link, "levitate:receiver", 0) != 0 && stateOr(*link, "levitate:power", 0) > 0) power = true;
    }
    if (power == panel.value("paused", false)) return;
    panel["paused"] = power;
    changed         = true;
    setGaugeTimer(gauge, slot, 1);
    log().info("stock: gauge panel at {} {} {} slot {} {} by redstone", gauge.x, gauge.y, gauge.z, slot, power ? "paused" : "resumed");
}

void notifyPanelLinkOutputs(Json const& panel) {
    // transmitting redstone links read the panels every tick (link::setBlockTransmitSource);
    // display links refresh now
    for (auto const& l : panel.value("targetedByLinks", Json::array())) {
        auto link = posFrom(l.value("pos", Json()));
        if (link && blockAt(*link) && idAt(*link) == kDisplayLinkId) display::refreshLink(*link);
    }
}

// ---- relocation ------------------------------------------------------------------------------------

bool movePanel(Player* player, GlobalPos const& from, int fromSlot, GlobalPos const& to, int toSlot) {
    if (!isGaugeAt(from) || toSlot < 0 || toSlot > 3) return false;
    auto panel = readGaugePanel(from, fromSlot);
    if (!activePanel(panel) || from.dim != to.dim) return false;
    auto* region = regionOf(to.dim);
    auto const* existing = blockAt(to);
    if (!region || !existing) return false;
    // Create: the target spot has no panel, and is air or a gauge with the same orientation
    bool onGauge = game::blockId(*existing) == kFactoryGauge;
    if (onGauge && activePanel(readGaugePanel(to, toSlot))) return false;
    if (!existing->isAir() && !onGauge) return false;
    if (onGauge && (faceOf(to) != faceOf(from) || facingOf(to) != facingOf(from))) return false;
    // every connection stays within 24 blocks
    auto within = [&](GlobalPos const& p) { return p.dim == to.dim && distSqr(p, to) < kMaxRelocateDistance * kMaxRelocateDistance; };
    for (char const* list : {"targetedByLinks", "targetedBy", "targeting"})
        for (auto const& c : panel.value(list, Json::array()))
            if (auto p = posFrom(c.value("pos", Json())); p && !within(*p)) return false;
    if (!onGauge) {
        auto const* block = game::blockWith(
            kFactoryGauge,
            {{kStateFace, static_cast<int>(faceOf(from))}, {kStateFacing, facingOf(from)}, {kStatePanels, 0}}
        );
        if (!block || !game::setBlock(*region, game::blockPos(to), *block)) return false;
    }
    // the panel moves with all its settings
    writeGaugePanel(to, toSlot, panel);
    writeGaugePanel(from, fromSlot, Json::object());
    registry().gauges.insert(to);
    saveRegistry();
    // rewire: the panels it feeds, its inputs, its links
    auto same = [](Json const& c, GlobalPos const& p, int s) { return posFrom(c.value("pos", Json())) == p && c.value("slot", 0) == s; };
    for (auto const& t : panel.value("targeting", Json::array())) {
        auto dstPos = posFrom(t.value("pos", Json()));
        if (!dstPos || !blockAt(*dstPos)) continue;
        int  ds  = t.value("slot", 0);
        auto dst = readGaugePanel(*dstPos, ds);
        for (auto& c : dst["targetedBy"])
            if (same(c, from, fromSlot)) c["pos"] = posJson(to), c["slot"] = toSlot;
        writeGaugePanel(*dstPos, ds, dst);
    }
    for (auto const& c : panel.value("targetedBy", Json::array())) {
        auto srcPos = posFrom(c.value("pos", Json()));
        if (!srcPos || !blockAt(*srcPos)) continue;
        int  ss  = c.value("slot", 0);
        auto src = readGaugePanel(*srcPos, ss);
        for (auto& t : src["targeting"])
            if (same(t, from, fromSlot)) t["pos"] = posJson(to), t["slot"] = toSlot;
        writeGaugePanel(*srcPos, ss, src);
    }
    for (auto const& l : panel.value("targetedByLinks", Json::array()))
        if (auto link = posFrom(l.value("pos", Json())); link && blockAt(*link)) rewriteLinkEntry(*link, from, fromSlot, std::pair{to, toSlot});
    updateGaugePanelsState(to);
    // the old block goes with its last panel (Create's FactoryPanelBlockEntity: no active panels)
    int left = 0;
    for (int s = 0; s < 4; ++s) left += activePanel(readGaugePanel(from, s)) ? 1 : 0;
    if (left == 0) game::clearBlock(*region, game::blockPos(from));
    else updateGaugePanelsState(from);
    if (player) statusKey(*player, "create.factory_panel.relocated", "Gauge moved to new position", "§a");
    playSound(to, "dig.copper", 1.0f, 1.0f);
    log().info("stock: gauge panel moved from {} {} {} slot {} to {} {} {} slot {}", from.x, from.y, from.z, fromSlot, to.x, to.y, to.z, toSlot);
    return true;
}

// ---- module ------------------------------------------------------------------------------------------

void installGaugeLinks() {
    // above the value boxes and filter slots of the links: Create's connection handler sees the
    // click before anything else
    handlers().push_back(ia::onUseOn({}, pendingUseOn, ia::priority::Highest + 10));
    handlers().push_back(ia::onUse({}, pendingUse, ia::priority::Highest + 10));
    static bool once = false;
    if (!std::exchange(once, true)) common::blockdata::onErase(onLinkErased);
    redstone::link::setBlockTransmitSource(linkTransmit);
}

void uninstallGaugeLinks() {
    for (auto id : handlers()) ia::remove(id);
    handlers().clear();
    redstone::link::setBlockTransmitSource({});
}

} // namespace lv::feature::stock
