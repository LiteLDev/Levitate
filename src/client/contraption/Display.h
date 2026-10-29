#pragma once

// Client mod internals of the contraption unit (not a contract): how a display entity is drawn.

#ifdef LV_CLIENT

class Actor;

namespace lv::client::contraption {

/// The turn of a display entity about its block centre at `partialTick` (0..1), from the
/// contraption motion the server sent: the axis and the angle left after the quarter turns the
/// displayed block already shows. False when the actor isn't a known display.
[[nodiscard]] bool displayTurn(Actor const& actor, float partialTick, int& axis, float& degrees);

} // namespace lv::client::contraption

#endif // LV_CLIENT
