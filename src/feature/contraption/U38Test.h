#pragma once

// Selftests of U38 (dx 3800..3899): `ctx.testPos(dx)` puts row 38 at spawn z + 145, outside the
// area the selftest run keeps ticking (spawn z -64..+64), where the entities of a test vanish in a
// full run. U38's tests pass this dz, which moves their row to spawn z - 60 (rows of negative dx,
// used by no unit): `ctx.testPos(3800 + n, kU38Dz)`.

namespace lv::feature::contraption {

inline constexpr int kU38Dz = -205;

} // namespace lv::feature::contraption
