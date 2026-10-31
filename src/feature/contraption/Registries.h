#pragma once

// Lookup side of the contraption registries (internal).

#include <optional>
#include <string>
#include <vector>

#include "feature/contraption/Contraption.h"

namespace lv::feature::contraption::detail {

[[nodiscard]] MovementRules const*       movementRules(std::string const& id);
[[nodiscard]] RotationHandler const*     rotationHandler(std::string const& id);
[[nodiscard]] MovementBehaviour const*   actorFor(std::string const& id);
[[nodiscard]] MovableAt const*           movableAt(std::string const& id);
[[nodiscard]] BeforeCapture const*       beforeCapture(std::string const& id);
[[nodiscard]] bool                       hasBeforeCapture();
[[nodiscard]] std::optional<double>      seatHeight(std::string const& id);
[[nodiscard]] FluidStorageAdapter const* fluidAdapter(std::string const& id);
[[nodiscard]] ItemStorageAdapter const*  itemAdapter(std::string const& id);
[[nodiscard]] std::vector<DataCarrier> const& carriers();

} // namespace lv::feature::contraption::detail
