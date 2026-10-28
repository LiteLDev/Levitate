#pragma once

// Client: curved track rendering (client build only).

#include <cstddef>

namespace lv::client::tracks {

#ifdef LV_CLIENT
/// Number of curves the client received from the server (all dimensions).
[[nodiscard]] size_t knownCurves();
#endif

} // namespace lv::client::tracks
