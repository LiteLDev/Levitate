// Redstone unit (U23): link networks (RedstoneLink.h) and the redstone link block.

#include <algorithm>
#include <unordered_map>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "feature/redstone/RedstoneLink.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::redstone {

namespace {

struct Links {
    cr::LinkNetwork                                       net;
    std::unordered_map<cr::LinkId, std::function<void(int)>> callbacks;
    std::unordered_map<cr::LinkId, int>                   received;
};
Links& links() {
    static Links l;
    return l;
}

void deliver(std::vector<cr::Delivery> const& out) {
    auto& l = links();
    for (auto const& d : out) {
        l.received[d.id] = d.power;
        auto it = l.callbacks.find(d.id);
        if (it == l.callbacks.end() || !it->second) continue;
        auto fn = it->second; // the callback may add / remove nodes (and so change the map)
        fn(d.power);
    }
}

void syncRange() { links().net.setRange(std::max(1, config::get<RedstoneConfig>().linkRange)); }

std::string posText(GP const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

} // namespace

cr::LinkNetwork& linkNetwork() { return links().net; }

void resetLinks() {
    auto& l = links();
    l.net.clear();
    l.callbacks.clear();
    l.received.clear();
}

// ---- public API (RedstoneLink.h) ---------------------------------------------------------------
namespace link {

std::string frequencyOf(ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return {};
    std::string key = common::items::idOf(stack);
    // Create: the item plus the dye colour of dyed items (leather armour, ...)
    if (auto const* tag = common::items::userData(stack); tag && tag->contains("customColor")) {
        auto const& c = (*tag)["customColor"];
        if (c.hold(Tag::Type::Int)) key += "#" + std::to_string(static_cast<std::uint32_t>(c.get<IntTag>().data));
    }
    return key;
}

LinkKey keyOf(ItemStack const& first, ItemStack const& last) { return {frequencyOf(first), frequencyOf(last)}; }

LinkId add(Node node) {
    syncRange();
    auto& l = links();
    cr::LinkNode n{node.pos.dim, node.pos.x, node.pos.y, node.pos.z, std::move(node.key), node.listening, node.transmit};
    auto [nid, out] = l.net.add(std::move(n));
    // the callback is in place before the deliveries of the first evaluation are handed out
    l.callbacks[nid] = std::move(node.onReceive);
    l.received[nid]  = 0;
    deliver(out);
    return nid;
}

void remove(LinkId id) {
    auto& l = links();
    auto  out = l.net.remove(id);
    l.callbacks.erase(id);
    l.received.erase(id);
    deliver(out);
}

void setTransmit(LinkId id, int strength) {
    syncRange();
    deliver(links().net.setTransmit(id, strength));
}

void setKey(LinkId id, LinkKey key) {
    syncRange();
    deliver(links().net.setKey(id, std::move(key)));
}

void setListening(LinkId id, bool listening, int transmit) {
    syncRange();
    if (!listening) links().received[id] = 0;
    deliver(links().net.setListening(id, listening, transmit));
}

void move(LinkId id, core::GlobalPos const& pos, bool update) {
    links().net.move(id, pos.dim, pos.x, pos.y, pos.z);
    if (update) {
        syncRange();
        deliver(links().net.update(id));
    }
}

int received(LinkId id) {
    auto it = links().received.find(id);
    return it == links().received.end() ? 0 : it->second;
}

int powerAt(core::GlobalPos const& pos, LinkKey const& key) {
    syncRange();
    return links().net.powerAt(pos.dim, key, pos.x, pos.y, pos.z);
}

bool anyPower(LinkKey const& key) { return links().net.anyPower(key); }

bool hasListeningLinks(int dim, LinkKey const& key) {
    auto const& net = links().net;
    for (auto id : net.members(dim, key))
        if (auto const* n = net.node(id); n && n->listening) return true;
    return false;
}

int range() { return std::max(1, config::get<RedstoneConfig>().linkRange); }

} // namespace link

// ---- the link block ----------------------------------------------------------------------------

core::common::Vec frequencySlotCentre(int facing, bool first) {
    constexpr double px = 1.0 / 16.0;
    switch (facing) {
    case kUp: return {0.5, 3.01 * px, (first ? 10.5 : 5.5) * px};
    case kDown: return {0.5, 12.99 * px, (first ? 5.5 : 10.5) * px};
    default: break;
    }
    double y = (first ? 10.5 : 5.5) * px;
    switch (facing) {
    case kSouth: return {0.5, y, 3.01 * px};
    case kNorth: return {0.5, y, 12.99 * px};
    case kEast: return {3.01 * px, y, 0.5};
    default: return {12.99 * px, y, 0.5}; // west
    }
}

std::optional<bool> frequencySlotAt(Block const& block, core::common::Vec const& hit) {
    int    facing = state(block, sFacing, kUp);
    auto   dist   = [&](core::common::Vec const& c) {
        double dx = hit[0] - c[0], dy = hit[1] - c[1], dz = hit[2] - c[2];
        return dx * dx + dy * dy + dz * dz;
    };
    auto first = frequencySlotCentre(facing, true), second = frequencySlotCentre(facing, false);
    bool hitFirst = core::common::testBoxHit(hit, first, kFrequencySlotScale);
    bool hitSecond = core::common::testBoxHit(hit, second, kFrequencySlotScale);
    if (hitFirst && hitSecond) return dist(first) <= dist(second);
    if (hitFirst) return true;
    if (hitSecond) return false;
    return std::nullopt;
}

std::pair<ItemStack, ItemStack> frequencies(GP const& pos) {
    auto const* d = dataOf(pos);
    if (!d) return {common::items::empty(), common::items::empty()};
    return {common::items::fromJson(d->value("first", Json())), common::items::fromJson(d->value("last", Json()))};
}

void setFrequency(GP const& pos, bool first, ItemStack const& stack) {
    auto copy    = common::items::withCount(stack, 1);
    auto [a, b]  = frequencies(pos);
    auto& before = first ? a : b;
    bool changed = !(common::items::isEmpty(copy) && common::items::isEmpty(before)) && !common::items::sameItemSameData(copy, before);
    auto& d      = dataFor(pos);
    if (common::items::isEmpty(copy)) d.erase(first ? "first" : "last");
    else d[first ? "first" : "last"] = common::items::toJson(copy);
    if (!changed) return;
    itemrender::refresh(pos); // the client mod draws the new frequency item
    if (auto* rt = runtimeAt(pos); rt && rt->link) {
        auto [na, nb] = frequencies(pos);
        link::setKey(*rt->link, link::keyOf(na, nb));
    }
}

namespace {
/// Create's LinkRenderer: both frequency items on the link's front, in their slots (filter slot
/// size), facing out. Client mod only (display items); vanilla keeps the action-bar message.
void linkDisplay(GP const& pos, Block const& block, std::vector<std::pair<std::int64_t, itemrender::DisplayItem>>& out) {
    static constexpr int kNormal[6][3]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    int  facing = std::clamp(state(block, sFacing, kUp), 0, 5);
    auto freq   = frequencies(pos);
    for (bool first : {true, false}) {
        auto const& stack = first ? freq.first : freq.second;
        if (common::items::isEmpty(stack)) continue;
        auto                    c = frequencySlotCentre(facing, first);
        itemrender::DisplayItem d;
        d.stack    = stack;
        d.at       = itemrender::facingFace(facing);
        d.at.x     = pos.x + c[0] + kNormal[facing][0] * .01;
        d.at.y     = pos.y + c[1] + kNormal[facing][1] * .01;
        d.at.z     = pos.z + c[2] + kNormal[facing][2] * .01;
        d.at.scale = static_cast<float>(kFrequencySlotScale) * .5f;
        out.emplace_back(first ? 0 : 1, std::move(d));
    }
}
struct LinkItems {
    LinkItems() { itemrender::registerProvider(kLink, linkDisplay); }
} const gLinkItems;
} // namespace

int linkInput(BlockSource& region, BlockPos const& pos, int facing) {
    // Create's RedstoneLinkBlock.getPower: the signal from every side, plus (except from the
    // block it hangs on) wires and powered blocks next to it whatever their direction
    int attached = opposite(facing);
    int power    = 0;
    for (int d = 0; d < 6 && power < 15; ++d) {
        power = std::max(power, signalFrom(region, pos, d));
        if (d != attached) power = std::max(power, undirectedSignal(region, pos, d));
    }
    return std::min(power, 15);
}

void linkLoaded(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    (void)region;
    if (rt.link) return;
    auto [a, b]  = frequencies(pos);
    bool receiver = state(block, sReceiver) != 0;
    auto const* d = dataOf(pos);
    link::Node n;
    n.pos       = pos;
    n.key       = link::keyOf(a, b);
    n.listening = receiver;
    n.transmit  = receiver ? 0 : (d ? d->value("transmit", 0) : 0);
    n.onReceive = [pos](int power) {
        if (auto* r = runtimeAt(pos)) r->received = power;
    };
    rt.link = link::add(std::move(n));
}

void linkUnloaded(GP const& pos, Runtime& rt) {
    (void)pos;
    if (!rt.link) return;
    link::remove(*rt.link);
    rt.link.reset();
    rt.received = 0;
}

void toggleLinkMode(BlockSource& region, GP const& pos) {
    auto        bp          = game::blockPos(pos);
    auto const& block       = region.getBlock(bp);
    bool        wasReceiver = state(block, sReceiver) != 0;
    bool        powered     = bestNeighborSignal(region, bp) > 0;
    setStates(region, bp, {{sReceiver, wasReceiver ? 0 : 1}, {sPowered, powered ? 1 : 0}, {sPower, 0}});
    dataFor(pos)["transmit"] = 0;
    if (auto* rt = runtimeAt(pos); rt && rt->link) {
        rt->received = 0;
        link::setListening(*rt->link, !wasReceiver, 0);
    }
    log().info("redstone link at {} is now a {}", posText(pos), wasReceiver ? "transmitter" : "receiver");
}

void tickLink(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    if (!rt.link) linkLoaded(region, pos, block, rt);
    auto bp       = game::blockPos(pos);
    bool receiver = state(block, sReceiver) != 0;
    if (!receiver) {
        int  power  = rt.inLink;
        auto const* d = dataOf(pos);
        int  stored = d ? d->value("transmit", 0) : 0;
        setStates(region, bp, {{sPowered, power > 0 ? 1 : 0}, {sPower, 0}});
        if (power != stored) {
            dataFor(pos)["transmit"] = power;
            if (rt.link) link::setTransmit(*rt.link, power);
        }
        return;
    }
    int r = std::clamp(rt.received, 0, 15);
    if (state(block, sPower) != r || (state(block, sPowered) != 0) != (r > 0)) {
        setStates(region, bp, {{sPowered, r > 0 ? 1 : 0}, {sPower, r}});
    }
}

} // namespace lv::feature::redstone
