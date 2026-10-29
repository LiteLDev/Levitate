#pragma once

// Steam whistles (U18, SPEC §3.6.18): Create 6.0.10's WhistleBlock / WhistleBlockEntity /
// WhistleExtenderBlock numbers, independent of the game.

#include <functional>
#include <optional>

namespace lv::core::steam {

/// Create's WhistleSize (the octave): small sounds an octave above medium, large one below.
enum class WhistleSize : int { Small = 0, Medium = 1, Large = 2 };
/// Create's WhistleExtenderShape.
enum class ExtensionShape : int { Single = 0, Double = 1, DoubleConnected = 2 };

/// Create's WhistleBlockEntity.updatePitch: walks up from the whistle; every full extension adds
/// two semitones, a half (single) extension one and ends the column. `above(i)` is the shape of
/// the block i (1-based) above the whistle, nullopt when it is not an extension.
[[nodiscard]] int whistlePitch(std::function<std::optional<ExtensionShape>(int)> const& above);

/// The sound pitch of a whistle with `pitch` semitones of extension: 2^(-pitch / 12).
[[nodiscard]] float whistleSoundPitch(int pitch);
/// Pitch factor of the size's octave relative to the medium whistle (small 2, medium 1, large .5).
[[nodiscard]] float octaveFactor(WhistleSize size);
/// Create's getPitchId: pitch + 100 · size.
[[nodiscard]] int pitchId(int pitch, WhistleSize size);

/// What a click with a whistle on a whistle column does (Create's WhistleBlock.incrementSize).
struct IncrementAction {
    enum Kind { None, Widen, Place } kind = None;
    int   offset = 0;  ///< blocks above the whistle
    float soundPitch = 1; ///< 2^(-(offset·2)/12)
};
/// `above(i)` as in `whistlePitch`; `replaceable(i)` tells whether block i above can be replaced.
[[nodiscard]] IncrementAction incrementSize(std::function<std::optional<ExtensionShape>(int)> const& above,
                                            std::function<bool(int)> const& replaceable);

/// Create's WhistleExtenderBlock.updateShape (towards UP): a full extension shows its
/// connected shape when another extension is above it.
[[nodiscard]] ExtensionShape connectedShape(ExtensionShape current, bool extensionAbove);

} // namespace lv::core::steam
