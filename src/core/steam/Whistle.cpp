#include "core/steam/Whistle.h"

#include <cmath>

namespace lv::core::steam {

int whistlePitch(std::function<std::optional<ExtensionShape>(int)> const& above) {
    int newPitch = 0;
    int i        = 1;
    for (newPitch = 0; newPitch <= 24; newPitch += 2) {
        auto shape = above(i);
        if (!shape) break;
        if (*shape == ExtensionShape::Single) {
            ++newPitch;
            break;
        }
        ++i;
    }
    return newPitch;
}

float whistleSoundPitch(int pitch) { return static_cast<float>(std::pow(2.0, -pitch / 12.0)); }

float octaveFactor(WhistleSize size) {
    switch (size) {
    case WhistleSize::Small:
        return 2.f;
    case WhistleSize::Large:
        return .5f;
    default:
        return 1.f;
    }
}

int pitchId(int pitch, WhistleSize size) { return pitch + 100 * static_cast<int>(size); }

IncrementAction incrementSize(std::function<std::optional<ExtensionShape>(int)> const& above,
                              std::function<bool(int)> const& replaceable) {
    for (int i = 1; i <= 6; ++i) {
        float pitch = static_cast<float>(std::pow(2.0, -(i * 2) / 12.0));
        if (auto shape = above(i)) {
            if (*shape == ExtensionShape::Single) return {IncrementAction::Widen, i, pitch};
            continue;
        }
        if (!replaceable(i)) return {};
        return {IncrementAction::Place, i, pitch};
    }
    return {};
}

ExtensionShape connectedShape(ExtensionShape current, bool extensionAbove) {
    bool connected = current == ExtensionShape::DoubleConnected;
    if (!connected && extensionAbove) return ExtensionShape::DoubleConnected;
    if (connected && !extensionAbove) return ExtensionShape::Double;
    return current;
}

} // namespace lv::core::steam
