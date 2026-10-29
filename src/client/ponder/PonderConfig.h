#pragma once

// `config/ponder.json` (client mod, SPEC §5): Create's Ponder client options plus the renderer's.

#ifdef LV_CLIENT

#include "mod/Config.h"

namespace lv::client::ponder {

struct PonderConfig {
    int version = 1;
    /// Create's "Comfy Reading" default: scenes slow down while a text is shown.
    bool comfyReading = false;
    /// Hold time scale of the Ponder key (1 = Create's ~11 ticks).
    float holdSpeed = 1.0f;
    /// Depth of a scene block in the UI pass, as a fraction of its on-screen size (sign = which way
    /// the GUI depth buffer grows).
    float depthScale = 0.05f;
    /// Colour of the placeholder drawn for blocks that are not registered (ARGB).
    unsigned placeholderColour = 0xFFFF55FF;
};

[[nodiscard]] inline PonderConfig const& config() { return lv::config::get<PonderConfig>(); }

} // namespace lv::client::ponder

#endif // LV_CLIENT
