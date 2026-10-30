#include "core/equipment/ToolboxLid.h"

#include <algorithm>
#include <cmath>

namespace lv::core::equipment {

namespace {
constexpr float kSpeed = .2f;
/// LerpedFloat settles an exponential chase once it is this close (and snaps to the target).
constexpr float kSettle = 1.f / 512.f;

bool isToolboxId(std::string const& id) {
    return id.starts_with("levitate:") && id.ends_with("_toolbox") && id.size() > 9 + 8;
}
bool isDyeId(std::string const& id) { return id.starts_with("minecraft:") && id.ends_with("_dye"); }
} // namespace

void ToolboxLid::tick(bool open) {
    float target = open ? 1.f : 0.f;
    // LINEAR: a fixed step towards the target
    if (lid < target) lid = std::min(target, lid + kSpeed);
    else lid = std::max(target, lid - kSpeed);
    if (std::abs(target - lid) < kSettle) lid = target;
    // EXP: a fifth of the remaining distance
    drawers += (target - drawers) * kSpeed;
    if (std::abs(target - drawers) < kSettle) drawers = target;
}

bool ToolboxLid::settled(bool open) const {
    float target = open ? 1.f : 0.f;
    return lid == target && drawers == target;
}

std::optional<int> dyeingToolboxSlot(std::vector<std::string> const& grid) {
    int toolboxes = 0, dyes = 0, slot = -1;
    for (int i = 0; i < static_cast<int>(grid.size()); ++i) {
        auto const& id = grid[static_cast<std::size_t>(i)];
        if (id.empty()) continue;
        if (isToolboxId(id)) {
            ++toolboxes;
            slot = i;
        } else if (isDyeId(id)) {
            ++dyes;
        } else {
            return std::nullopt;
        }
        if (toolboxes > 1 || dyes > 1) return std::nullopt;
    }
    if (toolboxes == 1 && dyes == 1) return slot;
    return std::nullopt;
}

} // namespace lv::core::equipment
