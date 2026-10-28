#include "core/pipes/Flow.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>

namespace lv::core::pipes {

float flowSpeed(float pressure) { return 1.f / 32 + std::clamp(pressure / 128.f, 0.f, 1.f) * 31.f / 32; }
int   transferSpeedFor(float pressure) { return std::max(1, static_cast<int>(pressure / 2.f)); }

Fluid Connection::providedFluid() const {
    if (!flow || !flow->inbound || !flow->complete) return {};
    return flow->fluid;
}

Fluid Connection::providedOutboundFluid() const {
    if (!flow || flow->inbound || !flow->complete) return {};
    return flow->fluid;
}

bool Node::active() const {
    if (phase != Phase::Idle) return true;
    for (auto const& c : connections)
        if (c && (c->hasPressure() || c->flow || c->network)) return true;
    return false;
}

// ---- nodes ----------------------------------------------------------------------------------------------

Node* Model::node(GlobalPos const& pos) {
    auto it = mNodes.find(pos);
    return it == mNodes.end() ? nullptr : &it->second;
}

Node const* Model::node(GlobalPos const& pos) const {
    auto it = mNodes.find(pos);
    return it == mNodes.end() ? nullptr : &it->second;
}

void Model::createConnections(GlobalPos const& pos, Node& node) {
    for (Face f : kFaces) {
        auto& slot = node.connections[static_cast<int>(f)];
        bool  open = mHost.canHaveFlowToward(pos, f);
        if (!open) slot.reset();
        else if (!slot) {
            slot.emplace();
            slot->side = f;
        }
    }
}

Node* Model::ensure(GlobalPos const& pos) {
    if (auto* n = node(pos)) return n;
    if (!mHost.loaded(pos) || mHost.kind(pos) == Kind::None) return nullptr;
    auto& n = mNodes[pos];
    createConnections(pos, n);
    return &n;
}

Connection* Model::connection(BlockFace const& face) {
    auto* n = ensure(face.pos);
    return n ? n->at(face.face) : nullptr;
}

void Model::remove(GlobalPos const& pos) {
    mNodes.erase(pos);
    mPumps.erase(pos);
    mPumpUpdates.erase(pos);
}

void Model::clear() {
    mNodes.clear();
    mPumps.clear();
    mPumpUpdates.clear();
}

// ---- pressure -----------------------------------------------------------------------------------------

void Model::wipeConnection(Connection& c) {
    c.pressureIn = c.pressureOut = 0;
    if (c.source) {
        c.previousSource = c.source;
        c.source.reset();
    }
    if (c.network) c.network->reset();
}

void Model::wipePressure(GlobalPos const& pos) {
    auto* n = ensure(pos);
    if (!n) return;
    createConnections(pos, *n);
    n->phase = Phase::WaitForPumps;
    for (auto& c : n->connections)
        if (c) wipeConnection(*c);
}

void Model::addPressure(GlobalPos const& pos, Face side, bool inbound, float amount) {
    auto* n = ensure(pos);
    if (!n) return;
    auto* c = n->at(side);
    if (!c) return;
    (inbound ? c->pressureIn : c->pressureOut) += amount;
}

void Model::propagateChangedPipe(GlobalPos const& start, std::optional<FaceMask> startFaces) {
    std::deque<std::pair<int, GlobalPos>> frontier{{0, start}};
    std::set<GlobalPos>                   visited;
    std::set<BlockFace>                   discoveredPumps;
    int const                             range = mHost.pumpRange();

    while (!frontier.empty()) {
        auto [distance, current] = frontier.front();
        frontier.pop_front();
        if (!visited.insert(current).second) continue;

        FaceMask faces = 0;
        if (current == start && startFaces) {
            faces = *startFaces;
            if (node(current)) wipePressure(current);
        } else {
            if (mHost.kind(current) == Kind::None) continue;
            auto* n = ensure(current);
            if (!n) continue;
            wipePressure(current);
            for (Face f : kFaces)
                if (n->at(f)) faces |= bit(f);
        }
        for (Face f : kFaces) {
            if (!has(faces, f)) continue;
            GlobalPos target = shifted(current, f);
            if (!mHost.loaded(target)) continue;
            Kind k = mHost.kind(target);
            if (k == Kind::Pump) {
                if (axisOf(mHost.pumpFront(target)) == axisOf(f)) discoveredPumps.insert({target, opposite(f)});
                continue;
            }
            if (visited.contains(target) || k == Kind::None) continue;
            bool pressured = false;
            if (auto const* tn = node(target))
                for (auto const& c : tn->connections) pressured |= c && c->hasPressure();
            if (distance >= range && !pressured) continue;
            if (mHost.canHaveFlowToward(target, opposite(f))) frontier.emplace_back(distance + 1, target);
        }
    }
    for (auto const& [pump, side] : discoveredPumps) {
        if (axisOf(side) != axisOf(mHost.pumpFront(pump))) continue;
        addPump(pump);
        auto& flags = mPumpUpdates[pump];
        (side == mHost.pumpFront(pump) ? flags.front : flags.back) = true;
        wipePressure(pump);
    }
}

void Model::resetAffectedFluidNetworks(GlobalPos const& start, Face side) {
    std::deque<GlobalPos> frontier{start};
    std::set<GlobalPos>   visited;
    while (!frontier.empty()) {
        GlobalPos pos = frontier.front();
        frontier.pop_front();
        if (!visited.insert(pos).second) continue;
        auto* n = node(pos);
        if (!n) continue;
        for (Face d : kFaces) {
            if (pos == start && d != side) continue;
            auto* c = n->at(d);
            if (!c || !c->flow || !c->flow->inbound) continue;
            if (c->network) c->network->reset();
            frontier.push_back(shifted(pos, d));
        }
    }
}

// ---- pumps ---------------------------------------------------------------------------------------------

void Model::addPump(GlobalPos const& pos) { mPumps.insert(pos); }

bool Model::isPullingOnSide(GlobalPos const& pump, Face side) { return side != mHost.pumpFront(pump); }

void Model::pumpSpeedChanged(GlobalPos const& pos, float previous, float speed) {
    addPump(pos);
    if (std::abs(previous) == std::abs(speed)) return;
    pumpUpdatePressure(pos);
}

void Model::pumpUpdatePressure(GlobalPos const& pos) {
    addPump(pos);
    mPumpUpdates[pos] = {};
    Face front        = mHost.pumpFront(pos);
    propagateChangedPipe(shifted(pos, front));
    propagateChangedPipe(shifted(pos, opposite(front)));
    wipePressure(pos);
    mPumpUpdates[pos] = {true, true};
}

bool Model::hasReachedValidEndpoint(BlockFace const& face, bool pull) {
    GlobalPos connected = face.connected();
    if (!mHost.loaded(connected)) return false;
    Kind k = mHost.kind(connected);
    if (k == Kind::Pump && axisOf(mHost.pumpFront(connected)) == axisOf(face.face))
        return isPullingOnSide(connected, opposite(face.face)) != pull;
    if (k != Kind::None && mHost.canHaveFlowToward(connected, opposite(face.face))) return false;
    if (mHost.hasHandler(connected, opposite(face.face))) return true;
    return mHost.isOpenEnd(face.pos, face.face);
}

void Model::distributePressureTo(GlobalPos const& pump, Face side) {
    float speed = std::abs(mHost.pumpSpeed(pump));
    if (speed == 0) return;
    BlockFace start{pump, side};
    bool      pull = isPullingOnSide(pump, side);

    using Edges = std::map<Face, bool>;
    std::map<GlobalPos, std::pair<int, Edges>> graph;
    std::set<BlockFace>                        targets;
    auto nodeAt = [&](GlobalPos const& p, int distance) -> Edges& {
        auto it = graph.find(p);
        if (it == graph.end()) it = graph.emplace(p, std::pair<int, Edges>{distance, {}}).first;
        return it->second.second;
    };

    if (!pull) resetAffectedFluidNetworks(pump, opposite(side));

    if (!hasReachedValidEndpoint(start, pull)) {
        nodeAt(pump, 0)[side]                        = pull;
        nodeAt(start.connected(), 1)[opposite(side)] = !pull;
        std::deque<std::pair<int, GlobalPos>> frontier{{1, start.connected()}};
        std::set<GlobalPos>                   visited;
        int const                             maxDistance = mHost.pumpRange();
        while (!frontier.empty()) {
            auto [distance, current] = frontier.front();
            frontier.pop_front();
            if (!mHost.loaded(current)) continue;
            if (!visited.insert(current).second) continue;
            if (mHost.kind(current) == Kind::None) continue;
            for (Face face : kFaces) {
                if (!mHost.canHaveFlowToward(current, face)) continue;
                BlockFace blockFace{current, face};
                GlobalPos connected = blockFace.connected();
                if (!mHost.loaded(connected)) continue;
                if (connected == pump) continue; // the face pointing back at the pump
                if (hasReachedValidEndpoint(blockFace, pull)) {
                    nodeAt(current, distance)[face] = pull;
                    targets.insert(blockFace);
                    continue;
                }
                Kind k = mHost.kind(connected);
                if (k != Kind::Pipe) continue; // no pipe there, or another pump
                if (visited.contains(connected)) continue;
                if (distance + 1 >= maxDistance) {
                    nodeAt(current, distance)[face] = pull;
                    targets.insert(blockFace);
                    continue;
                }
                nodeAt(current, distance)[face]               = pull;
                nodeAt(connected, distance + 1)[opposite(face)] = !pull;
                frontier.emplace_back(distance + 1, connected);
            }
        }
    }

    // faces on a path from the pump to a target, grouped by distance
    std::map<int, std::set<BlockFace>>    validFaces;
    std::map<BlockFace, bool>             memo;
    std::function<bool(BlockFace const&)> search = [&](BlockFace const& current) -> bool {
        if (auto m = memo.find(current); m != memo.end()) return m->second;
        auto it = graph.find(current.pos);
        if (it == graph.end()) return false;
        memo[current]   = false; // guards against cycles
        int  distance   = it->second.first;
        bool success    = false;
        auto const edges = it->second.second;
        for (Face d : kFaces) {
            if (d == current.face) continue;
            auto e = edges.find(d);
            if (e == edges.end()) continue;
            BlockFace face{current.pos, d};
            if (targets.contains(face)) {
                validFaces[distance].insert(face);
                success = true;
                continue;
            }
            if (e->second != pull) continue;
            if (search({face.connected(), opposite(d)})) {
                validFaces[distance].insert(face);
                success = true;
            }
        }
        if (success) validFaces[distance].insert(current);
        memo[current] = success;
        return success;
    };
    search({pump, opposite(side)});

    for (auto const& [distance, faces] : validFaces) {
        int parallelBranches = std::max(1, static_cast<int>(faces.size()) - 1);
        for (auto const& face : faces) {
            if (face.pos == pump) continue;
            auto g = graph.find(face.pos);
            if (g == graph.end()) continue;
            auto e = g->second.second.find(face.face);
            if (e == g->second.second.end()) continue;
            addPressure(face.pos, face.face, e->second, speed / static_cast<float>(parallelBranches));
        }
    }
}

void Model::tickPump(GlobalPos const& pos) {
    auto* n = ensure(pos);
    if (!n) return;
    tickNode(pos, *n);
    n = node(pos);
    if (!n) return;
    float speed = std::abs(mHost.pumpSpeed(pos));
    for (auto& c : n->connections) {
        if (!c) continue;
        bool pull     = isPullingOnSide(pos, c->side);
        c->pressureIn  = pull ? speed : 0.f;
        c->pressureOut = pull ? 0.f : speed;
    }
    auto it = mPumpUpdates.find(pos);
    if (it == mPumpUpdates.end()) return;
    Face front = mHost.pumpFront(pos);
    for (bool isFront : {true, false}) {
        bool& flag = isFront ? it->second.front : it->second.back;
        if (!flag) continue;
        flag = false;
        distributePressureTo(pos, isFront ? front : opposite(front));
    }
}

// ---- connections -------------------------------------------------------------------------------------

bool Model::determineSource(GlobalPos const& pos, Connection& c) {
    GlobalPos neighbour = shifted(pos, c.side);
    if (!mHost.loaded(neighbour)) return false;
    if (mHost.isOpenEnd(pos, c.side)) {
        if (c.previousSource && c.previousSource->kind == SourceKind::OpenEnd) c.source = c.previousSource;
        else c.source = Source{SourceKind::OpenEnd, {}};
        return true;
    }
    if (mHost.hasHandler(neighbour, opposite(c.side))) {
        c.source = Source{SourceKind::Handler, {}};
        return true;
    }
    c.source = Source{mHost.kind(neighbour) == Kind::None ? SourceKind::Blocked : SourceKind::OtherPipe, {}};
    return true;
}

Fluid Model::provideFluid(GlobalPos const& pos, Connection& c, std::function<bool(Fluid const&)> const& pred) {
    if (!c.source) return {};
    BlockFace at{pos, c.side};
    switch (c.source->kind) {
    case SourceKind::OtherPipe: {
        auto* n = node(at.connected());
        auto* o = n ? n->at(opposite(c.side)) : nullptr;
        if (!o) return {};
        Fluid f = o->providedOutboundFluid();
        return !f.empty() && pred(f) ? f : Fluid{};
    }
    case SourceKind::Blocked:
        return {};
    default:
        break;
    }
    bool open      = c.source->kind == SourceKind::OpenEnd;
    auto drain     = [&](Fluid const& request) {
        return open ? mHost.openEndDrain(at, c.source->openEnd, request, true)
                        : mHost.handlerDrain(at.connected(), opposite(c.side), request, true);
    };
    Fluid immediate = drain({"", 1});
    if (!immediate.empty() && pred(immediate)) return immediate;
    std::vector<Fluid> contents =
        open ? std::vector<Fluid>{c.source->openEnd.tank} : mHost.handlerTanks(at.connected(), opposite(c.side));
    for (auto const& contained : contents) {
        if (contained.empty() || !pred(contained)) continue;
        return drain(contained.withAmount(1));
    }
    return {};
}

bool Model::flipFlowsIfPressureReversed(Connection& c) {
    if (!c.flow) return false;
    bool singlePressure = c.comparePressure() != 0 && (c.pressureIn == 0 || c.pressureOut == 0);
    if (!singlePressure || (c.comparePressure() < 0) == c.flow->inbound) return false;
    c.flow->inbound = !c.flow->inbound;
    if (!c.flow->complete) c.flow.reset();
    return true;
}

bool Model::manageFlows(GlobalPos const& pos, Connection& c, Fluid const& internal, std::function<bool(Fluid const&)> const& pred) {
    auto retained = std::move(c.network);
    c.network.reset();
    if (!c.source && !determineSource(pos, c)) return false;

    auto start = [&](bool inbound, Fluid const& provided) {
        if (provided.empty()) return false;
        c.flow = Flow{inbound, provided, 0.f, false};
        return true;
    };

    if (!c.flow) {
        if (!c.hasPressure()) return false;
        bool prioritizeInbound = c.comparePressure() < 0;
        for (bool trueFalse : {true, false}) {
            bool inbound = prioritizeInbound == trueFalse;
            if (c.pressure(inbound) == 0) continue;
            if (start(inbound, inbound ? provideFluid(pos, c, pred) : internal)) return true;
        }
        return false;
    }

    Fluid provided = c.flow->inbound ? provideFluid(pos, c, pred) : internal;
    if (!c.hasPressure() || provided.empty() || !sameFluid(provided, c.flow->fluid)) {
        c.flow.reset();
        return true;
    }
    if (c.flow->inbound != (c.comparePressure() < 0)) {
        bool inbound = !c.flow->inbound;
        if ((inbound && !provided.empty()) || (!inbound && !internal.empty())) {
            resetAffectedFluidNetworks(pos, c.side);
            start(inbound, inbound ? provideFluid(pos, c, pred) : internal);
            return true;
        }
    }
    if (c.source->kind == SourceKind::OpenEnd) c.source->openEnd.pulling = c.flow->inbound;
    if (!c.source->endpoint() || !c.flow->inbound) return false;

    c.network = retained ? std::move(retained) : std::make_shared<Network>(*this, BlockFace{pos, c.side});
    auto network = c.network; // the tick may reset or replace connections
    network->tick();
    return false;
}

void Model::tickFlowProgress(Connection& c) {
    if (!c.flow || c.flow->fluid.empty() || c.flow->complete) return;
    float speed      = flowSpeed(c.pressure(c.flow->inbound));
    c.flow->progress = std::min(c.flow->progress + speed, 1.f);
    if (c.flow->progress >= 1.f) c.flow->complete = true;
}

void Model::tickNode(GlobalPos const& pos, Node& n) {
    if (n.phase == Phase::WaitForPumps) {
        n.phase = Phase::FlipFlows;
        return;
    }
    bool changed = false;
    for (auto& c : n.connections) {
        if (!c) continue;
        changed |= flipFlowsIfPressureReversed(*c);
        if (!c->source) determineSource(pos, *c);
    }
    if (changed) mHost.onFlowsChanged(pos);
    if (n.phase == Phase::FlipFlows) {
        n.phase = Phase::Idle;
        return;
    }

    Fluid available, colliding;
    int   singleSource = -1;
    for (int i = 0; i < 6; ++i) {
        auto const& c = n.connections[i];
        if (!c) continue;
        Fluid provided = c->providedFluid();
        if (provided.empty()) continue;
        if (available.empty()) {
            singleSource = i;
            available    = provided;
            continue;
        }
        if (sameFluid(available, provided)) {
            singleSource = -1;
            available    = provided;
            continue;
        }
        colliding = provided;
        break;
    }
    if (!colliding.empty()) {
        mHost.onFlowCollision(pos, available, colliding);
        return;
    }

    changed = false;
    for (int i = 0; i < 6; ++i) {
        auto* nn = node(pos); // a network tick may have touched the map
        if (!nn) return;
        auto& c = nn->connections[i];
        if (!c) continue;
        Face  side     = c->side;
        Fluid internal = i == singleSource ? Fluid{} : available;
        changed |= manageFlows(pos, *c, internal, [this, pos, side](Fluid const& f) {
            return mHost.canPullFluidFrom(pos, side, f);
        });
    }
    if (changed) mHost.onFlowsChanged(pos);
    if (auto* nn = node(pos))
        for (auto& c : nn->connections)
            if (c) tickFlowProgress(*c);
}

void Model::tick(std::int64_t gameTime) {
    mGameTime = gameTime;
    for (auto pump : std::vector<GlobalPos>(mPumps.begin(), mPumps.end())) {
        if (!mHost.loaded(pump)) continue;
        if (mHost.kind(pump) != Kind::Pump) {
            remove(pump);
            continue;
        }
        tickPump(pump);
    }
    std::vector<GlobalPos> active;
    for (auto const& [pos, n] : mNodes)
        if (!mPumps.contains(pos) && n.active()) active.push_back(pos);
    for (auto const& pos : active) {
        if (!mHost.loaded(pos)) continue;
        auto* n = node(pos);
        if (n) tickNode(pos, *n);
    }
}

Fluid Model::visibleFluid(GlobalPos const& pos) const {
    auto const* n = node(pos);
    if (!n) return {};
    for (auto const& c : n->connections)
        if (c && c->flow && !c->flow->fluid.empty()) return c->flow->fluid;
    return {};
}

// ---- persistence ---------------------------------------------------------------------------------------

namespace {
nlohmann::json posJson(GlobalPos const& p) { return {p.dim, p.x, p.y, p.z}; }
GlobalPos      posFrom(nlohmann::json const& j) { return {j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>(), j.at(3).get<int>()}; }
} // namespace

nlohmann::json Model::save(std::optional<int> dim) const {
    nlohmann::json nodes = nlohmann::json::array();
    for (auto const& [pos, n] : mNodes) {
        if (dim && pos.dim != *dim) continue;
        nlohmann::json conns = nlohmann::json::object();
        for (auto const& c : n.connections) {
            if (!c) continue;
            nlohmann::json cj = nlohmann::json::object();
            if (c->hasPressure()) cj["p"] = {c->pressureIn, c->pressureOut};
            Source const* open = c->source && c->source->kind == SourceKind::OpenEnd ? &*c->source
                               : c->previousSource && c->previousSource->kind == SourceKind::OpenEnd ? &*c->previousSource
                                                                                                     : nullptr;
            if (open) cj["open"] = {open->openEnd.tank.id, open->openEnd.tank.amount, open->openEnd.pulling};
            if (c->flow) {
                nlohmann::json fj{{"fluid", c->flow->fluid.id}, {"in", c->flow->inbound}};
                if (!c->flow->complete) fj["progress"] = c->flow->progress;
                cj["flow"] = std::move(fj);
            }
            if (!cj.empty()) conns[std::to_string(static_cast<int>(c->side))] = std::move(cj);
        }
        if (conns.empty() && !mPumps.contains(pos)) continue;
        nodes.push_back({{"pos", posJson(pos)}, {"c", std::move(conns)}});
    }
    nlohmann::json pumps = nlohmann::json::array();
    for (auto const& p : mPumps)
        if (!dim || p.dim == *dim) pumps.push_back(posJson(p));
    return {{"nodes", std::move(nodes)}, {"pumps", std::move(pumps)}};
}

void Model::load(nlohmann::json const& j) {
    if (!j.is_object()) return;
    for (auto const& p : j.value("pumps", nlohmann::json::array())) mPumps.insert(posFrom(p));
    for (auto const& nj : j.value("nodes", nlohmann::json::array())) {
        GlobalPos pos = posFrom(nj.at("pos"));
        Node&     n   = mNodes[pos];
        n.phase       = Phase::WaitForPumps; // not saved, as Create
        nlohmann::json const conns = nj.value("c", nlohmann::json::object());
        for (auto const& [key, cj] : conns.items()) {
            int side = std::stoi(key);
            if (side < 0 || side > 5) continue;
            auto& c = n.connections[side];
            c.emplace();
            c->side = static_cast<Face>(side);
            if (cj.contains("p")) {
                c->pressureIn  = cj["p"].at(0).get<float>();
                c->pressureOut = cj["p"].at(1).get<float>();
            }
            if (cj.contains("open")) {
                Source s{SourceKind::OpenEnd, {}};
                s.openEnd.tank    = {cj["open"].at(0).get<std::string>(), cj["open"].at(1).get<int>()};
                s.openEnd.pulling = cj["open"].at(2).get<bool>();
                c->source         = s;
            }
            if (cj.contains("flow")) {
                auto const& fj = cj["flow"];
                Flow        f;
                f.fluid    = {fj.value("fluid", std::string{}), 1};
                f.inbound  = fj.value("in", true);
                f.complete = !fj.contains("progress");
                f.progress = f.complete ? 1.f : fj["progress"].get<float>();
                c->flow    = f;
            }
        }
    }
}

// ---- network ----------------------------------------------------------------------------------------

Network::Network(Model& model, BlockFace start) : mModel(model), mStart(start) { reset(); }

void Network::reset() {
    mFrontier.clear();
    mVisited.clear();
    mTargets.clear();
    mQueued.clear();
    mFluid = {};
    mQueued.push_back(mStart);
    mPause = 2;
}

std::string Network::HandlerRef::key() const {
    if (openEnd) return "o" + face.pos.key() + ":" + std::to_string(static_cast<int>(face.face));
    return "h" + face.connected().key();
}

std::optional<Network::HandlerRef> Network::handlerOf(BlockFace const& face) {
    auto* n = mModel.node(face.pos);
    auto* c = n ? n->at(face.face) : nullptr;
    if (!c || !c->source || !c->source->endpoint()) return std::nullopt;
    return HandlerRef{face, c->source->kind == SourceKind::OpenEnd};
}

std::vector<Fluid> Network::tanks(HandlerRef const& h) {
    if (!h.openEnd) return mModel.mHost.handlerTanks(h.face.connected(), opposite(h.face.face));
    auto* c = mModel.connection(h.face);
    return c && c->source ? std::vector<Fluid>{c->source->openEnd.tank} : std::vector<Fluid>{};
}

Fluid Network::drain(HandlerRef const& h, Fluid const& request, bool simulate) {
    if (!h.openEnd) return mModel.mHost.handlerDrain(h.face.connected(), opposite(h.face.face), request, simulate);
    auto* c = mModel.connection(h.face);
    if (!c || !c->source) return {};
    return mModel.mHost.openEndDrain(h.face, c->source->openEnd, request, simulate);
}

int Network::fill(HandlerRef const& h, Fluid const& fluid, bool simulate) {
    if (fluid.empty()) return 0;
    if (!h.openEnd) return mModel.mHost.handlerFill(h.face.connected(), opposite(h.face.face), fluid, simulate);
    auto* c = mModel.connection(h.face);
    if (!c || !c->source) return 0;
    return mModel.mHost.openEndFill(h.face, c->source->openEnd, fluid, simulate);
}

void Network::tick() {
    if (mPause > 0) {
        --mPause;
        return;
    }
    Host& host = mModel.mHost;
    for (int cycle = 0; cycle < 16; ++cycle) {
        bool shouldContinue = false;
        // queued faces enter the frontier once their block is loaded
        std::vector<BlockFace> stillQueued;
        for (auto const& face : mQueued) {
            if (!host.loaded(face.pos)) {
                stillQueued.push_back(face);
                continue;
            }
            auto* c = mModel.connection(face);
            if (!c) continue;
            if (face == mStart) mTransferSpeed = transferSpeedFor(c->pressureIn);
            if (std::find(mFrontier.begin(), mFrontier.end(), face) == mFrontier.end()) mFrontier.push_back(face);
        }
        mQueued = std::move(stillQueued);

        for (auto it = mFrontier.begin(); it != mFrontier.end();) {
            auto* c = mModel.connection(*it);
            if (!c) {
                it = mFrontier.erase(it);
                continue;
            }
            if (!c->flow) {
                ++it;
                continue;
            }
            Flow const& flow = *c->flow;
            if (!mFluid.empty() && !sameFluid(flow.fluid, mFluid)) {
                it = mFrontier.erase(it);
                continue;
            }
            if (!flow.inbound) {
                if (c->comparePressure() >= 0) it = mFrontier.erase(it);
                else ++it;
                continue;
            }
            if (!flow.complete) {
                ++it;
                continue;
            }
            if (mFluid.empty()) mFluid = flow.fluid;

            bool      canRemove = true;
            BlockFace current   = *it;
            for (Face side : kFaces) {
                if (side == current.face) continue;
                BlockFace adjacentFace{current.pos, side};
                auto*     adjacent = mModel.connection(adjacentFace);
                if (!adjacent) continue;
                if (!adjacent->flow) {
                    if (adjacent->hasPressure() && adjacent->pressureOut > 0) canRemove = false;
                    continue;
                }
                Flow const& out = *adjacent->flow;
                if (out.inbound) {
                    if (adjacent->comparePressure() > 0) canRemove = false;
                    continue;
                }
                if (!out.complete) {
                    canRemove = false;
                    continue;
                }
                if (!adjacent->source && !mModel.determineSource(current.pos, *adjacent)) {
                    canRemove = false;
                    continue;
                }
                if (adjacent->source && adjacent->source->endpoint()) {
                    if (std::find(mTargets.begin(), mTargets.end(), adjacentFace) == mTargets.end())
                        mTargets.push_back(adjacentFace);
                    continue;
                }
                if (mVisited.insert(adjacentFace.connected()).second) {
                    mQueued.push_back(adjacentFace.opposite());
                    shouldContinue = true;
                }
            }
            // `current` may have moved if the vector grew: find it again
            it = std::find(mFrontier.begin(), mFrontier.end(), current);
            if (it == mFrontier.end()) break;
            if (canRemove) it = mFrontier.erase(it);
            else ++it;
        }
        if (!shouldContinue) break;
    }

    if (!handlerOf(mStart) || mTargets.empty()) return;

    int                        speed = mTransferSpeed;
    std::map<std::string, int> accumulated;
    for (bool simulate : {true, false}) {
        auto source = handlerOf(mStart);
        if (!source) return;
        Fluid transfer;
        for (auto const& contained : tanks(*source)) {
            if (contained.empty() || !sameFluid(contained, mFluid)) continue;
            transfer = drain(*source, contained.withAmount(speed), simulate);
            break;
        }
        if (transfer.empty()) {
            Fluid generic = drain(*source, Fluid{"", speed}, simulate);
            if (!generic.empty() && sameFluid(generic, mFluid)) transfer = generic;
        }
        if (transfer.empty()) return;
        if (simulate) speed = transfer.amount;

        std::vector<BlockFace> outputs = mTargets;
        while (!outputs.empty() && transfer.amount > 0) {
            int n         = static_cast<int>(outputs.size());
            int divided   = transfer.amount / n;
            int remainder = transfer.amount % n;
            for (auto it = outputs.begin(); it != outputs.end();) {
                int toTransfer = divided;
                if (remainder > 0) {
                    ++toTransfer;
                    --remainder;
                }
                if (transfer.amount <= 0) break;
                auto target = handlerOf(*it);
                if (!target) {
                    it = outputs.erase(it);
                    continue;
                }
                int simulated = toTransfer;
                if (simulate) simulated += accumulated[target->key()];
                int filled = fill(*target, transfer.withAmount(simulated), simulate);
                if (simulate) {
                    accumulated[target->key()] = filled;
                    filled -= simulated - toTransfer;
                }
                transfer.amount -= filled;
                if (filled < toTransfer) it = outputs.erase(it);
                else ++it;
            }
        }
        speed -= transfer.amount;
        if (speed <= 0) return;
    }
}

} // namespace lv::core::pipes
