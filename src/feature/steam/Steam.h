#pragma once

// Steam unit (U18) query API: fluid tanks, boilers, steam engines and whistles.
//
// For the goggles overlay (U8) and debugging: `goggleLines(pos, viewer)` gives Create's goggle
// tooltip lines (boiler status with its size / water / heat bars and the provided capacity, or
// the fluid container info) for a fluid tank, steam engine or steam whistle; empty for other
// blocks. Other units reach tanks through the fluid capability (feature/common/Capabilities.h).
// Server thread only.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/GlobalPos.h"

class Player;

namespace lv::feature::steam {

/// A fluid tank structure as seen from one of its blocks.
struct TankView {
    core::GlobalPos controller;
    int             width = 1, height = 1;
    std::string     fluid;
    std::int64_t    amount = 0, capacity = 0;
    bool            window = true, creative = false;
    bool            boiler = false;
    int             boilerLevel = 0; ///< 0..18 (Create's actual heat)
    bool            passive = false;
};
[[nodiscard]] std::optional<TankView> tank(core::GlobalPos const& pos);

/// Create's goggle lines (with § colour codes) in the viewer's language (nullptr: default).
[[nodiscard]] std::vector<std::string> goggleLines(core::GlobalPos const& pos, Player const* viewer = nullptr);

/// Create's comparator output of a tank (0..15).
[[nodiscard]] int comparatorLevel(core::GlobalPos const& pos);

/// The pitch (semitones of extension, 0..25) of the whistle at `pos`.
[[nodiscard]] int whistlePitch(core::GlobalPos const& pos);

} // namespace lv::feature::steam
