#include "core/trains/Assembly.h"

#include <algorithm>
#include <cstdlib>

namespace lv::core::trains {

bool canPlaceBogey(int index, std::vector<int> const& existing) {
    if (index < 0) return false;
    for (int e : existing)
        if (std::abs(e - index) <= 2) return false;
    return index == 0 || index >= 3;
}

std::optional<AssemblyError> checkBogeys(std::vector<int> const& indices, int maxBogeys) {
    if (indices.empty()) return AssemblyError{"create.train_assembly.no_bogeys", "No Bogeys Found", {}};
    if (indices.front() != 0)
        return AssemblyError{"create.train_assembly.frontmost_bogey_at_station", "Frontmost Bogey must be at Station Marker", {}};
    for (size_t i = 1; i < indices.size(); ++i) {
        if (indices[i] - indices[i - 1] < 3) {
            return AssemblyError{
                "create.train_assembly.bogeys_too_close",
                "Bogeys {0} and {1} are too close to each other",
                {std::to_string(i), std::to_string(i + 1)}
            };
        }
    }
    if (maxBogeys > 0 && int(indices.size()) > maxBogeys) {
        return AssemblyError{"create.train_assembly.too_many_bogeys", "Too many Bogeys attached: {0}", {std::to_string(indices.size())}};
    }
    return std::nullopt;
}

std::variant<Layout, AssemblyError> planCarriages(
    std::vector<int> const&                     indices,
    std::function<std::vector<int>(int)> const& touched,
    std::function<int(int)> const&              blocks
) {
    Layout out;
    int    n = int(indices.size());
    for (int i = 0; i < n;) {
        int carriageNo = int(out.carriages.size()) + 1;
        if (blocks(i) <= 1) {
            return AssemblyError{
                "create.train_assembly.nothing_attached",
                "No structure attached to Bogey {0}",
                {std::to_string(i + 1)},
                carriageNo
            };
        }
        auto held = touched(i);
        std::sort(held.begin(), held.end());
        held.erase(std::unique(held.begin(), held.end()), held.end());
        if (held.empty() || held.front() != i) held.insert(held.begin(), i);
        if (held.size() > 2) {
            return AssemblyError{
                "create.train_assembly.too_many_bogeys",
                "Too many Bogeys attached: {0}",
                {std::to_string(held.size())},
                carriageNo
            };
        }
        if (held.size() == 2 && held[1] != i + 1) {
            return AssemblyError{"create.train_assembly.not_connected_in_order", "Bogeys are not connected in order", {}, carriageNo};
        }
        Carriage c;
        c.bogeys = held;
        out.bogeySpacing.push_back(held.size() == 2 ? indices[held[1]] - indices[held[0]] : 0);
        out.carriageSpacing.push_back(out.carriages.empty() ? 0 : indices[held[0]] - indices[out.carriages.back().bogeys.back()]);
        out.carriages.push_back(std::move(c));
        i += int(held.size());
    }
    return out;
}

std::optional<AssemblyError> checkControls(std::vector<int> const& controls, bool& forward, bool& backward) {
    for (int c : controls) {
        if (c == 0) return AssemblyError{"create.train_assembly.sideways_controls", "Train Controls cannot face sideways", {}};
        if (c > 0) forward = true;
        else backward = true;
    }
    return std::nullopt;
}

} // namespace lv::core::trains
