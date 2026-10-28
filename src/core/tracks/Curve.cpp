#include "core/tracks/Curve.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::tracks {

namespace {
constexpr float kCircle = 2 * std::numbers::pi_v<float>;

nlohmann::json vecJson(Vec3d const& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
nlohmann::json posJson(BlockPos3 const& p) { return nlohmann::json::array({p.x, p.y, p.z}); }
Vec3d          jsonVec(nlohmann::json const& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
BlockPos3      jsonPos(nlohmann::json const& j) { return {j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>()}; }
} // namespace

Curve::Curve(CurveSpec spec, bool primary) : mSpec(std::move(spec)), mPrimary(primary) { resolve(); }

Curve Curve::secondary() const {
    CurveSpec s = mSpec;
    std::swap(s.blocks[0], s.blocks[1]);
    std::swap(s.starts[0], s.starts[1]);
    std::swap(s.axes[0], s.axes[1]);
    std::swap(s.normals[0], s.normals[1]);
    return Curve(std::move(s), !mPrimary);
}

void Curve::resolve() {
    Vec3d end1  = mSpec.starts[0];
    Vec3d end2  = mSpec.starts[1];
    Vec3d axis1 = mSpec.axes[0].normalize();
    Vec3d axis2 = mSpec.axes[1].normalize();

    // ---- handles (Create's determineHandles) ----
    [&] {
        Vec3d const up{0, 1, 0};
        Vec3d       cross1 = axis1.cross(up);
        Vec3d       cross2 = axis2.cross(up);
        mRadius            = 0;
        double a1          = std::atan2(-axis2.z, -axis2.x);
        double a2          = std::atan2(axis1.z, axis1.x);
        double angle       = std::fmod(a1 - a2 + kCircle, double(kCircle));
        if (std::abs(kCircle - angle) < std::abs(angle)) angle = kCircle - angle;

        if (nearlyEqual(angle, 0)) {
            if (auto hit = intersect(end1, end2, axis1, cross2, Axis::Y)) {
                double t = std::abs(hit->t), u = std::abs(hit->u);
                double mn = std::min(t, u), mx = std::max(t, u);
                if (mn > 1.2 && mx / mn > 1 && mx / mn < 3) {
                    mHandleLength = mx - mn;
                    return;
                }
            }
            mHandleLength = end2.distanceTo(end1) / 3;
            return;
        }

        double n      = kCircle / angle;
        double factor = 4 / 3.0 * std::tan(std::numbers::pi / (2 * n));
        auto   hit    = intersect(end1, end2, cross1, cross2, Axis::Y);
        if (!hit) {
            mHandleLength = end2.distanceTo(end1) / 3;
            return;
        }
        mRadius       = std::abs(hit->u);
        mHandleLength = mRadius * factor;
        if (nearlyEqual(mHandleLength, 0)) mHandleLength = 1;
    }();

    mFinish1 = end1 + axis1 * mHandleLength;
    mFinish2 = end2 + axis2 * mHandleLength;

    // ---- length (16 straight pieces) ----
    constexpr int scanCount = 16;
    mLength                 = 0;
    Vec3d previous          = end1;
    for (int i = 0; i <= scanCount; ++i) {
        Vec3d r = bezier(end1, end2, mFinish1, mFinish2, i / double(scanCount));
        mLength += r.distanceTo(previous);
        previous = r;
    }

    // ---- step LUT (arc-length correction per segment) ----
    mSegments = std::max(1, int(mLength * 2));
    mStepLut.assign(size_t(mSegments) + 1, 1.0f);
    float combined = 0;
    mBounds        = Box{end1, end1};
    mBounds.include(end2);
    previous = end1;
    for (int i = 0; i <= mSegments; ++i) {
        float t = i / float(mSegments);
        Vec3d r = bezier(end1, end2, mFinish1, mFinish2, t);
        mBounds.include(r);
        if (i > 0) {
            combined    += float(r.distanceTo(previous) / mLength);
            mStepLut[i]  = combined > 0 ? t / combined : 1.0f;
        }
        previous = r;
    }
    mBounds.min -= Vec3d{1.375, 1.375, 1.375};
    mBounds.max += Vec3d{1.375, 1.375, 1.375};
}

Vec3d Curve::position(double t) const { return bezier(mSpec.starts[0], mSpec.starts[1], mFinish1, mFinish2, t); }

Vec3d Curve::tangent(double t) const {
    return bezierDerivative(mSpec.starts[0], mSpec.starts[1], mFinish1, mFinish2, t).normalize();
}

Vec3d Curve::normal(double t) const {
    Vec3d fn1        = mSpec.normals[0];
    Vec3d fn2        = mSpec.normals[1];
    Vec3d derivative = tangent(t);
    Vec3d faceNormal = fn1 == fn2 ? fn1 : slerp(t, fn1, fn2);
    Vec3d side       = faceNormal.cross(derivative).normalize();
    return derivative.cross(side);
}

double Curve::segmentT(int index) const {
    if (index >= mSegments) return 1;
    if (index <= 0) return 0;
    return index * double(mStepLut[size_t(index)]) / mSegments;
}

double Curve::incrementT(double t, double distance) const {
    double dx = bezierDerivative(mSpec.starts[0], mSpec.starts[1], mFinish1, mFinish2, t).length() / mLength;
    if (dx <= 0) return t;
    return t + distance / mLength / dx;
}

double Curve::tAtDistance(double distance) const {
    if (distance <= 0) return 0;
    if (distance >= mLength) return 1;
    // walk the segments (each ~0.5 blocks) and interpolate inside the one containing `distance`
    double walked = 0;
    Vec3d  prev   = mSpec.starts[0];
    double prevT  = 0;
    for (int i = 1; i <= mSegments; ++i) {
        double t   = segmentT(i);
        Vec3d  p   = position(t);
        double len = p.distanceTo(prev);
        if (walked + len >= distance) {
            double f = len > 0 ? (distance - walked) / len : 0;
            return prevT + (t - prevT) * f;
        }
        walked += len;
        prev    = p;
        prevT   = t;
    }
    return 1;
}

std::vector<CurveSegment> Curve::segments() const {
    std::vector<CurveSegment> out;
    out.reserve(size_t(mSegments) + 1);
    Vec3d fn1 = mSpec.normals[0], fn2 = mSpec.normals[1];
    for (int i = 0; i <= mSegments; ++i) {
        double       t = segmentT(i);
        CurveSegment s;
        s.index      = i;
        s.position   = position(t);
        s.derivative = tangent(t);
        s.faceNormal = fn1 == fn2 ? fn1 : slerp(t, fn1, fn2);
        s.normal     = s.faceNormal.cross(s.derivative).normalize();
        out.push_back(s);
    }
    return out;
}

std::map<std::pair<int, int>, double> Curve::rasterise() const {
    using Key = std::pair<int, int>;
    std::map<Key, double> yLevels;
    Vec3d const           origin = mSpec.blocks[0].corner();
    Vec3d const           lift{0, 3 / 16.0, 0};
    Vec3d                 end1    = mSpec.starts[0] - origin + lift;
    Vec3d                 end2    = mSpec.starts[1] - origin + lift;
    Vec3d                 finish1 = end1 + mSpec.axes[0] * mHandleLength;
    Vec3d                 finish2 = end2 + mSpec.axes[1] * mHandleLength;
    Vec3d                 fn1 = mSpec.normals[0], fn2 = mSpec.normals[1];

    std::vector<Vec3d> samples(static_cast<size_t>(mSegments));
    for (int i = 0; i < mSegments; ++i) {
        // Create's rasterise scales the half-segment offset by the LUT too (unlike its paver)
        double t          = std::clamp((i + 0.5) * mStepLut[size_t(i)] / mSegments, 0.0, 1.0);
        Vec3d  result     = bezier(end1, end2, finish1, finish2, t);
        Vec3d  derivative = bezierDerivative(end1, end2, finish1, finish2, t).normalize();
        Vec3d  faceNormal = fn1 == fn2 ? fn1 : slerp(t, fn1, fn2);
        Vec3d  normal     = faceNormal.cross(derivative).normalize();
        Vec3d  below      = result + faceNormal * -.25;
        Vec3d  rail1      = below + normal * .05;
        Vec3d  rail2      = below - normal * .05;
        samples[size_t(i)] = (rail1 + rail2) * .5;
    }

    Vec3d center = (end1 + end2) * .5;
    auto  diff   = [&](Key const& k) {
        return Vec3d{k.first + .5, center.y, k.second + .5}.distanceToSqr(center);
    };
    auto doubled = [](Key const& from, Key const& via, Key const& to) {
        int d1x = via.first - from.first, d1z = via.second - from.second;
        int d2x = to.first - via.first, d2z = to.second - via.second;
        return std::abs(d1x) + std::abs(d1z) == 1 && std::abs(d2x) + std::abs(d2z) == 1 && d1x != d2x && d1z != d2z;
    };

    std::optional<Key> prev, prev2, prev3;
    for (int i = 0; i < mSegments; ++i) {
        Vec3d     railMiddle = samples[size_t(i)];
        BlockPos3 pos        = containing(railMiddle);
        Key       key{pos.x, pos.z};
        auto      it             = yLevels.find(key);
        bool      alreadyPresent = it != yLevels.end();
        if (alreadyPresent && it->second <= railMiddle.y) continue;
        yLevels[key] = railMiddle.y;
        if (alreadyPresent) continue;

        if (prev3) { // remove obsolete pixels
            bool doubledViaPrev  = doubled(*prev2, *prev, key);
            bool doubledViaPrev2 = doubled(*prev3, *prev2, *prev);
            bool prevCloser      = diff(*prev) > diff(*prev2);
            if (doubledViaPrev2 && (!doubledViaPrev || !prevCloser)) {
                yLevels.erase(*prev2);
                prev2 = prev;
                prev  = key;
                continue;
            }
            if (doubledViaPrev && doubledViaPrev2 && prevCloser) {
                yLevels.erase(*prev);
                prev = key;
                continue;
            }
        }
        prev3 = prev2;
        prev2 = prev;
        prev  = key;
    }
    return yLevels;
}

std::vector<BlockPos3> Curve::occupiedBlocks() const {
    std::vector<BlockPos3> out;
    BlockPos3 const        base = mSpec.blocks[0];
    for (auto const& [xz, y] : rasterise()) {
        BlockPos3 p{base.x + xz.first, base.y + int(std::floor(y)) + 1, base.z + xz.second};
        if (p == mSpec.blocks[0] || p == mSpec.blocks[1]) continue;
        out.push_back(p);
    }
    return out;
}

bool Curve::sameShape(Curve const& other) const {
    auto same = [](CurveSpec const& a, CurveSpec const& b) {
        for (int i = 0; i < 2; ++i) {
            if (!(a.blocks[i] == b.blocks[i]) || !a.starts[i].near(b.starts[i]) || !a.axes[i].near(b.axes[i])
                || !a.normals[i].near(b.normals[i]))
                return false;
        }
        return a.girder == b.girder;
    };
    return same(mSpec, other.mSpec) || same(mSpec, other.secondary().mSpec);
}

nlohmann::json Curve::toJson() const {
    return {
        {"blocks",   {posJson(mSpec.blocks[0]), posJson(mSpec.blocks[1])}    },
        {"starts",   {vecJson(mSpec.starts[0]), vecJson(mSpec.starts[1])}    },
        {"axes",     {vecJson(mSpec.axes[0]), vecJson(mSpec.axes[1])}        },
        {"normals",  {vecJson(mSpec.normals[0]), vecJson(mSpec.normals[1])}  },
        {"girder",   mSpec.girder                                            },
        {"material", mSpec.material                                          },
        {"primary",  mPrimary                                                },
    };
}

std::optional<Curve> Curve::fromJson(nlohmann::json const& j) {
    try {
        CurveSpec s;
        for (int i = 0; i < 2; ++i) {
            s.blocks[i]  = jsonPos(j.at("blocks").at(i));
            s.starts[i]  = jsonVec(j.at("starts").at(i));
            s.axes[i]    = jsonVec(j.at("axes").at(i));
            s.normals[i] = jsonVec(j.at("normals").at(i));
        }
        s.girder   = j.value("girder", false);
        s.material = j.value("material", std::string("andesite"));
        return Curve(std::move(s), j.value("primary", true));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace lv::core::tracks
