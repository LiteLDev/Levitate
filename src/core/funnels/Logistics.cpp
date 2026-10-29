#include "core/funnels/Logistics.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace lv::core::funnels {

// ---- extraction counts ----------------------------------------------------------------------------

std::string amountLabel(AmountSetting s, int maxStack, std::string_view anyText) {
    if (s.upTo && s.count >= maxStack) return std::string(anyText);
    return std::string(s.upTo ? "≤" : "=") + std::to_string(std::max(1, s.count));
}

// ---- brass tunnel distribution ---------------------------------------------------------------------

std::string_view selectionModeKey(SelectionMode mode) {
    switch (mode) {
    case SelectionMode::Split: return "create.tunnel.selection_mode.split";
    case SelectionMode::ForcedSplit: return "create.tunnel.selection_mode.forced_split";
    case SelectionMode::RoundRobin: return "create.tunnel.selection_mode.round_robin";
    case SelectionMode::ForcedRoundRobin: return "create.tunnel.selection_mode.forced_round_robin";
    case SelectionMode::PreferNearest: return "create.tunnel.selection_mode.prefer_nearest";
    case SelectionMode::Randomize: return "create.tunnel.selection_mode.randomize";
    case SelectionMode::Synchronize: return "create.tunnel.selection_mode.synchronize";
    }
    return "";
}

std::string_view selectionModeEnglish(SelectionMode mode) {
    switch (mode) {
    case SelectionMode::Split: return "Split";
    case SelectionMode::ForcedSplit: return "Forced Split";
    case SelectionMode::RoundRobin: return "Round Robin";
    case SelectionMode::ForcedRoundRobin: return "Forced Round Robin";
    case SelectionMode::PreferNearest: return "Prefer Nearest";
    case SelectionMode::Randomize: return "Randomize";
    case SelectionMode::Synchronize: return "Synchronize Inputs";
    }
    return "";
}

int distributionStart(SelectionMode mode, int previousIndex, int targets, unsigned random) {
    if (targets <= 0) return 0;
    switch (mode) {
    case SelectionMode::Randomize: return static_cast<int>(random % static_cast<unsigned>(targets));
    case SelectionMode::PreferNearest:
    case SelectionMode::Synchronize: return 0;
    default: return ((previousIndex % targets) + targets) % targets;
    }
}

DistributionPlan planDistribution(SelectionMode mode, int stackCount, int targets, int start, OutputProbe const& probe) {
    DistributionPlan plan;
    plan.kept = stackCount;
    if (targets <= 0 || stackCount <= 0) return plan;
    bool force = mode == SelectionMode::ForcedSplit || mode == SelectionMode::ForcedRoundRobin;
    bool split = mode == SelectionMode::Split || mode == SelectionMode::ForcedSplit;
    bool robin = mode == SelectionMode::RoundRobin || mode == SelectionMode::ForcedRoundRobin;

    std::vector<int> order;
    for (int i = 0; i < targets; ++i) order.push_back((start + i) % targets);
    std::vector<int> given(static_cast<std::size_t>(targets), 0);
    // items `t` takes of `extra` more on top of what it was already given
    auto accepts = [&](int t, int extra) -> std::optional<int> {
        int  base = given[static_cast<std::size_t>(t)];
        auto rest = probe(t, base + extra);
        if (!rest) return std::nullopt;
        return std::clamp(base + extra - *rest - base, 0, extra);
    };
    auto finish = [&] {
        int total = 0;
        for (int t : order) {
            if (int g = given[static_cast<std::size_t>(t)]; g > 0) {
                plan.give.emplace_back(t, g);
                total += g;
            }
        }
        plan.kept = stackCount - total;
        return plan;
    };

    if (!split) {
        for (int t : order) {
            auto a = accepts(t, stackCount);
            if (!a || *a == 0) {
                if (force || robin) return plan; // waits / skips this output next time
                continue;
            }
            if (force && *a < stackCount) return plan;
            given[static_cast<std::size_t>(t)] = *a;
            break;
        }
        return finish();
    }

    int              remaining = stackCount;
    std::vector<int> active    = order;
    for (int round = 0; round < 2 && remaining > 0 && !active.empty(); ++round) {
        // outputs that take nothing of their share are skipped (forced split waits instead)
        for (bool dropped = true; dropped && !active.empty();) {
            dropped    = false;
            int n      = static_cast<int>(active.size());
            int base   = remaining / n, extra = remaining % n;
            for (int i = 0; i < n; ++i) {
                int c = base + (i < extra ? 1 : 0);
                if (c == 0) continue;
                auto a = accepts(active[static_cast<std::size_t>(i)], c);
                if (!a || *a == 0) {
                    if (force) return plan;
                    active.erase(active.begin() + i);
                    dropped = true;
                    break;
                }
            }
        }
        if (active.empty()) break;
        int              n    = static_cast<int>(active.size());
        int              base = remaining / n, extra = remaining % n;
        int              leftovers = 0;
        std::vector<int> stillOpen;
        for (int i = 0; i < n; ++i) {
            int t = active[static_cast<std::size_t>(i)];
            int c = base + (i < extra ? 1 : 0);
            if (c == 0) {
                stillOpen.push_back(t);
                continue;
            }
            int a = accepts(t, c).value_or(0);
            given[static_cast<std::size_t>(t)] += a;
            leftovers += c - a;
            if (a == c) stillOpen.push_back(t);
        }
        remaining = leftovers;
        active    = std::move(stillOpen);
    }
    return finish();
}

// ---- tunnel shapes ----------------------------------------------------------------------------------

TunnelShape tunnelShape(bool outputLeft, bool outputRight, bool canHaveWindow) {
    if (outputLeft && outputRight) return TunnelShape::Cross;
    if (outputLeft) return TunnelShape::TLeft;
    if (outputRight) return TunnelShape::TRight;
    return canHaveWindow ? TunnelShape::Window : TunnelShape::Straight;
}

std::vector<int> tunnelSides(int axis, TunnelShape shape) {
    std::vector<int> sides;
    for (int dir : kHorizontals) {
        if (axisOf(dir) != axis) {
            bool positive = isPositive(dir) ^ (axisOf(dir) == 2);
            if (isStraight(shape)) continue;
            if (positive && shape == TunnelShape::TLeft) continue;
            if (!positive && shape == TunnelShape::TRight) continue;
        }
        sides.push_back(dir);
    }
    return sides;
}

// ---- chutes ----------------------------------------------------------------------------------------

float chuteItemMotion(float push, float pull) {
    constexpr float fanSpeedModifier = 1.f / 64.f, maxItemSpeed = 20.f, gravity = 4.f;
    float motion = (push + pull) * fanSpeedModifier;
    return (std::clamp(motion, -maxItemSpeed, maxItemSpeed) + (motion <= 0 ? -gravity : 0.f)) / 20.f;
}

float chutePullDistance(float speed) {
    if (speed >= 128) return 3;
    if (speed >= 64) return 2;
    if (speed >= 32) return 1;
    return std::max(0.f, speed / 32.f);
}

std::pair<int, ChuteShape> updateChuteState(
    int                        facing,
    ChuteShape                 shape,
    std::array<bool, 6> const& diagonalInputs,
    bool                       targetBelowIsChute,
    bool                       aboveFacesDown
) {
    bool vertical = facing == kDown;
    if (!vertical && !targetBelowIsChute) return {kDown, ChuteShape::Normal};
    int amount = 0;
    for (int dir : kHorizontals)
        if (diagonalInputs[static_cast<std::size_t>(dir)]) ++amount;
    if (vertical) {
        if (amount == 0) return {facing, shape == ChuteShape::Intersection ? ChuteShape::Normal : shape};
        return {facing, ChuteShape::Intersection};
    }
    auto in = [&](int f) { return diagonalInputs[static_cast<std::size_t>(f)]; };
    if (amount == 0) return {facing, ChuteShape::Intersection};
    if (in(kNorth) && in(kSouth)) return {facing, ChuteShape::Intersection};
    if (in(kEast) && in(kWest)) return {facing, ChuteShape::Intersection};
    if (amount == 1 && in(facing) && !aboveFacesDown)
        return {facing, shape == ChuteShape::Encased ? ChuteShape::Encased : ChuteShape::Normal};
    return {facing, ChuteShape::Intersection};
}

// ---- item vaults -------------------------------------------------------------------------------------

namespace {
VaultCell offsetCell(VaultCell o, int axis, int l, int a, int b) {
    if (axis == 0) return {o.x + l, o.y + a, o.z + b};
    return {o.x + a, o.y + b, o.z + l};
}
} // namespace

VaultCell VaultStructure::cell(int index) const {
    int l = index / (width * width), rest = index % (width * width);
    return offsetCell(origin, axis, l, rest / width, rest % width);
}

int VaultStructure::indexOf(VaultCell const& c) const {
    int dx = c.x - origin.x, dy = c.y - origin.y, dz = c.z - origin.z;
    int l, a, b;
    if (axis == 0) {
        l = dx;
        a = dy;
        b = dz;
    } else {
        a = dx;
        b = dy;
        l = dz;
    }
    if (l < 0 || l >= length || a < 0 || a >= width || b < 0 || b >= width) return -1;
    return l * width * width + a * width + b;
}

std::vector<VaultStructure>
formVaults(std::vector<VaultCell> const& component, std::function<std::optional<int>(VaultCell const&)> const& axisAt) {
    std::set<VaultCell> open(component.begin(), component.end());
    std::vector<VaultStructure> out;
    auto lengthOf = [&](VaultCell const& origin, int axis, int width) {
        int length = 0;
        for (int l = 0; l < vaultMaxLength(width); ++l) {
            for (int a = 0; a < width; ++a)
                for (int b = 0; b < width; ++b) {
                    auto c = offsetCell(origin, axis, l, a, b);
                    if (!open.contains(c) || axisAt(c) != axis) return length;
                }
            ++length;
        }
        return length;
    };
    while (!open.empty()) {
        std::optional<VaultStructure> best;
        for (auto const& origin : open) {
            auto axis = axisAt(origin);
            if (!axis) continue;
            int bestAmount = -1;
            VaultStructure cand{origin, *axis, 1, 1};
            for (int w = 1; w <= kVaultMaxWidth; ++w) {
                int len = lengthOf(origin, *axis, w);
                int amount = len * w * w;
                if (len == 0 || amount < bestAmount) continue;
                bestAmount = amount;
                cand.width = w;
                cand.length = len;
            }
            if (bestAmount <= 0) continue;
            if (!best || cand.size() > best->size()) best = cand;
        }
        if (!best) break;
        for (int i = 0; i < best->size(); ++i) open.erase(best->cell(i));
        out.push_back(*best);
    }
    return out;
}

VaultLook vaultLook(VaultStructure const& s, VaultCell const& c) {
    int idx = s.indexOf(c);
    if (idx < 0) return {};
    int l = idx / (s.width * s.width), rest = idx % (s.width * s.width);
    int a = rest / s.width, b = rest % s.width;
    auto part = [](int i, int n) { return n == 1 ? 0 : (i == 0 ? 1 : (i == n - 1 ? 3 : 2)); };
    VaultLook look;
    look.len = part(l, s.length);
    // axis x: a = y offset, b = z offset; axis z: a = x offset, b = y offset
    int horizontal = s.axis == 0 ? b : a, vertical = s.axis == 0 ? a : b;
    look.u     = part(horizontal, s.width);
    look.v     = part(vertical, s.width);
    look.large = s.width > 2;
    return look;
}

int comparatorSignal(std::vector<std::pair<int, int>> const& slots, int totalSlots) {
    if (totalSlots <= 0) return 0;
    float f = 0;
    int   filled = 0;
    for (auto const& [count, max] : slots) {
        if (count <= 0) continue;
        f += static_cast<float>(count) / static_cast<float>(std::max(1, max));
        ++filled;
    }
    f /= static_cast<float>(totalSlots);
    return static_cast<int>(std::floor(f * 14.f)) + (filled > 0 ? 1 : 0);
}

} // namespace lv::core::funnels
