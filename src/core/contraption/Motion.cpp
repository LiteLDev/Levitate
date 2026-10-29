#include "core/contraption/Motion.h"

#include <cmath>

namespace lv::core::contraption {

Transform lerp(Transform const& a, Transform const& b, double t) {
    if (a.axis != b.axis || t >= 1) return b;
    Transform out = b;
    Vec3d     from = a.origin + a.offset, to = b.origin + b.offset;
    out.origin     = from + (to - from) * t;
    out.offset     = {};
    double delta   = std::remainder(b.angle - a.angle, 360.0); // -180..180
    out.angle      = b.angle - delta * (1 - t);
    return out;
}

void MotionBuffer::reset(Transform const& t) {
    mReady = true;
    mPrev = mCur = t;
    mQueue.clear();
}

void MotionBuffer::push(Transform const& t) {
    if (!mReady) return reset(t);
    mQueue.push_back(t);
    while (mQueue.size() > kMaxQueued) mQueue.pop_front();
}

void MotionBuffer::step() {
    mPrev = mCur;
    if (mQueue.empty()) return;
    mCur = mQueue.back();
    mQueue.clear();
}

} // namespace lv::core::contraption
