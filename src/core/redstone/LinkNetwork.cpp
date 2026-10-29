#include "core/redstone/LinkNetwork.h"

#include <algorithm>

namespace lv::core::redstone {

bool LinkNetwork::withinRange(LinkNode const& a, LinkNode const& b, int range) {
    if (a.dim != b.dim) return false;
    long long dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    long long r  = range;
    return dx * dx + dy * dy + dz * dz < r * r;
}

std::pair<LinkId, std::vector<Delivery>> LinkNetwork::add(LinkNode node) {
    LinkId id = mNextId++;
    node.transmit = std::clamp(node.transmit, 0, 15);
    auto& members = mNetworks[{node.dim, node.key}];
    members.push_back(id);
    auto const& stored = mNodes.emplace(id, std::move(node)).first->second;
    return {id, evaluate(stored, id)};
}

std::vector<Delivery> LinkNetwork::remove(LinkId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return {};
    LinkNode node = it->second;
    mNodes.erase(it);
    auto net = mNetworks.find({node.dim, node.key});
    if (net == mNetworks.end()) return {};
    std::erase(net->second, id);
    if (net->second.empty()) {
        mNetworks.erase(net);
        return {};
    }
    return evaluate(node, std::nullopt);
}

std::vector<Delivery> LinkNetwork::update(LinkId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return {};
    return evaluate(it->second, id);
}

std::vector<Delivery> LinkNetwork::setTransmit(LinkId id, int strength) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return {};
    it->second.transmit = std::clamp(strength, 0, 15);
    return evaluate(it->second, id);
}

std::vector<Delivery> LinkNetwork::setKey(LinkId id, LinkKey key) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || it->second.key == key) return {};
    LinkNode node = it->second;
    // leave the old network (its members are updated around this node) ...
    auto out = remove(id);
    // ... and join the new one under the same id
    node.key = std::move(key);
    mNetworks[{node.dim, node.key}].push_back(id);
    auto const& stored = mNodes.emplace(id, std::move(node)).first->second;
    auto        more   = evaluate(stored, id);
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

std::vector<Delivery> LinkNetwork::setListening(LinkId id, bool listening, int transmit) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return {};
    it->second.listening = listening;
    it->second.transmit  = std::clamp(transmit, 0, 15);
    return evaluate(it->second, id);
}

void LinkNetwork::move(LinkId id, int dim, int x, int y, int z) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    auto& n = it->second;
    if (n.dim != dim) {
        auto net = mNetworks.find({n.dim, n.key});
        if (net != mNetworks.end()) {
            std::erase(net->second, id);
            if (net->second.empty()) mNetworks.erase(net);
        }
        mNetworks[{dim, n.key}].push_back(id);
    }
    n.dim = dim, n.x = x, n.y = y, n.z = z;
}

LinkNode const* LinkNetwork::node(LinkId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? nullptr : &it->second;
}

std::vector<LinkId> LinkNetwork::members(int dim, LinkKey const& key) const {
    auto it = mNetworks.find({dim, key});
    return it == mNetworks.end() ? std::vector<LinkId>{} : it->second;
}

int LinkNetwork::powerAt(int dim, LinkKey const& key, int x, int y, int z) const {
    LinkNode probe;
    probe.dim = dim, probe.x = x, probe.y = y, probe.z = z;
    int  power = 0;
    auto net   = mNetworks.find({dim, key});
    if (net == mNetworks.end()) return 0;
    for (auto id : net->second) {
        auto const& other = mNodes.at(id);
        if (withinRange(probe, other, mRange)) power = std::max(power, other.listening ? 0 : other.transmit);
    }
    return power;
}

bool LinkNetwork::anyPower(LinkKey const& key) const {
    for (auto const& [nk, ids] : mNetworks) {
        if (nk.second != key) continue;
        for (auto id : ids) {
            auto const& n = mNodes.at(id);
            if (!n.listening && n.transmit > 0) return true;
        }
    }
    return false;
}

void LinkNetwork::clear() {
    mNodes.clear();
    mNetworks.clear();
}

std::vector<Delivery> LinkNetwork::evaluate(LinkNode const& actor, std::optional<LinkId> actorId) {
    std::vector<Delivery> out;
    auto net = mNetworks.find({actor.dim, actor.key});
    if (net == mNetworks.end()) return out;
    int power = 0;
    for (auto id : net->second) {
        auto const& other = mNodes.at(id);
        bool inRange = (actorId && id == *actorId) || withinRange(actor, other, mRange);
        if (!inRange) continue;
        if (power < 15) power = std::max(power, other.listening ? 0 : other.transmit);
    }
    if (actorId && actor.listening) out.push_back({*actorId, power});
    for (auto id : net->second) {
        if (actorId && id == *actorId) continue;
        auto const& other = mNodes.at(id);
        if (other.listening && withinRange(actor, other, mRange)) out.push_back({id, power});
    }
    return out;
}

} // namespace lv::core::redstone
