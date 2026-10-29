// Breaking piston pole lines and rope columns (Create's playerWillDestroy of the piston, head
// and pole blocks, and RopeBlockBase.onRemove / PulleyBlock.onRemove).

#include "feature/pistons/Pistons.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::pistons {

namespace {

bool poleAlong(GlobalPos const& q, cc::Axis axis) { return idAt(q) == kPole && cc::axisOf(facingAt(q)) == axis; }

/// Destroys the blocks strictly between `a` and `b` (same line), except `skip`.
void destroyBetween(GlobalPos const& a, GlobalPos const& b, cc::Dir towardsB, GlobalPos const& skip, bool drops, bool includeB) {
    for (int i = 1;; ++i) {
        auto q = rel(a, towardsB, i);
        if (q == b && !includeB) break;
        if (q != skip) destroyAt(q, drops);
        if (q == b) break;
        if (i > 4096) break;
    }
}

void resetPulley(GlobalPos const& pulley) {
    if (idAt(pulley) != kRopePulley) return;
    auto& d   = data(pulley);
    d["init"] = 0;
    if (!d.value("running", false)) d["offset"] = 0.0f; // Create's onLengthBroken
}

} // namespace

void destroyPistonChain(GlobalPos const& piston, cc::Dir facing, bool drops) {
    int  maxPoles = config::get<Config>().maxPistonPoles;
    auto axis     = cc::axisOf(facing);
    std::optional<GlobalPos> head;
    for (int o = 1; o < maxPoles; ++o) {
        auto cur = rel(piston, facing, o);
        if (poleAlong(cur, axis)) continue;
        if (idAt(cur) == kHead && facingAt(cur) == facing) head = cur;
        break;
    }
    if (head) destroyBetween(piston, *head, facing, piston, drops, true);
    for (int o = 1; o < maxPoles; ++o) {
        auto cur = rel(piston, cc::opposite(facing), o);
        if (!poleAlong(cur, axis)) break;
        destroyAt(cur, drops);
    }
}

void pistonChainBroken(GlobalPos const& p, std::string const& id, cc::Dir facing, bool drops) {
    int  maxPoles = config::get<Config>().maxPistonPoles;
    auto axis     = cc::axisOf(facing);
    if (id == kHead) {
        // MechanicalPistonHeadBlock.playerWillDestroy: back through the poles to the base
        for (int o = 1; o < maxPoles; ++o) {
            auto cur = rel(p, cc::opposite(facing), o);
            if (poleAlong(cur, axis)) continue;
            if (isPiston(idAt(cur)) && facingAt(cur) == facing) {
                destroyBetween(cur, p, facing, p, drops, false);
                {
                    EditGuard g;
                    setStates(cur, {{kState, int(PistonState::Retracted)}});
                }
                log().info("piston head at {} {} {} broken: piston retracted", p.x, p.y, p.z);
            }
            break;
        }
        return;
    }
    if (id != kPole) return;
    // PistonExtensionPoleBlock.playerWillDestroy: the base and the head of the line
    cc::Dir                  positive = cc::dirOf(axis, true);
    std::optional<GlobalPos> base, head;
    for (int modifier : {1, -1}) {
        cc::Dir d = modifier > 0 ? positive : cc::opposite(positive);
        for (int o = 1; o < maxPoles; ++o) {
            auto cur = rel(p, d, o);
            if (poleAlong(cur, axis)) continue;
            auto cid = idAt(cur);
            if (isPiston(cid) && cc::axisOf(facingAt(cur)) == axis) base = cur;
            if (cid == kHead && cc::axisOf(facingAt(cur)) == axis) head = cur;
            break;
        }
    }
    if (!base || !head || facingAt(*base) != facingAt(*head)) return;
    cc::Dir towardsHead = facingAt(*base);
    destroyBetween(*base, *head, towardsHead, p, drops, true);
    {
        EditGuard g;
        setStates(*base, {{kState, int(PistonState::Retracted)}});
    }
    auto& d     = data(*base);
    d["offset"] = 0.0f;
    log().info("piston pole at {} {} {} broken: piston at {} {} {} retracted", p.x, p.y, p.z, base->x, base->y, base->z);
}

void ropeBroken(GlobalPos const& p, bool pulleyRemoved) {
    EditGuard g;
    int       destroyed = 0;
    if (!pulleyRemoved) {
        // up the column to the pulley
        auto cur = rel(p, cc::Dir::Up);
        while (isRopeLike(idAt(cur))) {
            destroyAt(cur, true);
            ++destroyed;
            cur = rel(cur, cc::Dir::Up);
        }
        resetPulley(cur);
    }
    auto cur = rel(p, cc::Dir::Down);
    while (isRopeLike(idAt(cur))) {
        destroyAt(cur, true);
        ++destroyed;
        cur = rel(cur, cc::Dir::Down);
    }
    if (destroyed) log().info("rope at {} {} {} broke: {} rope block(s) removed", p.x, p.y, p.z, destroyed);
}

} // namespace lv::feature::pistons
