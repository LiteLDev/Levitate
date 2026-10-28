#include "core/kinetics/Solver.h"

#include <algorithm>
#include <cmath>
#include <deque>

#include "nlohmann/json.hpp"

namespace lv::core::kinetics {

namespace {
/// Safety net against propagation that never settles (Create would overflow its stack).
constexpr long kMaxPropagationSteps = 4'000'000;

float signum(float v) { return v > 0 ? 1.f : v < 0 ? -1.f : 0.f; }

nlohmann::json posJson(GlobalPos const& p) { return nlohmann::json::array({p.dim, p.x, p.y, p.z}); }
GlobalPos      posFrom(nlohmann::json const& j) {
    return GlobalPos{j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>(), j.at(3).get<int>()};
}
} // namespace

char const* reasonName(DestroyReason r) {
    switch (r) {
    case DestroyReason::Conflict:
        return "speed conflict";
    case DestroyReason::TooFast:
        return "too fast";
    case DestroyReason::RatioLoop:
        return "gear ratio loop";
    case DestroyReason::Flicker:
        return "speed changed too often";
    default:
        return "generator driven backwards";
    }
}

// ---- persistence --------------------------------------------------------------------------------------

void to_json(nlohmann::json& j, NodeRecord const& r) {
    j = nlohmann::json{{"p", posJson(r.pos)}, {"id", r.state.id}};
    if (!r.state.values.empty()) {
        auto& s = j["s"] = nlohmann::json::object();
        for (auto const& [k, v] : r.state.values) s[k] = v;
    }
    if (r.speed != 0) j["v"] = r.speed;
    if (r.generated != 0) j["g"] = r.generated;
    if (r.source) j["src"] = posJson(*r.source);
    if (r.network) j["net"] = posJson(*r.network);
    if (r.impactOverride) j["impact"] = *r.impactOverride;
    if (r.capacityOverride) j["capacity"] = *r.capacityOverride;
}

void from_json(nlohmann::json const& j, NodeRecord& r) {
    r.pos      = posFrom(j.at("p"));
    r.state.id = j.at("id").get<std::string>();
    r.state.values.clear();
    if (auto it = j.find("s"); it != j.end() && it->is_object()) {
        for (auto const& [k, v] : it->items()) r.state.values.emplace_back(k, v.get<int>());
    }
    r.speed     = j.value("v", 0.f);
    r.generated = j.value("g", 0.f);
    r.source.reset();
    r.network.reset();
    r.impactOverride.reset();
    r.capacityOverride.reset();
    if (auto it = j.find("src"); it != j.end()) r.source = posFrom(*it);
    if (auto it = j.find("net"); it != j.end()) r.network = posFrom(*it);
    if (auto it = j.find("impact"); it != j.end()) r.impactOverride = it->get<double>();
    if (auto it = j.find("capacity"); it != j.end()) r.capacityOverride = it->get<double>();
}

// ---- internal types -----------------------------------------------------------------------------------

struct Solver::Node {
    GlobalPos                pos;
    BlockState               state;
    KineticSpec const*       spec = nullptr;
    float                    speed = 0; ///< theoretical speed
    std::optional<GlobalPos> source;
    std::optional<NetworkId> network;
    float                    generated    = 0;
    int                      flicker      = 0;
    bool                     reactivate   = false;
    bool                     overstressed = false;
    bool                     doomed       = false;
    std::optional<double>    impactOverride, capacityOverride;
};

struct Solver::Network {
    std::unordered_set<GlobalPos, GlobalPosHash> members;
    double                                       stress       = 0;
    double                                       capacity     = 0;
    bool                                         overstressed = false;
};

struct Solver::Snapshot {
    bool                     existed     = false;
    float                    speed       = 0;
    float                    theoretical = 0;
    std::optional<GlobalPos> source;
    std::string              id;
};

Solver::Solver(SpecLookup specs, SolverConfig config) : mSpecs(std::move(specs)), mConfig(std::move(config)) {}
Solver::~Solver() = default;

void Solver::setConfig(SolverConfig config) {
    mConfig = std::move(config);
    refreshAllNetworks();
}

// ---- small helpers -------------------------------------------------------------------------------------

Solver::Node* Solver::find(GlobalPos const& pos) {
    auto it = mNodes.find(pos);
    return it == mNodes.end() ? nullptr : it->second.get();
}
Solver::Node const* Solver::find(GlobalPos const& pos) const {
    auto it = mNodes.find(pos);
    return it == mNodes.end() ? nullptr : it->second.get();
}
bool Solver::alive(Node const* n) const { return n && !n->doomed; }

NodeView Solver::view(Node const& n) const { return NodeView{n.pos, &n.state, n.speed, n.source, n.spec->generator}; }

double Solver::impactOf(Node const& n) const {
    if (n.impactOverride) return *n.impactOverride;
    return mConfig.impact ? mConfig.impact(*n.spec, n.state) : n.spec->impact;
}
double Solver::capacityOf(Node const& n) const {
    if (n.capacityOverride) return *n.capacityOverride;
    return mConfig.capacity ? mConfig.capacity(*n.spec, n.state) : n.spec->capacity;
}
float Solver::effective(Node const& n) const { return n.overstressed ? 0.f : n.speed; }

NodeInfo Solver::info(Node const& n) const {
    NodeInfo i;
    i.pos              = n.pos;
    i.state            = n.state;
    i.speed            = effective(n);
    i.theoreticalSpeed = n.speed;
    i.generatedSpeed   = n.generated;
    i.source           = n.source;
    i.network          = n.network;
    i.generator        = n.spec->generator;
    i.overstressed     = n.overstressed;
    i.impact           = impactOf(n);
    i.capacity         = n.spec->generator ? capacityOf(n) : 0;
    i.flicker          = n.flicker;
    return i;
}

void Solver::touch(Node const& n) {
    mTouched.try_emplace(n.pos, Snapshot{true, effective(n), n.speed, n.source, n.state.id});
}

void Solver::markDirty(std::optional<NetworkId> const& id) {
    if (id) mDirtyNetworks.insert(*id);
}

// ---- Create's RotationPropagator -----------------------------------------------------------------------

std::vector<GlobalPos> Solver::potentialNeighbourLocations(Node const& n) const {
    std::vector<GlobalPos> out;
    out.reserve(18);
    for (Face f : kFaces) out.push_back(shifted(n.pos, f));
    // Large cogs reach all 12 diagonal neighbours (Create's SimpleKineticBlockEntity, so that
    // perpendicular large cogs mesh); other diagonal blocks only those in the plane of their axis
    // (KineticBlockEntity.addPropagationLocations).
    bool large = n.spec->cogOf(n.state) == CogType::Large;
    auto axis  = n.spec->axisOf(n.state);
    if (large || (axis && n.spec->diagonal(n.state))) {
        // BlockPos.betweenClosed(-1,-1,-1 .. 1,1,1): x fastest, then y, then z.
        for (int z = -1; z <= 1; ++z) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    if (x * x + y * y + z * z != 2) continue;
                    if (!large && choose(*axis, x, y, z) != 0) continue;
                    out.push_back(shifted(n.pos, Offset{x, y, z}));
                }
            }
        }
    }
    if (n.spec->extraNeighbours) n.spec->extraNeighbours(view(n), out);
    return out;
}

float Solver::axisModifier(Node const& n, Face face) const {
    return n.spec->axisModifier ? n.spec->axisModifier(view(n), face) : 1.f;
}

namespace {
bool largeToSmall(KineticSpec const& large, BlockState const& ls, KineticSpec const& small, BlockState const& ss, Offset d) {
    auto axisFrom = large.axisOf(ls);
    auto axisTo   = small.axisOf(ss);
    if (!axisFrom || !axisTo || *axisFrom != *axisTo) return false;
    if (choose(*axisFrom, d) != 0) return false;
    for (Axis a : kAxes) {
        if (a == *axisFrom) continue;
        if (std::abs(choose(a, d)) != 1) return false;
    }
    return true;
}
bool largeToLarge(KineticSpec const& a, BlockState const& as, KineticSpec const& b, BlockState const& bs, Offset d) {
    if (a.cogOf(as) != CogType::Large || b.cogOf(bs) != CogType::Large) return false;
    auto fromAxis = a.axisOf(as);
    auto toAxis   = b.axisOf(bs);
    if (!fromAxis || !toAxis || *fromAxis == *toAxis) return false;
    for (Axis axis : kAxes) {
        int axisDiff = choose(axis, d);
        if (axis == *fromAxis || axis == *toAxis) {
            if (axisDiff == 0) return false;
        } else if (axisDiff != 0) {
            return false;
        }
    }
    return true;
}
} // namespace

float Solver::rotationSpeedModifier(Node const& from, Node const& to) const {
    auto const& sf = *from.spec;
    auto const& st = *to.spec;
    Offset      d  = diff(from.pos, to.pos);
    Face        dir = nearestFace(d.x, d.y, d.z);

    bool alignedAxes = true;
    for (Axis a : kAxes) {
        if (a != kinetics::axisOf(dir) && choose(a, d) != 0) alignedAxes = false;
    }
    bool byAxis  = alignedAxes && sf.shaftTowards(from.state, dir) && st.shaftTowards(to.state, opposite(dir));
    bool byGears = sf.cogOf(from.state) == CogType::Small && st.cogOf(to.state) == CogType::Small;

    if (sf.propagate) {
        float custom = sf.propagate(view(from), view(to), d, byAxis, byGears);
        if (custom != 0) return custom;
    }

    // Axis <-> Axis
    if (byAxis) {
        float m = axisModifier(to, opposite(dir));
        if (m != 0) m = 1 / m;
        return axisModifier(from, dir) * m;
    }

    // Large Gear <-> Large Gear
    if (largeToLarge(sf, from.state, st, to.state, d)) {
        Axis sourceAxis     = *sf.axisOf(from.state);
        Axis targetAxis     = *st.axisOf(to.state);
        int  sourceAxisDiff = choose(sourceAxis, d);
        int  targetAxisDiff = choose(targetAxis, d);
        return ((sourceAxisDiff > 0) ^ (targetAxisDiff > 0)) ? -1.f : 1.f;
    }

    // Gear <-> Large Gear
    if (sf.cogOf(from.state) == CogType::Large && st.cogOf(to.state) == CogType::Small) {
        if (largeToSmall(sf, from.state, st, to.state, d)) return -2.f;
    }
    if (st.cogOf(to.state) == CogType::Large && sf.cogOf(from.state) == CogType::Small) {
        if (largeToSmall(st, to.state, sf, from.state, d)) return -.5f;
    }

    // Gear <-> Gear
    if (byGears) {
        if (std::abs(d.x) + std::abs(d.y) + std::abs(d.z) != 1) return 0;
        auto fromAxis = sf.axisOf(from.state);
        auto toAxis   = st.axisOf(to.state);
        if (!fromAxis || !toAxis) return 0;
        if (kinetics::axisOf(dir) == *fromAxis) return 0;
        if (*fromAxis == *toAxis) return -1;
    }
    return 0;
}

bool Solver::isConnected(Node const& from, Node const& to) const {
    if (rotationSpeedModifier(from, to) != 0) return true;
    if (from.spec->customConnection && from.spec->customConnection(view(from), view(to))) return true;
    return to.spec->customConnection && to.spec->customConnection(view(to), view(from));
}

std::vector<GlobalPos> Solver::connected(Node const& n) const {
    std::vector<GlobalPos> out;
    for (auto const& loc : potentialNeighbourLocations(n)) {
        if (loc == n.pos) continue;
        auto const* nb = find(loc);
        if (!alive(nb)) continue;
        if (isConnected(n, *nb)) out.push_back(loc);
    }
    return out;
}

float Solver::conveyed(Node const& from, Node const& to) const {
    if (from.spec->conveyedSpeed) {
        if (auto v = from.spec->conveyedSpeed(view(from), view(to), true)) return *v;
    }
    if (to.spec->conveyedSpeed) {
        if (auto v = to.spec->conveyedSpeed(view(to), view(from), false)) return *v;
    }
    return from.speed * rotationSpeedModifier(from, to);
}

void Solver::propagateNewSource(GlobalPos const& start) {
    struct Frame {
        GlobalPos              pos;
        std::vector<GlobalPos> neighbours;
        std::size_t            index = 0;
    };
    std::vector<Frame> stack;
    auto               push = [&](GlobalPos const& p) {
        auto* n = find(p);
        if (!alive(n)) return;
        stack.push_back(Frame{p, connected(*n), 0});
    };
    push(start);
    long steps = 0;

    while (!stack.empty()) {
        auto* cur = find(stack.back().pos);
        if (!alive(cur) || stack.back().index >= stack.back().neighbours.size()) {
            stack.pop_back();
            continue;
        }
        GlobalPos npos = stack.back().neighbours[stack.back().index++];
        auto*     nb   = find(npos);
        if (!alive(nb)) continue;

        if (++steps > kMaxPropagationSteps) {
            destroy(*cur, DestroyReason::Flicker);
            stack.clear();
            break;
        }

        float speedOfCurrent   = cur->speed;
        float speedOfNeighbour = nb->speed;
        float newSpeed         = conveyed(*cur, *nb);
        float oppositeSpeed    = conveyed(*nb, *cur);
        if (newSpeed == 0 && oppositeSpeed == 0) continue;

        bool incompatible = signum(newSpeed) != signum(speedOfNeighbour) && newSpeed != 0 && speedOfNeighbour != 0;
        bool tooFast = std::abs(newSpeed) > mConfig.maxRotationSpeed || std::abs(oppositeSpeed) > mConfig.maxRotationSpeed;
        bool speedChangedTooOften = cur->flicker > kMaxFlickerScore;
        if (tooFast || speedChangedTooOften) {
            destroy(*cur, tooFast ? DestroyReason::TooFast : DestroyReason::Flicker);
            stack.pop_back();
            continue;
        }
        // Opposite directions
        if (incompatible) {
            destroy(*cur, DestroyReason::Conflict);
            stack.pop_back();
            continue;
        }
        // Same direction: overpower the slower speed.
        // Neighbour faster: overpower the incoming tree (re-run this block from the start).
        if (std::abs(oppositeSpeed) > std::abs(speedOfCurrent)) {
            float prevSpeed = effective(*cur);
            touch(*cur);
            setSource(*cur, *nb);
            cur->speed = conveyed(*nb, *cur);
            onSpeedChanged(*cur, prevSpeed);
            stack.back() = Frame{cur->pos, connected(*cur), 0};
            continue;
        }
        // Current faster: overpower the neighbour's tree.
        if (std::abs(newSpeed) >= std::abs(speedOfNeighbour)) {
            // Do not overpower your own network -> cycle
            if (!cur->network || cur->network == nb->network) {
                float epsilon = std::abs(speedOfNeighbour) / 256.f / 256.f;
                if (std::abs(newSpeed) > std::abs(speedOfNeighbour) + epsilon) {
                    destroy(*cur, DestroyReason::RatioLoop);
                    stack.pop_back();
                }
                continue;
            }
            if (cur->source && *cur->source == nb->pos) removeSource(*cur);
            float prevSpeed = effective(*nb);
            touch(*nb);
            setSource(*nb, *cur);
            nb->speed = conveyed(*cur, *nb);
            onSpeedChanged(*nb, prevSpeed);
            push(nb->pos);
            continue;
        }
    }
}

void Solver::propagateMissingSource(GlobalPos const& start) {
    std::vector<GlobalPos> potentialNewSources;
    std::deque<GlobalPos>  frontier{start};
    auto const*            first         = find(start);
    auto                   missingSource = first ? first->source : std::nullopt;

    while (!frontier.empty()) {
        GlobalPos pos = frontier.front();
        frontier.pop_front();
        auto* cur = find(pos);
        if (!alive(cur)) continue;
        removeSource(*cur);

        for (auto const& npos : connected(*cur)) {
            auto const* nb = find(npos);
            if (missingSource && npos == *missingSource) continue;
            if (!nb->source) {
                // Not in Create (it skips every block without a source): a running generator next
                // to the cleared area powers it again instead of leaving it stopped.
                if (nb->spec->generator && nb->speed != 0) potentialNewSources.push_back(npos);
                continue;
            }
            if (*nb->source != pos) {
                potentialNewSources.push_back(npos);
                continue;
            }
            if (nb->spec->generator) potentialNewSources.push_back(npos);
            frontier.push_back(npos);
        }
    }

    for (auto const& p : potentialNewSources) {
        auto const* n = find(p);
        if (!alive(n)) continue;
        // Create: `hasSource() || isSource()`. A generator only counts while it turns (a stopped or
        // just-cleared one would propagate nothing and hide the candidates after it).
        if (n->source || (n->spec->generator && n->speed != 0)) {
            propagateNewSource(p);
            return;
        }
    }
}

void Solver::handleRemoved(GlobalPos const& pos, std::vector<GlobalPos> const& locations, float speed) {
    if (speed == 0) return;
    for (auto const& loc : locations) {
        auto const* nb = find(loc);
        if (!alive(nb)) continue;
        if (!nb->source || *nb->source != pos) continue;
        propagateMissingSource(loc);
    }
}

// ---- Create's KineticBlockEntity / GeneratingKineticBlockEntity ------------------------------------------

void Solver::setNetwork(Node& n, std::optional<NetworkId> id) {
    if (n.network == id) return;
    if (n.network) {
        markDirty(n.network);
        auto it = mNetworks.find(*n.network);
        if (it != mNetworks.end()) {
            it->second.members.erase(n.pos);
            if (it->second.members.empty()) {
                if (it->second.overstressed) mNetworkEvents.push_back({*n.network, false, 0, 0});
                mNetworks.erase(it);
            }
        }
    }
    n.network = id;
    if (!id) {
        n.overstressed = false;
        return;
    }
    mNetworks[*id].members.insert(n.pos);
    markDirty(id);
}

void Solver::setSource(Node& n, Node const& source) {
    n.source = source.pos;
    setNetwork(n, source.network);
    if (n.spec->generator && n.reactivate && std::abs(effective(source)) >= std::abs(n.generated)) {
        n.reactivate = false;
        mReactivate.erase(n.pos);
    }
}

void Solver::removeSource(Node& n) {
    touch(n);
    if (n.source && n.spec->generator) {
        n.reactivate = true;
        mReactivate.insert(n.pos);
    }
    float prevSpeed = effective(n);
    markDirty(n.network);
    n.speed  = 0;
    n.source = std::nullopt;
    setNetwork(n, std::nullopt);
    onSpeedChanged(n, prevSpeed);
}

void Solver::onSpeedChanged(Node& n, float prevSpeed) {
    float cur          = effective(n);
    bool  fromOrToZero = (prevSpeed == 0) != (cur == 0);
    bool  directionSwap = !fromOrToZero && signum(prevSpeed) != signum(cur);
    if (fromOrToZero || directionSwap) {
        n.flicker += 5;
        mFlickering.insert(n.pos);
    }
    markDirty(n.network);
}

void Solver::detach(Node& n) { handleRemoved(n.pos, potentialNeighbourLocations(n), n.speed); }

void Solver::updateGeneratedRotation(Node& n) {
    float speed     = n.generated;
    float prevSpeed = n.speed;
    float prevEff   = effective(n);
    touch(n);
    if (prevSpeed != speed) {
        // applyNewSpeed
        if (speed == 0) {
            if (n.source) {
                markDirty(n.network); // only the capacity changes
            } else {
                detach(n);
                n.speed = 0;
                setNetwork(n, std::nullopt);
            }
        } else if (prevSpeed == 0) {
            // Now turning: create a new network
            n.speed = speed;
            setNetwork(n, n.pos);
            propagateNewSource(n.pos);
        } else if (n.source) {
            // Driven by another generator
            if (std::abs(prevSpeed) >= std::abs(speed)) {
                // staying below the overpowering speed
                if (signum(prevSpeed) != signum(speed)) destroy(n, DestroyReason::Generator);
            } else {
                // faster than the attached network: become the new root
                detach(n);
                n.speed  = speed;
                n.source = std::nullopt;
                setNetwork(n, n.pos);
                propagateNewSource(n.pos);
            }
        } else {
            // Reapply source
            detach(n);
            n.speed = speed;
            propagateNewSource(n.pos);
        }
    }
    if (alive(&n)) {
        markDirty(n.network);
        onSpeedChanged(n, prevEff);
    }
}

void Solver::validate(Node& n) {
    // Create's KineticBlockEntity.validateKinetics
    if (n.source) {
        if (!n.network) {
            removeSource(n);
            return;
        }
        auto const* src = find(*n.source);
        if (!src || src->speed == 0) {
            removeSource(n);
            detach(n);
        }
        return;
    }
    if (n.speed != 0 && n.generated == 0) {
        touch(n);
        n.speed = 0;
        markDirty(n.network);
    }
}

void Solver::destroy(Node& n, DestroyReason reason) {
    if (n.doomed) return;
    n.doomed = true;
    mDestroyed.push_back(Destroyed{n.pos, n.state, reason});
    mPendingRemoval.push_back(n.pos);
}

// ---- operations ----------------------------------------------------------------------------------------------

void Solver::removeNow(GlobalPos const& pos) {
    auto* n = find(pos);
    if (!n) return;
    touch(*n);
    auto  locations = potentialNeighbourLocations(*n);
    float speed     = n->speed;
    setNetwork(*n, std::nullopt);
    mFlickering.erase(pos);
    mReactivate.erase(pos);
    mNodes.erase(pos);
    handleRemoved(pos, locations, speed);
}

void Solver::recomputeNetwork(NetworkId const& id) {
    auto it = mNetworks.find(id);
    if (it == mNetworks.end()) return;
    auto& net = it->second;
    if (net.members.empty()) {
        mNetworks.erase(it);
        return;
    }
    double stress = 0, capacity = 0;
    for (auto const& p : net.members) {
        auto const* n = find(p);
        if (!n) continue;
        stress += impactOf(*n) * std::abs(n->speed);
        if (n->spec->generator) capacity += capacityOf(*n) * std::abs(n->generated);
    }
    bool over = !mConfig.disableStress && capacity < stress;
    if (over != net.overstressed) mNetworkEvents.push_back({id, over, stress, capacity});
    net.stress       = stress;
    net.capacity     = capacity;
    net.overstressed = over;
    for (auto const& p : net.members) {
        auto* n = find(p);
        if (!n || n->overstressed == over) continue;
        float prev = effective(*n);
        touch(*n);
        n->overstressed = over;
        onSpeedChanged(*n, prev);
    }
}

void Solver::settle() {
    if (mDepth > 0) return;
    ++mDepth;
    for (int round = 0; round < 8; ++round) {
        while (!mPendingRemoval.empty()) {
            GlobalPos p = mPendingRemoval.back();
            mPendingRemoval.pop_back();
            removeNow(p);
        }
        if (mDirtyNetworks.empty()) break;
        auto dirty = std::move(mDirtyNetworks);
        mDirtyNetworks.clear();
        for (auto const& id : dirty) recomputeNetwork(id);
        // recomputing only flips overstress flags, which never changes the sums: one more round
        // at most to clear the marks it left.
    }
    mDirtyNetworks.clear();
    --mDepth;
}

bool Solver::add(GlobalPos const& pos, BlockState state) {
    auto const* spec = mSpecs ? mSpecs(state.id) : nullptr;
    if (!spec) return false;
    if (find(pos)) removeNow(pos);
    mTouched.try_emplace(pos, Snapshot{});
    auto node   = std::make_unique<Node>();
    node->pos   = pos;
    node->state = std::move(state);
    node->spec  = spec;
    auto& n     = *node;
    mNodes[pos] = std::move(node);
    mValidationOrder.push_back(pos);

    float initial = (spec->generator && spec->initialGeneratedSpeed) ? spec->initialGeneratedSpeed(n.state) : 0.f;
    if (initial != 0) {
        n.generated = initial;
        updateGeneratedRotation(n);
    } else {
        propagateNewSource(pos); // Create's handleAdded on the first tick
    }
    settle();
    return true;
}

void Solver::remove(GlobalPos const& pos) {
    removeNow(pos);
    settle();
}

void Solver::update(GlobalPos const& pos, BlockState state) {
    auto* n = find(pos);
    if (!n) {
        add(pos, std::move(state));
        return;
    }
    if (n->state == state) return;
    auto const* spec = mSpecs ? mSpecs(state.id) : nullptr;
    if (!spec) {
        remove(pos);
        return;
    }
    if (spec != n->spec) {
        removeNow(pos);
        add(pos, std::move(state));
        return;
    }
    // Create's KineticBlockEntity.switchToBlockState: leave the network, detach, drop the source,
    // then attach again with the new state.
    touch(*n);
    detach(*n);
    if (alive(n)) {
        removeSource(*n);
        n->state = std::move(state);
        if (n->spec->generator && n->generated != 0) updateGeneratedRotation(*n);
        else propagateNewSource(pos);
    }
    settle();
}

void Solver::setGeneratedSpeed(GlobalPos const& pos, float rpm) {
    auto* n = find(pos);
    if (!alive(n) || !n->spec->generator) return;
    if (n->generated == rpm) return;
    n->generated = rpm;
    updateGeneratedRotation(*n);
    settle();
}

void Solver::setImpactOverride(GlobalPos const& pos, std::optional<double> impact) {
    auto* n = find(pos);
    if (!n) return;
    n->impactOverride = impact;
    markDirty(n->network);
    settle();
}

void Solver::setCapacityOverride(GlobalPos const& pos, std::optional<double> capacity) {
    auto* n = find(pos);
    if (!n) return;
    n->capacityOverride = capacity;
    markDirty(n->network);
    settle();
}

void Solver::refreshAllNetworks() {
    for (auto const& [id, net] : mNetworks) mDirtyNetworks.insert(id);
    settle();
}

void Solver::tick() {
    // flicker decay
    for (auto it = mFlickering.begin(); it != mFlickering.end();) {
        auto* n = find(*it);
        if (!n || --n->flicker <= 0) {
            if (n) n->flicker = 0;
            it = mFlickering.erase(it);
        } else {
            ++it;
        }
    }
    // generators driven by a source that went away start again (Create's reActivateSource)
    if (!mReactivate.empty()) {
        auto pending = std::vector<GlobalPos>(mReactivate.begin(), mReactivate.end());
        std::sort(pending.begin(), pending.end());
        mReactivate.clear();
        for (auto const& p : pending) {
            auto* n = find(p);
            if (!alive(n) || !n->reactivate) continue;
            n->reactivate = false;
            updateGeneratedRotation(*n);
            settle();
        }
    }
    // validation: every block once per `validationFrequency` ticks
    int freq = std::max(1, mConfig.validationFrequency);
    if (mValidationCursor >= mValidationOrder.size()) {
        mValidationOrder.clear();
        mValidationOrder.reserve(mNodes.size());
        for (auto const& [p, n] : mNodes) mValidationOrder.push_back(p);
        std::sort(mValidationOrder.begin(), mValidationOrder.end());
        mValidationCursor = 0;
    }
    std::size_t count = (mValidationOrder.size() + freq - 1) / freq;
    for (std::size_t i = 0; i < count && mValidationCursor < mValidationOrder.size(); ++i) {
        auto* n = find(mValidationOrder[mValidationCursor++]);
        if (alive(n)) validate(*n);
    }
    settle();
}

void Solver::clear() {
    mNodes.clear();
    mNetworks.clear();
    mDirtyNetworks.clear();
    mTouched.clear();
    mPendingRemoval.clear();
    mDestroyed.clear();
    mNetworkEvents.clear();
    mFlickering.clear();
    mReactivate.clear();
    mValidationOrder.clear();
    mValidationCursor = 0;
}

void Solver::restore(NodeRecord const& r) {
    auto const* spec = mSpecs ? mSpecs(r.state.id) : nullptr;
    if (!spec) return;
    if (auto* old = find(r.pos)) setNetwork(*old, std::nullopt);
    auto n              = std::make_unique<Node>();
    n->pos              = r.pos;
    n->state            = r.state;
    n->spec             = spec;
    n->speed            = r.speed;
    n->generated        = spec->generator ? r.generated : 0.f;
    n->source           = r.source;
    n->impactOverride   = r.impactOverride;
    n->capacityOverride = r.capacityOverride;
    auto& node          = *n;
    mNodes[r.pos]       = std::move(n);
    setNetwork(node, r.network);
}

void Solver::finishRestore() {
    // Create re-attaches every kinetic block entity when it loads (`updateSpeed`), which re-checks
    // the network against its neighbours; validate first so stale sources are dropped.
    std::vector<GlobalPos> all;
    all.reserve(mNodes.size());
    for (auto const& [p, n] : mNodes) all.push_back(p);
    std::sort(all.begin(), all.end());
    for (auto const& p : all) {
        if (auto* n = find(p); alive(n)) validate(*n);
    }
    settle();
    for (auto const& p : all) {
        if (find(p)) propagateNewSource(p);
        settle();
    }
    mTouched.clear(); // a reload is not a change
}

NodeRecord Solver::record(GlobalPos const& pos) const {
    NodeRecord r;
    auto const* n = find(pos);
    if (!n) return r;
    r.pos              = n->pos;
    r.state            = n->state;
    r.speed            = n->speed;
    r.generated        = n->generated;
    r.source           = n->source;
    r.network          = n->network;
    r.impactOverride   = n->impactOverride;
    r.capacityOverride = n->capacityOverride;
    return r;
}

// ---- queries ----------------------------------------------------------------------------------------------------

bool Solver::contains(GlobalPos const& pos) const { return alive(find(pos)); }

std::optional<NodeInfo> Solver::node(GlobalPos const& pos) const {
    auto const* n = find(pos);
    if (!alive(n)) return std::nullopt;
    return info(*n);
}

std::optional<NetworkInfo> Solver::network(NetworkId const& id) const {
    auto it = mNetworks.find(id);
    if (it == mNetworks.end()) return std::nullopt;
    NetworkInfo i;
    i.id           = id;
    i.stress       = it->second.stress;
    i.capacity     = it->second.capacity;
    i.overstressed = it->second.overstressed;
    i.members      = static_cast<int>(it->second.members.size());
    for (auto const& p : it->second.members) {
        auto const* n = find(p);
        if (n && n->spec->generator) ++i.sources;
    }
    return i;
}

std::optional<NetworkInfo> Solver::networkAt(GlobalPos const& pos) const {
    auto const* n = find(pos);
    if (!alive(n) || !n->network) return std::nullopt;
    return network(*n->network);
}

std::vector<GlobalPos> Solver::members(NetworkId const& id) const {
    std::vector<GlobalPos> out;
    auto                   it = mNetworks.find(id);
    if (it == mNetworks.end()) return out;
    out.assign(it->second.members.begin(), it->second.members.end());
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t Solver::size() const { return mNodes.size(); }

void Solver::forEachNode(std::function<void(NodeInfo const&)> const& fn) const {
    for (auto const& [p, n] : mNodes) {
        if (!n->doomed) fn(info(*n));
    }
}

std::vector<GlobalPos> Solver::connectedNeighbours(GlobalPos const& pos) const {
    auto const* n = find(pos);
    if (!alive(n)) return {};
    return connected(*n);
}

float Solver::conveyedSpeed(GlobalPos const& from, GlobalPos const& to) const {
    auto const* a = find(from);
    auto const* b = find(to);
    if (!alive(a) || !alive(b) || !isConnected(*a, *b)) return 0;
    return conveyed(*a, *b);
}

// ---- effects ---------------------------------------------------------------------------------------------------

std::vector<Destroyed> Solver::takeDestroyed() { return std::exchange(mDestroyed, {}); }

std::vector<NetworkEvent> Solver::takeNetworkEvents() { return std::exchange(mNetworkEvents, {}); }

std::vector<Change> Solver::takeChanges() {
    std::vector<Change> out;
    for (auto const& [pos, snap] : mTouched) {
        auto const* n = find(pos);
        if (!n || n->doomed) {
            if (snap.existed) {
                Change c;
                c.pos             = pos;
                c.id              = snap.id;
                c.prevSpeed       = snap.speed;
                c.prevTheoretical = snap.theoretical;
                c.prevSource      = snap.source;
                c.removed         = true;
                out.push_back(c);
            }
            continue;
        }
        Change c;
        c.pos             = pos;
        c.id              = n->state.id;
        c.prevSpeed       = snap.speed;
        c.speed           = effective(*n);
        c.prevTheoretical = snap.theoretical;
        c.theoretical     = n->speed;
        c.prevSource      = snap.source;
        c.source          = n->source;
        c.added           = !snap.existed;
        if (c.added || c.prevSpeed != c.speed || c.prevTheoretical != c.theoretical || c.prevSource != c.source) {
            out.push_back(c);
        }
    }
    mTouched.clear();
    std::sort(out.begin(), out.end(), [](Change const& a, Change const& b) { return a.pos < b.pos; });
    return out;
}

// ---- levels and rendering maths -----------------------------------------------------------------------------------

SpeedLevel speedLevel(float speed, float mediumSpeed, float fastSpeed) {
    speed = std::abs(speed);
    if (speed >= fastSpeed) return SpeedLevel::Fast;
    if (speed >= mediumSpeed) return SpeedLevel::Medium;
    if (speed >= 1) return SpeedLevel::Slow;
    return SpeedLevel::None;
}

StressLevel stressLevel(double stress, double capacity) {
    double usage = capacity > 0 ? stress / capacity : (stress > 0 ? 2.0 : 0.0);
    if (usage > 1) return StressLevel::Overstressed;
    if (usage > .75) return StressLevel::High;
    if (usage > .5) return StressLevel::Medium;
    return StressLevel::Low;
}

float rotationOffset(GlobalPos const& pos, Axis axis, bool largeCog, float extra) {
    float     offset = largeCog ? 11.25f : 0.f;
    long long d      = static_cast<long long>(axis == Axis::X ? 0 : pos.x) + (axis == Axis::Y ? 0 : pos.y)
                + (axis == Axis::Z ? 0 : pos.z);
    if (d % 2 == 0) offset = 22.5f;
    return offset + extra;
}

float rotationAngle(double ticks, float speed, float offset) {
    return static_cast<float>(std::fmod(ticks * speed * 3.0 / 10.0 + offset, 360.0));
}

} // namespace lv::core::kinetics
