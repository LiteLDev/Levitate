#pragma once

// Goggle overlay contract (U8, docs/SPEC.md §3.6.8): what a block shows next to the crosshair
// (client mod) or in the action bar (vanilla clients) while a player looks at it. Create's
// IHaveGoggleInformation (only with Engineer's Goggles on), IHaveHoveringInformation (always),
// IHaveCustomOverlayIcon and IProxyHoveringInformation, plus the speed/stress text helpers of
// Create's IRotate for gauges.
//
// Defaults (Levitate blocks, unless a provider sets `replaceDefaults`): kinetic blocks show
// Create's kinetic / generator stats and the overstressed / not-fast-enough hints; blocks with
// a fluid capability show their tanks (Create's containedFluidTooltip), with an item capability
// their stored items, with a heat provider their heat level.
//
//     #include "feature/goggles/GoggleInfo.h"
//     namespace goggles = lv::feature::goggles;
//     LV_REGISTER_GOGGLE_INFO("levitate:stressometer", {
//         .goggle = [](goggles::InfoContext const& c, goggles::Tooltip& t) {
//             t.push_back(goggles::line(c.tr("create.gui.gauge.info_header", "Gauge Information:")));
//             ...
//             return true;
//         },
//     });
//
// Lines are Bedrock text with formatting codes (core/goggles/Tooltip.h `fmt::`), already
// translated for the viewer (`InfoContext::tr`). Server thread only.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/GlobalPos.h"
#include "core/goggles/Tooltip.h"
#include "core/kinetics/Solver.h"
#include "mod/I18n.h"
#include "mod/Registry.h"

class Block;
class BlockSource;
class Player;

namespace lv::feature::goggles {

using Tooltip = std::vector<std::string>;

/// The block a player looks at.
struct InfoContext {
    Player&         player;
    BlockSource&    region;
    core::GlobalPos pos;
    Block const&    block;
    bool            sneaking       = false; ///< Create's isPlayerSneaking
    bool            wearingGoggles = false;

    /// Translation in the viewer's language.
    template <class... Args>
    [[nodiscard]] std::string tr(std::string_view key, std::string_view fallback, Args const&... args) const {
        return trFor(player, key, fallback, args...);
    }
};

struct InfoProvider {
    /// Create's addToGoggleTooltip: only asked while wearing goggles; true when it added lines.
    std::function<bool(InfoContext const&, Tooltip&)> goggle;
    /// Create's addToTooltip (IHaveHoveringInformation): asked with or without goggles.
    std::function<bool(InfoContext const&, Tooltip&)> hover;
    /// Item id drawn on the panel (Create's IHaveCustomOverlayIcon; default "levitate:goggles").
    std::function<std::string(InfoContext const&)> icon;
    /// Create's IProxyHoveringInformation: show the information of another position.
    std::function<std::optional<core::GlobalPos>(InfoContext const&)> proxy;
    /// Don't add Levitate's default lines (kinetic stats, fluids, items, heat) for this block.
    bool replaceDefaults = false;
};

/// Adds a provider for `blockId` (several providers of a block are asked in registration order).
void registerBlock(std::string blockId, InfoProvider provider);

/// Create's GogglesItem.addIsWearingPredicate (default: Engineer's Goggles in the head slot).
void addWearingPredicate(std::function<bool(Player const&)> predicate);
[[nodiscard]] bool isWearingGoggles(Player const& player);

// ---- kinetic block properties used by the overlay and the item tooltips ----------------------

using core::kinetics::SpeedLevel;
/// Create's IRotate.getMinimumRequiredSpeedLevel ("not fast enough" hint).
void setSpeedRequirement(std::string blockId, SpeedLevel level);
[[nodiscard]] SpeedLevel speedRequirementOf(std::string_view blockId);
/// Create's IRotate.hideStressImpact (no impact line in the item tooltip).
void setHideStressImpact(std::string blockId, bool hide = true);
[[nodiscard]] bool hidesStressImpact(std::string_view blockId);
/// Create's BlockStressValues.setGeneratorSpeed (item tooltip "-> N su"; `mayGenerateLess`
/// prints "Up to N su"). Create's values for the hand crank, valve handles, water wheels and the
/// creative motor are preset.
struct GeneratorSpeed {
    double rpm             = 0;
    bool   mayGenerateLess = false;
};
void setGeneratorSpeed(std::string blockId, double rpm, bool mayGenerateLess = false);
[[nodiscard]] std::optional<GeneratorSpeed> generatorSpeedOf(std::string_view blockId);

// ---- line helpers (Create's LangBuilder / TooltipHelper / IRotate) ----------------------------

/// Create's forGoggles: 4 + `indent` spaces, then `text`.
[[nodiscard]] inline std::string line(std::string_view text, int indent = 0) { return core::goggles::forGoggles(text, indent); }
/// Create's CreateLang.number.
[[nodiscard]] inline std::string number(double v) { return core::goggles::formatNumber(v); }
/// Create's TooltipHelper.cutTextComponent(hint, GRAY_AND_WHITE) added with forGoggles.
void addHint(Tooltip& tooltip, std::string const& text);
/// Create's SpeedLevel.getFormattedSpeedText (gauges).
[[nodiscard]] std::string formattedSpeed(Player const& viewer, float speed, bool overstressed);
/// Create's StressImpact.getFormattedStressText (gauges; `percent` 0..1+).
[[nodiscard]] std::string formattedStress(Player const& viewer, double percent);

/// Create's KineticBlockEntity / GeneratingKineticBlockEntity tooltips, for providers that
/// replace the defaults but want them too ("super" calls).
bool kineticGoggleTooltip(InfoContext const& ctx, Tooltip& tooltip);
bool kineticHoverTooltip(InfoContext const& ctx, Tooltip& tooltip);
/// Create's IHaveGoggleInformation.containedFluidTooltip over the fluid capability.
bool fluidTooltip(InfoContext const& ctx, Tooltip& tooltip);

/// The overlay of the block at `pos` for `player` (what the client mod draws and the action bar
/// shows). Empty lines: nothing to show.
struct Overlay {
    Tooltip     lines;
    std::string icon = "levitate:goggles";
};
[[nodiscard]] Overlay overlayFor(Player& player, core::GlobalPos const& pos);

namespace detail {
struct InfoRegistrar {
    InfoRegistrar(std::string id, InfoProvider p) { registerBlock(std::move(id), std::move(p)); }
};
} // namespace detail

} // namespace lv::feature::goggles

/// Registers a goggle info provider at static init: `LV_REGISTER_GOGGLE_INFO("levitate:x", {...});`
#define LV_REGISTER_GOGGLE_INFO(id, ...)                                                                               \
    static ::lv::feature::goggles::detail::InfoRegistrar LV_UNIQUE(lv_goggle_info_)(id, __VA_ARGS__)
