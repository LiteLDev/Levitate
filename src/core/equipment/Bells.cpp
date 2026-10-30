#include "core/equipment/Bells.h"

#include <cmath>
#include <numbers>

namespace lv::core::equipment {

int oppositeFace(int face) { return face ^ 1; }

bool horizontalFace(int face) { return face >= 2 && face <= 5; }

int clockwise(int face) {
    switch (face) {
    case 2: return 5; // north -> east
    case 5: return 3; // east -> south
    case 3: return 4; // south -> west
    case 4: return 2; // west -> north
    default: return face;
    }
}

std::array<int, 3> faceOffset(int face) {
    switch (face) {
    case 0: return {0, -1, 0};
    case 1: return {0, 1, 0};
    case 2: return {0, 0, -1};
    case 3: return {0, 0, 1};
    case 4: return {-1, 0, 0};
    case 5: return {1, 0, 0};
    default: return {0, 0, 0};
    }
}

namespace {

bool sameAxis(int a, int b) { return (a >> 1) == (b >> 1); }

bool sturdy(BellSupport const& s, int dir) { return s.sturdy && s.sturdy(dir); }

} // namespace

bool bellSurvives(BellState const& s, BellSupport const& support) {
    // the side the bell hangs on: below (floor), above (ceiling), else towards FACING (the wall)
    switch (s.attach) {
    case BellAttach::Floor: return sturdy(support, 0);
    case BellAttach::Ceiling: return support.centreAbove && support.centreAbove();
    default: return sturdy(support, s.facing);
    }
}

std::optional<BellState> placeBell(int clickedFace, int playerFacing, BellSupport const& support) {
    if (!horizontalFace(clickedFace)) {
        BellState s{clickedFace == 0 ? BellAttach::Ceiling : BellAttach::Floor, horizontalFace(playerFacing) ? playerFacing : 3};
        if (bellSurvives(s, support)) return s;
        return std::nullopt;
    }
    // against a wall: facing into it; between two walls along that axis: a double wall bell
    int  facing  = oppositeFace(clickedFace);
    bool between = sturdy(support, facing) && sturdy(support, oppositeFace(facing));
    BellState s{between ? BellAttach::DoubleWall : BellAttach::SingleWall, facing};
    if (bellSurvives(s, support)) return s;
    s.attach = sturdy(support, 0) ? BellAttach::Floor : BellAttach::Ceiling;
    if (bellSurvives(s, support)) return s;
    return std::nullopt;
}

std::optional<BellState> bellAfterNeighbourChange(BellState const& s, int changed, BellSupport const& support) {
    int hangsOn = s.attach == BellAttach::Floor ? 0 : s.attach == BellAttach::Ceiling ? 1 : s.facing;
    if (changed == hangsOn && !bellSurvives(s, support) && s.attach != BellAttach::DoubleWall) return std::nullopt;
    if (horizontalFace(changed) && sameAxis(changed, s.facing)) {
        if (s.attach == BellAttach::DoubleWall && !sturdy(support, changed)) return BellState{BellAttach::SingleWall, oppositeFace(changed)};
        if (s.attach == BellAttach::SingleWall && changed == oppositeFace(s.facing) && sturdy(support, changed))
            return BellState{BellAttach::DoubleWall, s.facing};
    }
    return s;
}

bool bellRingsFrom(BellState const& s, int hitFace, double height) {
    if (!horizontalFace(hitFace)) return false;
    if (height > 0.8124) return false;
    switch (s.attach) {
    case BellAttach::Floor:
    case BellAttach::Ceiling: return sameAxis(s.facing, hitFace);
    default: return !sameAxis(s.facing, hitFace);
    }
}

int bellRedstoneDirection(BellState const& s) {
    return s.attach == BellAttach::Floor || s.attach == BellAttach::Ceiling ? s.facing : clockwise(s.facing);
}

float bellSwingAngle(float ticks) {
    float t = ticks / 1.5f;
    return 1.2f * std::sin(t / std::numbers::pi_v<float>) / (2.5f + t / 3.0f);
}

// ---- soul pulses ---------------------------------------------------------------------------------

std::vector<Pos> const& soulLayer(int idx) {
    static std::vector<std::vector<Pos>> const layers = [] {
        std::vector<std::vector<Pos>> out(kSoulMaxDistance);
        int const r = kSoulMaxDistance - 1;
        for (int x = -r; x <= r; ++x)
            for (int y = -r; y <= r; ++y)
                for (int z = -r; z <= r; ++z) {
                    int dist = static_cast<int>(std::lround(std::sqrt(double(x * x + y * y + z * z))));
                    if (dist > kSoulMaxDistance) continue;
                    if (dist <= 0) dist = 1;
                    out[static_cast<std::size_t>(dist - 1)].push_back({x, y, z});
                }
        return out;
    }();
    static std::vector<Pos> const none;
    if (idx < 0 || idx >= kSoulMaxDistance) return none;
    return layers[static_cast<std::size_t>(idx)];
}

SoulPulse::SoulPulse(Pos origin, int distance, bool canOverlap)
: mOrigin(origin),
  mDistance(distance),
  mCanOverlap(canOverlap),
  mTicks(kSoulTicksPerLayer * distance) {}

std::optional<std::vector<Pos>> SoulPulse::tick(std::function<bool(Pos const&)> const& canSpawn) {
    if (finished()) return std::nullopt;
    --mTicks;
    if (mTicks < 0 || mTicks % kSoulTicksPerLayer != 0) return std::nullopt;
    auto layer = [&] {
        std::vector<Pos> out;
        for (auto const& o : soulLayer(currentLayer())) {
            Pos p{mOrigin[0] + o[0], mOrigin[1] + o[1], mOrigin[2] + o[2]};
            if (canSpawn(p)) out.push_back(p);
        }
        return out;
    };
    auto spawns = layer();
    while (spawns.empty() && mTicks > 0) {
        mTicks -= kSoulTicksPerLayer;
        auto more = layer();
        spawns.insert(spawns.end(), more.begin(), more.end());
    }
    return spawns;
}

void SoulPulses::add(SoulPulse pulse) { mPulses.push_back(std::move(pulse)); }

void SoulPulses::tick(std::function<bool(Pos const&)> const& canSpawn, std::function<void(Pos const&)> const& mark) {
    for (auto& pulse : mPulses) {
        auto spawns = pulse.tick(canSpawn);
        if (!spawns) continue;
        for (auto const& p : *spawns) {
            if (!pulse.canOverlap()) {
                if (mOccupied.contains(p)) continue;
                pulse.added.push_back(p);
                mOccupied.insert(p);
            }
            mark(p);
        }
    }
    for (auto const& pulse : mPulses)
        if (pulse.finished() && !pulse.canOverlap())
            for (auto const& p : pulse.added) mOccupied.erase(p);
    std::erase_if(mPulses, [](SoulPulse const& p) { return p.finished(); });
}

bool SoulPulses::expanding(Pos const& origin, int distance) const {
    for (auto const& p : mPulses)
        if (!p.canOverlap() && p.expanding() && p.origin() == origin && p.distance() == distance) return true;
    return false;
}

void SoulPulses::clear() {
    mPulses.clear();
    mOccupied.clear();
}

bool heldBellPulses(int heldTicks, long long gameTime) {
    if (heldTicks < kHeldPulseWarmup) return false;
    if (heldTicks == kHeldPulseWarmup) return true;
    return gameTime % kHeldPulseRecharge != 0;
}

} // namespace lv::core::equipment
