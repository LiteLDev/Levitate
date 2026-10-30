#include "core/redstone/RoseQuartzLamp.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace lv::core::redstone {

LampChange lampNeighbourChanged(LampState const& s, bool input) {
    if (s.powered == input) return LampChange::None;
    return s.powered ? LampChange::Unpower : LampChange::Activate;
}

LampState lampTick(LampState s) {
    if (s.powering || s.activate) {
        s.powering = s.activate;
        s.activate = false;
    }
    return s;
}

std::vector<LampPos> lampCluster(LampPos start, std::function<bool(LampPos const&)> const& isLamp) {
    static constexpr int dirs[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    std::vector<LampPos> out;
    std::deque<LampPos>  frontier{start};
    std::set<LampPos>    visited{start};
    while (!frontier.empty()) {
        LampPos pos = frontier.front();
        frontier.pop_front();
        for (auto const& d : dirs) {
            LampPos next{pos[0] + d[0], pos[1] + d[1], pos[2] + d[2]};
            int     dist = std::abs(next[0] - start[0]) + std::abs(next[1] - start[1]) + std::abs(next[2] - start[2]);
            if (dist > kLampClusterRange) continue;
            if (!visited.insert(next).second) continue;
            if (!isLamp(next)) continue;
            out.push_back(next);
            frontier.push_back(next);
        }
    }
    return out;
}

int lampDistanceToPowered(std::function<int(int i)> const& lampAt) {
    for (int power = 15, i = 0; power > 0; --power, ++i) {
        int s = lampAt(i);
        if (s < 0) return 0;
        if (s > 0) return power;
    }
    return 0;
}

BellPose deskBellPose(float p, float offset) {
    constexpr float kPi = 3.14159265358979f;
    BellPose        out;
    float           f  = static_cast<float>(1 - 4 * std::pow(std::max(p - 0.5, 0.0) - 0.5, 2));
    float           f2 = std::pow(std::max(p, 0.f), 1.25f);
    out.plunger        = f * .75f / 16.f;
    out.tiltX          = f2 * 8 * std::sin(p * kPi * 4 + offset);
    out.tiltZ          = f2 * 8 * std::cos(p * kPi * 4 + offset);
    return out;
}

} // namespace lv::core::redstone
