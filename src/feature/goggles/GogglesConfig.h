#pragma once

// `config/goggles.json` (U8): Create's CClient goggle overlay / tooltip keys (read by the client
// mod; on BDS only the Levitate keys matter) and the vanilla-client overlay.

namespace lv::feature::goggles {

struct GogglesConfig {
    int version = 2; // 2: vanillaOverlayHud

    // ---- CClient ----
    /// Show item descriptions on Shift and controls on Ctrl (enableTooltips).
    bool enableTooltips = true;
    /// Display a tooltip when looking at overstressed components (enableOverstressedTooltip).
    bool enableOverstressedTooltip = true;
    /// Offset of the goggle- and hover-overlay from the crosshair (GUI pixels; /create overlay).
    int overlayOffsetX = 20;
    int overlayOffsetY = 0;
    /// Use the custom colours below for the overlay (customColorsOverlay).
    bool customColorsOverlay = false;
    /// ARGB colours (customBackgroundOverlay, customBorderTopOverlay, customBorderBotOverlay).
    int customBackgroundOverlay = static_cast<int>(0xf0100010u);
    int customBorderTopOverlay  = static_cast<int>(0x505000ffu);
    int customBorderBotOverlay  = static_cast<int>(0x5028007fu);

    // ---- Levitate ----
    /// Show the overlay to players without the client mod.
    bool vanillaOverlay = true;
    /// Players without the client mod see the overlay as Create's panel next to the crosshair,
    /// drawn by the resource pack's HUD (ui/hud_screen.json) from a marked title; off: in the
    /// action bar (for clients that refused the resource pack).
    bool vanillaOverlayHud = true;
    /// Ticks between two looks at the targeted block (client mod queries and action bar).
    int overlayInterval = 4;
};

} // namespace lv::feature::goggles
