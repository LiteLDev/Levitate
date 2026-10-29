#pragma once

// Text helpers of Create's tooltips and goggle overlay (pure C++, unit-tested): number format,
// progress bars, goggle indentation, word wrap with _highlights_ and item descriptions.
// Colours are Bedrock formatting codes ("§7" gray, "§f" white, ...).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lv::core::goggles {

/// Bedrock formatting codes for Create's ChatFormatting colours.
namespace fmt {
inline constexpr char const* kGray       = "§7";
inline constexpr char const* kDarkGray   = "§8";
inline constexpr char const* kWhite      = "§f";
inline constexpr char const* kGold       = "§6";
inline constexpr char const* kAqua       = "§b";
inline constexpr char const* kRed        = "§c";
inline constexpr char const* kYellow     = "§e";
inline constexpr char const* kGreen      = "§a";
inline constexpr char const* kPurple     = "§d"; ///< LIGHT_PURPLE
inline constexpr char const* kReset      = "§r";
/// Create's Palette.STANDARD_CREATE (0xC9974C / 0xF1DD79): Bedrock's material gold and yellow.
inline constexpr char const* kCreatePrimary   = "§p";
inline constexpr char const* kCreateHighlight = "§e";
} // namespace fmt

/// Create's tooltip wrap width (TooltipHelper.MAX_WIDTH_PER_LINE, GUI pixels).
inline constexpr int kMaxWidthPerLine = 200;

/// Create's LangNumberFormat: grouping, at most 2 fraction digits, no trailing zeros
/// (1234.5 -> "1,234.5", 16384 -> "16,384", -0.001 -> "0").
[[nodiscard]] std::string formatNumber(double value);

/// Create's TooltipHelper.makeProgressBar: " " + filled "█" + empty "▒" + " ".
[[nodiscard]] std::string progressBar(int length, int filled);

/// Create's LangBuilder.forGoggles: the line indented by 4 + `indent` spaces.
[[nodiscard]] std::string forGoggles(std::string_view text, int indent = 0);

/// Removes Bedrock formatting codes ("§x").
[[nodiscard]] std::string stripFormatting(std::string_view text);

/// Width in GUI pixels of `text` in Minecraft's default font (formatting codes and Create's
/// highlight underscores excluded). An approximation of the glyph widths: most ASCII 6 px,
/// narrow punctuation less, CJK / full-width 9 px.
[[nodiscard]] int textWidth(std::string_view text);

/// Create's TooltipHelper.cutTextComponent: splits `text` into words (break after spaces and
/// around CJK characters), wraps at `maxWidth` and colours the parts between underscores with
/// `highlight`, the rest with `primary`. Each line starts with `indent` spaces.
[[nodiscard]] std::vector<std::string>
cutText(std::string_view text, std::string_view primary, std::string_view highlight, int indent = 0, int maxWidth = kMaxWidthPerLine);

/// Translated pieces of an item description (Create's ItemDescription.Builder input).
struct DescriptionText {
    std::vector<std::string>                         summary;    ///< `<key>.summary`
    std::vector<std::pair<std::string, std::string>> behaviours; ///< condition_i, behaviour_i
    std::vector<std::pair<std::string, std::string>> actions;    ///< control_i, action_i
    /// create.tooltip.holdForDescription / holdForControls with one `{}` (or `{0}`) for the key.
    std::string holdForDescription = "Hold [{}] for Summary";
    std::string holdForControls    = "Hold [{}] for Controls";
    std::string keyShift           = "Shift";
    std::string keyCtrl            = "Ctrl";
};

/// Create's ItemDescription: the lines inserted below the item name (no key held, Shift held,
/// Ctrl held). Empty when there is no summary.
[[nodiscard]] std::vector<std::string> describeItem(DescriptionText const& text, bool shift, bool ctrl);

} // namespace lv::core::goggles
