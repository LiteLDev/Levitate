#pragma once

// Vocabulary of the fluid pipe model (pure C++): faces / axes (Create's Direction order, shared with
// the kinetic network), fluid amounts and block faces.

#include <compare>
#include <cstdint>
#include <string>

#include "core/GlobalPos.h"
#include "core/kinetics/Types.h"

namespace lv::core::pipes {

using core::GlobalPos;
using kinetics::Axis;
using kinetics::axisOf;
using kinetics::Face;
using kinetics::faceOf;
using kinetics::GlobalPosHash;
using kinetics::isPositive;
using kinetics::kAxes;
using kinetics::kFaces;
using kinetics::offsetOf;
using kinetics::opposite;
using kinetics::shifted;

/// A fluid amount: `id` is the fluid id ("minecraft:water", "levitate:honey", "potion:swiftness"),
/// `amount` in mB (1 bucket = 1000 mB).
struct Fluid {
    std::string id;
    int         amount = 0;
    [[nodiscard]] bool empty() const { return id.empty() || amount <= 0; }
    [[nodiscard]] Fluid withAmount(int n) const { return {id, n}; }
    friend bool         operator==(Fluid const&, Fluid const&) = default;
};

/// Create's `FluidStack.isSameFluidSameComponents`: same fluid (ids carry the potion type).
[[nodiscard]] inline bool sameFluid(Fluid const& a, Fluid const& b) { return !a.empty() && !b.empty() && a.id == b.id; }

/// A face of a block (Create's BlockFace): the block at `pos` and its side `face`.
struct BlockFace {
    GlobalPos pos;
    Face      face = Face::Down;

    [[nodiscard]] GlobalPos connected() const { return shifted(pos, face); }
    /// The same face seen from the other block.
    [[nodiscard]] BlockFace opposite() const { return {connected(), kinetics::opposite(face)}; }
    friend bool             operator==(BlockFace const&, BlockFace const&) = default;
    friend auto             operator<=>(BlockFace const& a, BlockFace const& b) {
        if (auto c = a.pos <=> b.pos; c != 0) return c;
        return static_cast<int>(a.face) <=> static_cast<int>(b.face);
    }
};

/// Bit mask of open faces (bit `int(face)`).
using FaceMask = std::uint8_t;
[[nodiscard]] constexpr FaceMask bit(Face f) { return static_cast<FaceMask>(1u << static_cast<int>(f)); }
[[nodiscard]] constexpr bool     has(FaceMask m, Face f) { return (m & bit(f)) != 0; }
[[nodiscard]] constexpr FaceMask axisMask(Axis a) { return bit(faceOf(a, true)) | bit(faceOf(a, false)); }
[[nodiscard]] int                count(FaceMask m);

} // namespace lv::core::pipes
