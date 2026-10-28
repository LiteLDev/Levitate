#include "core/kinetics/Spec.h"

namespace lv::core::kinetics {

char const* faceName(Face f) {
    static constexpr char const* kNames[] = {"down", "up", "north", "south", "west", "east"};
    return kNames[static_cast<int>(f)];
}

char const* axisName(Axis a) {
    static constexpr char const* kNames[] = {"x", "y", "z"};
    return kNames[static_cast<int>(a)];
}

void KineticSpec::rotorParts(RenderView const& view, std::vector<RotorPart>& out) const {
    if (rotorState.empty() || !view.state) return;
    if (parts) {
        parts(view, out);
        return;
    }
    auto axis = axisOf(*view.state);
    if (!axis) return;
    out.push_back(RotorPart{"rotor", *axis, 1.f, 0.f, cogOf(*view.state) == CogType::Large});
}

std::string defaultStressKey(std::string_view id) {
    auto colon = id.find(':');
    return std::string(colon == std::string_view::npos ? id : id.substr(colon + 1));
}

namespace specs {

std::function<std::optional<Axis>(BlockState const&)> axisFromState(std::string state) {
    return [state = std::move(state)](BlockState const& s) -> std::optional<Axis> {
        int v = s.get(state, 1);
        if (v < 0 || v > 2) return std::nullopt;
        return static_cast<Axis>(v);
    };
}

std::function<std::optional<Axis>(BlockState const&)> axisFromFacing(std::string state) {
    return [state = std::move(state)](BlockState const& s) -> std::optional<Axis> {
        int v = s.get(state, 0);
        if (v < 0 || v > 5) return std::nullopt;
        return lv::core::kinetics::axisOf(static_cast<Face>(v));
    };
}

namespace {
KineticSpec axial(std::string axisState) {
    KineticSpec spec;
    spec.states       = {axisState};
    spec.rotationAxis = axisFromState(axisState);
    spec.hasShaftTowards = [axis = spec.rotationAxis](BlockState const& s, Face f) {
        auto a = axis(s);
        return a && *a == lv::core::kinetics::axisOf(f);
    };
    spec.rotorState = "levitate:rotor";
    return spec;
}
} // namespace

KineticSpec withModel(KineticSpec spec, std::string model, std::string texture) {
    spec.rotorModel   = std::move(model);
    spec.rotorTexture = std::move(texture);
    return spec;
}

KineticSpec shaft(std::string axisState) { return axial(std::move(axisState)); }

KineticSpec cogwheel(bool large, std::string axisState) {
    auto spec = axial(std::move(axisState));
    spec.cog  = [large](BlockState const&) { return large ? CogType::Large : CogType::Small; };
    return spec;
}

KineticSpec generatorShaft(double capacity, std::string axisState) {
    auto spec      = axial(std::move(axisState));
    spec.generator = true;
    spec.capacity  = capacity;
    return spec;
}

float gearboxModifier(NodeView const& self, Face face) {
    // Create's RotationPropagator.getAxisModifier for GearboxBlockEntity.
    if (!(self.hasSource() || self.generator)) return 1;
    auto source = self.sourceFacing();
    if (!source) return 1;
    if (lv::core::kinetics::axisOf(face) == lv::core::kinetics::axisOf(*source)) return face == *source ? 1.f : -1.f;
    return isPositive(face) == isPositive(*source) ? -1.f : 1.f;
}

float splitShaftModifier(NodeView const& self, Face face, bool active, bool reverse) {
    // Create's GearshiftBlockEntity / ClutchBlockEntity.getRotationSpeedModifier.
    if (!(self.hasSource() || self.generator)) return 1;
    auto source = self.sourceFacing();
    if (self.hasSource() && source && face != *source && active) return reverse ? -1.f : 0.f;
    return 1;
}

} // namespace specs

} // namespace lv::core::kinetics
