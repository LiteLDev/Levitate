// Belts unit: building and changing belts. The belt connector (Create's BeltConnectorItem),
// right-clicks on belts (BeltBlock.use / onWrenched: items, dye, pulleys, casings), the wrench and
// connector on belt ends (BeltSlicer: shorten, split, extend, merge) and breaking (onRemove: the
// whole chain goes, pulleys stay as shafts).

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/world/phys/HitResult.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::belts {

namespace citems = common::items;

namespace {

std::string posText(GlobalPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

cb::ConnectWorld connectWorld(int dim) {
    cb::ConnectWorld w;
    w.loaded    = [dim](cb::Vec3i const& p) { return loadedAt({dim, p.x, p.y, p.z}); };
    w.shaftAxis = [dim](cb::Vec3i const& p) { return shaftAxisAt({dim, p.x, p.y, p.z}); };
    w.speed     = [dim](cb::Vec3i const& p) { return kinetics::theoreticalSpeedAt({dim, p.x, p.y, p.z}); };
    w.replaceable = [dim](cb::Vec3i const& p) {
        auto* r = regionOf(dim);
        return r && game::replaceable(*r, BlockPos{p.x, p.y, p.z});
    };
    return w;
}

// ---- moving items through edits ------------------------------------------------------------------------

/// An item taken off a belt during an edit, remembered by the world segment it was on.
struct Loose {
    GlobalPos        seg;
    float            frac = 0; ///< position inside the segment along the old belt direction
    cb::Vec3i        dir;      ///< the old belt vector
    cb::Item         item;
    StackData        data;
};

std::vector<Loose> takeItems(GlobalPos const& controller) {
    std::vector<Loose> out;
    auto&              s  = state();
    auto               it = s.belts.find(controller);
    if (it == s.belts.end()) {
        if (!beltFor(controller)) return out;
        it = s.belts.find(controller);
    }
    Belt& b  = *it->second;
    auto  cs = segmentAt(controller).value_or(cb::SegmentState{});
    auto  v  = cb::beltVector(cs);
    for (auto const& item : b.inv.all()) {
        auto sd = b.stacks.find(item.id);
        if (sd == b.stacks.end() || citems::isEmpty(sd->second.stack)) continue;
        int index = std::clamp(int(std::floor(item.beltPosition)), 0, std::max(b.length - 1, 0));
        out.push_back({offset(controller, v * index), item.beltPosition - float(index), v, item, std::move(sd->second)});
    }
    s.belts.erase(it);
    // the saved copy must not come back when the belt is loaded again
    if (auto const* d = common::blockdata::get(controller); d && d->contains("belt")) common::blockdata::edit(controller)["belt"].erase("items");
    return out;
}

void putItems(std::vector<Loose>& items) {
    for (auto& l : items) {
        auto seg  = segmentAt(l.seg);
        auto info = seg ? segmentInfo(l.seg) : std::nullopt;
        Belt* b   = info ? beltFor(info->controller) : nullptr;
        if (!seg || !info || !b) {
            removeDisplay(l.data);
            spawnItem(l.seg, l.seg.x + .5, l.seg.y + cb::kBeltTop + .1, l.seg.z + .5, l.data.stack);
            continue;
        }
        auto cs       = segmentAt(info->controller).value_or(*seg);
        auto v        = cb::beltVector(cs);
        bool flipped  = v.x == -l.dir.x && v.y == -l.dir.y && v.z == -l.dir.z && !(v == l.dir);
        float frac    = flipped ? 1.f - l.frac : l.frac;
        l.item.beltPosition     = float(info->index) + frac;
        l.item.prevBeltPosition = l.item.beltPosition;
        if (flipped) l.item.sideOffset = -l.item.sideOffset, l.item.targetSideOffset = -l.item.targetSideOffset;
        l.item.insertedAt = info->index;
        b->stacks[l.item.id] = std::move(l.data);
        b->inv.insertSorted(l.item);
    }
    items.clear();
}

void setSegment(GlobalPos const& p, cb::SegmentState const& s) {
    if (auto const* b = beltBlock(s)) setBlockQuiet(p, *b);
}

bool isCreative(Player const& p) { return p.isCreative(); }

int dyeColor(std::string const& id) {
    static constexpr char const* kDyes[] = {
        "minecraft:white_dye", "minecraft:orange_dye", "minecraft:magenta_dye", "minecraft:light_blue_dye",
        "minecraft:yellow_dye", "minecraft:lime_dye", "minecraft:pink_dye", "minecraft:gray_dye",
        "minecraft:light_gray_dye", "minecraft:cyan_dye", "minecraft:purple_dye", "minecraft:blue_dye",
        "minecraft:brown_dye", "minecraft:green_dye", "minecraft:red_dye", "minecraft:black_dye",
    };
    for (int i = 0; i < 16; ++i)
        if (id == kDyes[i]) return i;
    return -1;
}

bool isWater(ItemStack const& stack) {
    auto id = citems::idOf(stack);
    if (id == "minecraft:water_bucket") return true;
    return id == "minecraft:potion" && stack.getAuxValue() == 0; // water bottle
}

bool isShaftItem(std::string const& id) { return id == kShaftId || id == kTestShaftId; }

/// Items matching `pred` in the player's inventory.
int countInInventory(Player& player, std::function<bool(ItemStack const&)> const& pred) {
    auto& inv   = player.getInventory();
    int   found = 0;
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& st = inv.getItem(i);
        if (!citems::isEmpty(st) && pred(st)) found += citems::countOf(st);
    }
    return found;
}

/// Takes `count` items matching `pred` from the player's inventory (survival); all or nothing.
bool takeFromInventory(Player& player, int count, std::function<bool(ItemStack const&)> const& pred) {
    if (count <= 0) return true;
    auto& inv   = player.getInventory();
    int   size  = inv.getContainerSize();
    int   found = 0;
    for (int i = 0; i < size && found < count; ++i) {
        auto const& st = inv.getItem(i);
        if (!citems::isEmpty(st) && pred(st)) found += citems::countOf(st);
    }
    if (found < count) return false;
    int need = count;
    for (int i = 0; i < size && need > 0; ++i) {
        ItemStack st = inv.getItem(i);
        if (citems::isEmpty(st) || !pred(st)) continue;
        int take = std::min(need, citems::countOf(st));
        need -= take;
        inv.setItem(i, citems::withCount(st, citems::countOf(st) - take));
    }
    player.sendInventory(false);
    return true;
}

void consumeHeld(Player& player, int n = 1) {
    if (isCreative(player)) return;
    ItemStack held = player.getSelectedItem();
    player.setSelectedItem(citems::withCount(held, citems::countOf(held) - n));
    player.sendInventory(false);
}

/// Create's applyColor: the whole belt takes the colour (null removes it).
void applyColor(GlobalPos const& pos, int color) {
    auto info = segmentInfo(pos);
    if (!info) return;
    auto chain = chainFrom(info->controller);
    for (auto const& p : chain) {
        auto s = segmentAt(p);
        if (!s || s->color == color) continue;
        s->color = color;
        setSegment(p, *s);
    }
}

// ---- BeltSlicer --------------------------------------------------------------------------------------

/// Wrench on a belt (Create's BeltSlicer.useWrench). Returns nullopt for PASS, else handled.
std::optional<bool> useWrench(Player& player, GlobalPos const& pos, cb::SegmentState const& st, int face, cb::Vec3d const& hitFromCentre) {
    auto info = segmentInfo(pos);
    if (!info) return std::nullopt;
    if (st.casing != cb::Casing::None && face != 1) return std::nullopt;
    if (st.part == cb::Part::Pulley && face != 0 && face != 1) return std::nullopt;
    int length = info->length;
    if (length == 2) return false;
    auto v       = cb::beltVector(st);
    bool creative = isCreative(player);

    if (cb::hoveringEnd(st, hitFromCentre)) {
        // shorten from this end
        GlobalPos next = st.part == cb::Part::End ? offset(pos, {-v.x, -v.y, -v.z}) : offset(pos, v);
        auto      nextState = segmentAt(next);
        if (!nextState) return false;
        auto items = takeItems(info->controller);
        bool wasPulley = nextState->part == cb::Part::Pulley;
        cb::SegmentState moved = st;
        moved.casing           = nextState->casing;
        setSegment(next, moved);
        clearBlockQuiet(pos);
        particle(pos, pos.x + .5, pos.y + .5, pos.z + .5, "minecraft:huge_explosion_lab_misc_emitter");
        if (!creative && wasPulley) {
            if (auto shaft = shaftItem()) giveToPlayer(player, *shaft);
        }
        rebuildBelt(next);
        putItems(items);
        log().info("belts: {} shortened the belt at {} to {} blocks", player.getRealName(), posText(pos), length - 1);
        return true;
    }

    // split in two
    double    dot          = hitFromCentre.x * v.x + hitFromCentre.y * v.y + hitFromCentre.z * v.z;
    bool      towardsStart = dot < 0;
    int       a            = towardsStart ? info->index - 1 : info->index; // split between a and a + 1
    if (a < 1 || a + 1 > length - 2) return false;                         // both parts need 2 blocks
    GlobalPos pa = offset(info->controller, v * a), pb = offset(info->controller, v * (a + 1));
    auto      sa = segmentAt(pa), sb = segmentAt(pb);
    if (!sa || !sb) return false;
    int need = (cb::hasPulley(sa->part) ? 0 : 1) + (cb::hasPulley(sb->part) ? 0 : 1);
    if (!creative) {
        // Create's BeltSlicer: a survival split takes the shafts *and one belt connector* (each half
        // gives a connector back when broken); nothing is taken unless both are there
        auto isShaft     = [](ItemStack const& s) { return isShaftItem(citems::idOf(s)); };
        auto isConnector = [](ItemStack const& s) { return citems::idOf(s) == kBeltConnectorId; };
        if (countInInventory(player, isShaft) < need || countInInventory(player, isConnector) < 1) return false;
        takeFromInventory(player, need, isShaft);
        takeFromInventory(player, 1, isConnector);
    }
    auto items = takeItems(info->controller);
    sa->part   = cb::Part::End;
    sb->part   = cb::Part::Start;
    setSegment(pa, *sa);
    setSegment(pb, *sb);
    rebuildBelt(pa);
    rebuildBelt(pb);
    putItems(items);
    log().info("belts: {} split the belt at {} into {} + {} blocks", player.getRealName(), posText(pos), a + 1, length - a - 1);
    return true;
}

/// Create's BeltBlock.onWrenched: casing off, else a pulley's shaft out.
bool onWrenched(Player& player, GlobalPos const& pos, cb::SegmentState st) {
    if (st.casing != cb::Casing::None) {
        st.casing = cb::Casing::None;
        setSegment(pos, st);
        playSound(pos, "dig.stone", 1, 1);
        log().info("belts: {} removed the casing at {}", player.getRealName(), posText(pos));
        return true;
    }
    if (st.part == cb::Part::Pulley) {
        st.part = cb::Part::Middle;
        setSegment(pos, st);
        if (!isCreative(player))
            if (auto shaft = shaftItem()) giveToPlayer(player, *shaft);
        log().info("belts: {} removed the pulley at {}", player.getRealName(), posText(pos));
        return true;
    }
    return false;
}

/// Belt connector on a belt end (Create's BeltSlicer.useConnector: extend / merge).
bool useConnectorOnBelt(Player& player, GlobalPos const& pos, cb::SegmentState const& st, cb::Vec3d const& hitFromCentre) {
    auto info = segmentInfo(pos);
    if (!info) return false;
    int max = config().maxBeltLength;
    if (info->length >= max) return false;
    if (!cb::hoveringEnd(st, hitFromCentre)) return false;
    auto      v    = cb::beltVector(st);
    GlobalPos next = st.part == cb::Part::Start ? offset(pos, {-v.x, -v.y, -v.z}) : offset(pos, v);
    auto*     r    = regionOf(pos.dim);
    if (!r || !loadedAt(next)) return false;
    auto nextState = segmentAt(next);
    if (!game::replaceable(*r, game::blockPos(next))) {
        // merge with the belt there
        if (!nextState || !cb::statesCompatible(st, *nextState)) return false;
        auto otherInfo = segmentInfo(next);
        if (!otherInfo || otherInfo->controller == info->controller) return false;
        if (otherInfo->length + info->length > max) return false;
        auto items  = takeItems(info->controller);
        auto items2 = takeItems(otherInfo->controller);
        items.insert(items.end(), std::make_move_iterator(items2.begin()), std::make_move_iterator(items2.end()));
        bool flip = st.facing != nextState->facing;
        for (auto const& p : chainFrom(otherInfo->controller)) {
            auto s = segmentAt(p);
            if (!s) continue;
            if (flip) *s = cb::flipped(*s);
            s->color = st.color;
            setSegment(p, *s);
        }
        auto here = st;
        here.part = cb::Part::Middle;
        setSegment(pos, here);
        auto there = *segmentAt(next);
        there.part = cb::Part::Middle;
        setSegment(next, there);
        if (!isCreative(player)) {
            if (auto shaft = shaftItem(2)) giveToPlayer(player, *shaft);
            if (auto conn = makeStack(kBeltConnectorId)) giveToPlayer(player, *conn);
        }
        playSound(pos, "hit.cloth", .5f, 1.3f);
        rebuildBelt(pos);
        putItems(items);
        log().info("belts: {} merged two belts at {}", player.getRealName(), posText(pos));
        return true;
    }
    // extend by one block
    auto items = takeItems(info->controller);
    auto here  = st;
    here.part  = cb::Part::Middle;
    setSegment(pos, here);
    auto added   = st;
    added.casing = cb::Casing::None;
    setSegment(next, added);
    playSound(pos, "use.cloth", .5f, 1);
    rebuildBelt(pos);
    putItems(items);
    log().info("belts: {} extended the belt at {} to {} blocks", player.getRealName(), posText(pos), info->length + 1);
    return true;
}

} // namespace

// ---- the belt connector ------------------------------------------------------------------------------

bool createBelt(GlobalPos const& start, GlobalPos const& end) {
    auto w    = connectWorld(start.dim);
    auto plan = cb::planBelt(w, vecOf(start), vecOf(end));
    for (auto const& seg : plan) {
        auto const* b = blockAt({start.dim, seg.pos.x, seg.pos.y, seg.pos.z});
        if (!b) return false;
    }
    for (auto const& seg : plan) {
        GlobalPos p{start.dim, seg.pos.x, seg.pos.y, seg.pos.z};
        setSegment(p, seg.state);
    }
    rebuildBelt(start);
    GlobalPos mid{start.dim, (start.x + end.x) / 2, (start.y + end.y) / 2, (start.z + end.z) / 2};
    playSound(mid, "use.cloth", .5f, 1);
    return true;
}

void syncConnectorSelection(Player& player) {
    if (!net::isCompanion(player)) return;
    auto& s  = state();
    auto  it = s.connector.find(uuidOf(player));
    if (it == s.connector.end()) {
        net::send(player, "belts.connector", {{"clear", true}});
        return;
    }
    auto const& p = it->second.pos;
    net::send(player, "belts.connector", {{"dim", p.dim}, {"pos", {p.x, p.y, p.z}}, {"max", config().maxBeltLength}});
}

namespace {
void selectShaft(Player& player, std::optional<GlobalPos> pos) {
    auto& s    = state();
    auto  uuid = uuidOf(player);
    if (pos) s.connector[uuid] = {*pos};
    else s.connector.erase(uuid);
    syncConnectorSelection(player);
}
} // namespace

void clearConnectorSelection(Player& player) { selectShaft(player, std::nullopt); }

bool setCasing(GlobalPos const& pos, int casing) {
    auto st = segmentAt(pos);
    if (!st) return false;
    auto c = casing == 2 ? cb::Casing::Brass : casing == 1 ? cb::Casing::Andesite : cb::Casing::None;
    if (st->casing == c) return true;
    st->casing = c;
    setSegment(pos, *st);
    log().info("belts: the belt at {} was encased ({}) for a tunnel", posText(pos), c == cb::Casing::Brass ? "brass" : "andesite");
    return true;
}

bool onConnectorUse(Player& player, GlobalPos const& pos, int /*face*/, cb::Vec3d const& hitInBlock) {
    auto& s    = state();
    auto  uuid = uuidOf(player);
    if (player.isSneaking()) {
        selectShaft(player, std::nullopt);
        return true;
    }
    if (auto st = segmentAt(pos)) {
        useConnectorOnBelt(player, pos, *st, hitInBlock - cb::Vec3d{.5, .5, .5});
        return true;
    }
    if (!shaftAxisAt(pos)) return false;
    auto sel = s.connector.find(uuid);
    auto farAway = [&](GlobalPos const& a) { // Create drops a first shaft 2 x maxBeltLength away
        double dx = a.x - pos.x, dy = a.y - pos.y, dz = a.z - pos.z, r = 2.0 * config().maxBeltLength;
        return dx * dx + dy * dy + dz * dz >= r * r;
    };
    if (sel != s.connector.end() && sel->second.pos.dim == pos.dim && shaftAxisAt(sel->second.pos) && !farAway(sel->second.pos)) {
        GlobalPos first = sel->second.pos;
        // Create's BeltConnectorItem.useOn: a failed connection (also the same shaft again) keeps
        // the first shaft selected; only a belt made clears it
        if (first == pos) return true;
        auto err = cb::canConnect(connectWorld(pos.dim), vecOf(first), vecOf(pos), config().maxBeltLength);
        if (err != cb::ConnectError::None) {
            log().info("belts: {} could not connect {} and {}: {}", player.getRealName(), posText(first), posText(pos), cb::connectErrorName(err));
            return true;
        }
        if (!createBelt(first, pos)) return true;
        selectShaft(player, std::nullopt);
        consumeHeld(player);
        auto info = segmentInfo(first);
        log().info("belts: {} connected a belt from {} to {} ({} blocks)", player.getRealName(), posText(first), posText(pos), info ? info->length : 0);
        return true;
    }
    selectShaft(player, pos);
    log().info("belts: {} selected the shaft at {}", player.getRealName(), posText(pos));
    return true;
}

namespace {
std::mt19937& previewRng() {
    static std::mt19937 g{std::random_device{}()};
    return g;
}

void sendDust(Player& player, int dim, double x, double y, double z, bool valid) {
    SpawnParticleEffectPacket pkt(SpawnParticleEffectPacketPayload(
        Vec3{float(x), float(y), float(z)},
        valid ? "levitate:belt_connector_valid" : "levitate:belt_connector_invalid",
        static_cast<uchar>(dim),
        std::nullopt
    ));
    pkt.sendTo(player);
}
} // namespace

std::optional<ConnectorPreview> connectorPreviewFor(Player& player, GlobalPos const& first) {
    auto axis = shaftAxisAt(first);
    if (!axis) return std::nullopt;
    ConnectorPreview out;
    auto             hit = player.traceRay(5.5f, false, true);
    if (hit.mType != HitResultType::Tile) return out; // looking at nothing: only the first shaft sparkles
    GlobalPos selected = game::globalPos(first.dim, hit.mBlock);
    auto*     region   = regionOf(first.dim);
    if (!region || game::replaceable(*region, game::blockPos(selected))) return std::nullopt;
    if (!shaftAxisAt(selected)) selected = offset(selected, cb::faceNormal(hit.mFacing));
    cb::Vec3i d = vecOf(selected) - vecOf(first);
    int       m = config().maxBeltLength;
    if (!(d.distSqr() < 1LL * m * m)) return std::nullopt;
    out.valid = shaftAxisAt(selected) && cb::canConnect(connectWorld(first.dim), vecOf(first), vecOf(selected), m) == cb::ConnectError::None;
    out.line  = cb::previewLine(vecOf(first), vecOf(selected), *axis);
    if (!out.line) return std::nullopt;
    return out;
}

void connectorPreview() {
    // Create's BeltConnectorHandler: dust along the planned belt while the connector is held with a
    // shaft selected. The client mod draws it for its player every tick; the others get the
    // particles from the server every 5 ticks (sent to that player only, as Create's client-side
    // particles; a fifth of Create's rate keeps the packets down).
    auto& s = state();
    if (s.connector.empty()) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    auto& rng = previewRng();
    level->forEachPlayer([&](Player& p) {
        auto it = s.connector.find(uuidOf(p));
        if (it == s.connector.end() || net::isCompanion(p)) return true;
        if (citems::idOf(p.getSelectedItem()) != kBeltConnectorId) return true;
        GlobalPos first = it->second.pos;
        if (int(p.getDimensionId()) != first.dim) return true;
        auto preview = connectorPreviewFor(p, first);
        if (!preview) return true;
        std::uniform_real_distribution<double> jitter(-.25, .25);
        if (!preview->line) {
            if (rng() % 50 == 0) sendDust(p, first.dim, first.x + .5 + jitter(rng), first.y + .5 + jitter(rng), first.z + .5 + jitter(rng), true);
            return true;
        }
        auto const& line = *preview->line;
        for (float f = 0; f < line.length; f += 1 / 16.f) {
            if (rng() % 10 != 0) continue;
            sendDust(p, first.dim, first.x + .5 + line.step.x * f, first.y + .5 + line.step.y * f, first.z + .5 + line.step.z * f, preview->valid);
        }
        return true;
    });
}
// ---- right-click on a belt ---------------------------------------------------------------------------

bool onBeltUse(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock) {
    if (player.isSneaking()) return false;
    auto st = segmentAt(pos);
    if (!st) return false;
    ItemStack const& held = player.getSelectedItem();
    auto             id   = citems::idOf(held);

    int dye = dyeColor(id);
    if (dye >= 0 || isWater(held)) {
        applyColor(pos, dye);
        log().info("belts: {} {} the belt at {}", player.getRealName(), dye >= 0 ? "dyed" : "washed", posText(pos));
        return true;
    }
    if (id == kBeltConnectorId) return onConnectorUse(player, pos, face, hitInBlock);
    if (id == kWrenchId) return onBeltWrench(player, pos, face, hitInBlock);
    auto* belt = beltOfSegment(pos);
    if (!belt) return false;
    if (citems::isEmpty(held)) {
        // take the items on this segment (Create: within .55 of the segment's centre)
        auto info = segmentInfo(pos);
        if (!info) return true;
        int taken = 0;
        applyOnBelt(*belt, info->index + .5f, .55f, [&](TransportedItem& t) {
            giveToPlayer(player, t.stack);
            ++taken;
            return TransportedResult::removeItem();
        });
        if (taken) {
            belt->inv.flushQueues();
            playSound(pos, "random.pop", .2f, 1.f);
            log().info("belts: {} took {} stack(s) from the belt at {}", player.getRealName(), taken, posText(pos));
        }
        return true;
    }
    if (isShaftItem(id)) {
        if (st->part != cb::Part::Middle) return false;
        consumeHeld(player);
        st->part = cb::Part::Pulley;
        setSegment(pos, *st);
        log().info("belts: {} added a pulley at {}", player.getRealName(), posText(pos));
        return true;
    }
    if (id == kBrassCasingId || id == kAndesiteCasingId) {
        auto c = id == kBrassCasingId ? cb::Casing::Brass : cb::Casing::Andesite;
        if (st->casing == c) return true;
        st->casing = c;
        setSegment(pos, *st);
        playSound(pos, "dig.stone", 1, 1);
        log().info("belts: {} encased the belt at {} ({})", player.getRealName(), posText(pos), c == cb::Casing::Brass ? "brass" : "andesite");
        return true;
    }
    return false;
}

bool onBeltWrench(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock) {
    auto st = segmentAt(pos);
    if (!st) return false;
    if (useWrench(player, pos, *st, face, hitInBlock - cb::Vec3d{.5, .5, .5})) return true;
    onWrenched(player, pos, *st);
    return true;
}

// ---- breaking ------------------------------------------------------------------------------------------

namespace {
std::mt19937& explosionRng() {
    static std::mt19937 g{std::random_device{}()};
    return g;
}
} // namespace

void processRemovals() {
    auto& s = state();
    if (s.removals.empty()) return;
    auto                                   removals = std::exchange(s.removals, {});
    std::map<GlobalPos, cb::SegmentState>  removed;
    for (auto const& r : removals) removed[r.pos] = r.state;
    std::set<GlobalPos> handled;

    for (auto const& r : removals) {
        if (handled.contains(r.pos)) continue;
        std::vector<GlobalPos>               world;
        std::vector<std::pair<GlobalPos, cb::SegmentState>> gone{{r.pos, r.state}};
        handled.insert(r.pos);
        for (bool fwd : {true, false}) {
            GlobalPos        cur = r.pos;
            cb::SegmentState st  = r.state;
            for (int i = 0; i < 1000; ++i) {
                auto off = cb::nextSegmentOffset(st, fwd);
                if (!off) break;
                GlobalPos nxt = offset(cur, *off);
                if (auto it = removed.find(nxt); it != removed.end()) {
                    if (handled.contains(nxt)) break;
                    handled.insert(nxt);
                    gone.emplace_back(nxt, it->second);
                    st  = it->second;
                    cur = nxt;
                    continue;
                }
                auto ws = segmentAt(nxt);
                if (!ws || handled.contains(nxt)) break;
                handled.insert(nxt);
                world.push_back(nxt);
                st  = *ws;
                cur = nxt;
            }
        }
        // the items of the belt fall out (Create: the controller ejects all)
        auto drop = [&](GlobalPos const& p) {
            if (auto it = s.belts.find(p); it != s.belts.end()) {
                ejectAll(*it->second);
                s.belts.erase(it);
            }
            indexRemove(p);
        };
        for (auto const& [p, st] : gone)
            if (st.part == cb::Part::Start) drop(p);
        for (auto const& p : world)
            if (auto st = segmentAt(p); st && st->part == cb::Part::Start) drop(p);
        // the rest of the chain: pulleys become shafts again
        for (auto const& p : world) {
            auto st = segmentAt(p);
            if (!st) continue;
            if (cb::hasPulley(st->part)) {
                if (auto const* shaft = shaftBlock(cb::rotationAxis(*st))) setBlockQuiet(p, *shaft);
            } else {
                clearBlockQuiet(p);
            }
        }
        // drops of the broken segments: one belt connector for the belt, a shaft per pulley
        bool anySurvival = false;
        int  shafts      = 0;
        for (auto const& [p, st] : gone) {
            auto it = s.playerBroke.find(p);
            if (it == s.playerBroke.end() || it->second) continue; // not a player, or creative
            anySurvival = true;
            if (cb::hasPulley(st.part)) ++shafts;
        }
        if (anySurvival) {
            if (auto c = makeStack(kBeltConnectorId)) spawnItem(r.pos, r.pos.x + .5, r.pos.y + .5, r.pos.z + .5, *c);
            if (shafts > 0)
                if (auto sh = shaftItem(shafts)) spawnItem(r.pos, r.pos.x + .5, r.pos.y + .5, r.pos.z + .5, *sh);
        }
        // segments blown up: Create's loot (a connector, plus a shaft for a pulley) of each exploded
        // segment, each surviving the explosion with a chance of 1 / radius (survives_explosion)
        int blown = 0;
        for (auto const& [p, st] : gone) {
            auto it = s.exploded.find(p);
            if (it == s.exploded.end()) continue;
            ++blown;
            float chance = it->second > 1 ? 1.f / it->second : 1.f;
            auto  roll   = [&] { return std::uniform_real_distribution<float>(0.f, 1.f)(explosionRng()) < chance; };
            if (roll())
                if (auto c = makeStack(kBeltConnectorId)) spawnItem(p, p.x + .5, p.y + .5, p.z + .5, *c);
            if (cb::hasPulley(st.part) && roll())
                if (auto sh = shaftItem(1)) spawnItem(p, p.x + .5, p.y + .5, p.z + .5, *sh);
        }
        // segments a `/setblock` / `/fill` in destroy mode removed: their loot, as Create's
        // destroyBlock(pos, true)
        for (auto const& [p, st] : gone) {
            if (!s.commandDestroyed.contains(p) || s.exploded.contains(p)) continue;
            if (auto c = makeStack(kBeltConnectorId)) spawnItem(p, p.x + .5, p.y + .5, p.z + .5, *c);
            if (cb::hasPulley(st.part))
                if (auto sh = shaftItem(1)) spawnItem(p, p.x + .5, p.y + .5, p.z + .5, *sh);
            log().info("belts: belt segment at {} destroyed by a command", posText(p));
        }
        s.blownSegments += std::uint64_t(blown);
        if (blown > 0) log().info("belts: {} belt segment(s) at {} blown up", blown, posText(r.pos));
        log().info("belts: belt broken at {} ({} blocks)", posText(r.pos), gone.size() + world.size());
    }
}

} // namespace lv::feature::belts
