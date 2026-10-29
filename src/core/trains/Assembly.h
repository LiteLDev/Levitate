#pragma once

// Train assembly rules (pure C++): Create 6.0.10's StationBlockEntity bogey checks and how
// bogeys group into carriages.
//
// The assembly track starts one block past the station's track in the assembly direction;
// a bogey's index is its distance (blocks) from that start.

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lv::core::trains {

struct AssemblyError {
    std::string              key;      ///< create.train_assembly.* / create.gui.assembly.exception.*
    std::string              fallback; ///< English text with {0} placeholders
    std::vector<std::string> args;
    int                      carriage = 0; ///< 1-based carriage number (0: the whole train)
};

/// Whether a new bogey may be created at assembly index `index` (Create: at the station marker,
/// or at least 3 blocks in and not within 2 blocks of another bogey).
[[nodiscard]] bool canPlaceBogey(int index, std::vector<int> const& existing);

/// Checks the bogeys found on the assembly track (sorted indices): at least one, the first one
/// at the station marker (index 0), consecutive bogeys at least 3 blocks apart.
[[nodiscard]] std::optional<AssemblyError> checkBogeys(std::vector<int> const& indices, int maxBogeys);

struct Carriage {
    std::vector<int> bogeys; ///< positions in the bogey list (one or two)
};

struct Layout {
    std::vector<Carriage> carriages;
    std::vector<int>      bogeySpacing;    ///< per carriage: blocks between its two bogeys (0: one bogey)
    std::vector<int>      carriageSpacing; ///< between carriage i-1's last bogey and carriage i's first (index 0 unused)
};

/// Groups the bogeys into carriages. `touched(i)` returns the positions (in the bogey list) of
/// every bogey the structure built on bogey `i` holds (including `i`), `blocks(i)` its size.
[[nodiscard]] std::variant<Layout, AssemblyError> planCarriages(
    std::vector<int> const&                     indices,
    std::function<std::vector<int>(int)> const& touched,
    std::function<int(int)> const&              blocks
);

/// Create's controls check for one carriage: `controls` are the facings of its Train Controls
/// relative to the assembly direction (+1 facing it, -1 facing against it, 0 sideways).
/// Returns the error for sideways controls; sets `forward` / `backward`.
[[nodiscard]] std::optional<AssemblyError> checkControls(std::vector<int> const& controls, bool& forward, bool& backward);

} // namespace lv::core::trains
