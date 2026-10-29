#pragma once

// Client side motion of a contraption (U34): the server sends the contraption's transform each
// tick it changes; each client tick the client takes the newest one and draws in between at the
// frame's partial tick, as Create's clients interpolate a contraption entity's motion.

#include <cstddef>
#include <deque>

#include "core/contraption/Geometry.h"

namespace lv::core::contraption {

/// The transform at `t` (0 = `a`, 1 = `b`): translation and angle interpolated (the angle along
/// the shorter way), axis and pivot taken from `b` (a change of axis jumps to `b`).
[[nodiscard]] Transform lerp(Transform const& a, Transform const& b, double t);

/// Transforms received from the server, taken at each client tick.
class MotionBuffer {
public:
    /// Most transforms kept waiting between two client ticks (only the newest is used).
    static constexpr std::size_t kMaxQueued = 2;

    /// Starts at `t` (no motion).
    void reset(Transform const& t);
    /// A transform from the server (the first one also starts the buffer).
    void push(Transform const& t);
    /// One client tick: the newest transform becomes the current one (standing still if none
    /// arrived; when several arrived, as after a lag spike, the step covers them all, so the
    /// drawing never stays behind the server).
    void step();

    [[nodiscard]] bool             ready() const { return mReady; }
    [[nodiscard]] Transform const& previous() const { return mPrev; }
    [[nodiscard]] Transform const& current() const { return mCur; }
    [[nodiscard]] std::size_t      queued() const { return mQueue.size(); }
    /// The transform drawn at `partialTick` (0..1) of the current client tick.
    [[nodiscard]] Transform at(double partialTick) const { return lerp(mPrev, mCur, partialTick); }

private:
    bool                  mReady = false;
    Transform             mPrev, mCur;
    std::deque<Transform> mQueue;
};

} // namespace lv::core::contraption
