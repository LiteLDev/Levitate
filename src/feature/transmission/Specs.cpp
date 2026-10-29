// Kinetic specs of the transmission blocks (U6): Create's IRotate shapes and the special
// propagation rules (gearbox / split shaft halves, chain drives, the rotation speed controller,
// gauges). Rotating parts are drawn by the client mod from `transmission_rotors.geo.json`.

#include <algorithm>

#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"

namespace lv::feature::transmission {

namespace k = lv::kinetics;
using k::BlockState;
using k::CogType;
using k::KineticSpec;
using k::NodeView;
using k::RenderView;
using k::RotorPart;

namespace {

constexpr char kRotorModel[]   = "geometry.levitate.transmission_rotors";
constexpr char kRotorTexture[] = "levitate_transmission_rotor";

Axis axisValue(BlockState const& s, char const* state = kAxis) {
    return static_cast<Axis>(std::clamp(s.get(state, 1), 0, 2));
}

KineticSpec base(std::vector<std::string> states) {
    KineticSpec spec;
    spec.states       = std::move(states);
    spec.rotorState   = kRotor;
    spec.rotorModel   = kRotorModel;
    spec.rotorTexture = kRotorTexture;
    return spec;
}

/// A block turning about `levitate:axis` with shaft ends on both faces of it.
KineticSpec axial(std::vector<std::string> states, char const* bone) {
    auto spec         = base(std::move(states));
    spec.rotationAxis = [](BlockState const& s) -> std::optional<Axis> { return axisValue(s); };
    spec.hasShaftTowards = [](BlockState const& s, Face f) { return ck::axisOf(f) == axisValue(s); };
    spec.parts = [bone = std::string(bone)](RenderView const& v, std::vector<RotorPart>& out) {
        out.push_back(RotorPart{bone, axisValue(*v.state), 1.f, 0.f, false});
    };
    return spec;
}

char const* halfBone(Face f) { return ck::isPositive(f) ? "half_pos" : "half_neg"; }

/// The two halves of a split shaft along the block's axis, each turned by `modifier(face)`.
void splitHalves(RenderView const& v, std::vector<RotorPart>& out, std::function<float(Face)> const& modifier) {
    Axis axis = axisValue(*v.state);
    for (bool positive : {false, true}) {
        Face f = ck::faceOf(axis, positive);
        out.push_back(RotorPart{halfBone(f), axis, modifier(f), 0.f, false});
    }
}

// ---- speed controller <-> large cog (Create's isLargeCogToSpeedController) ---------------------------

bool isController(BlockState const& s) { return s.id == kSpeedController; }

/// `cog` is a large cog sitting on top of the controller `ctrl` with a horizontal axis across it.
bool cogOnController(NodeView const& cog, NodeView const& ctrl) {
    if (!cog.state || !ctrl.state || !isLargeCogBlock(cog.state->id) || !isController(*ctrl.state)) return false;
    auto d = ck::diff(cog.pos, ctrl.pos);
    if (!(d == ck::Offset{0, -1, 0})) return false;
    Axis cogAxis = axisValue(*cog.state);
    if (cogAxis == Axis::Y) return false;
    return axisValue(*ctrl.state) != cogAxis;
}

float largeCogPropagate(NodeView const& from, NodeView const& to, ck::Offset, bool, bool) {
    return cogOnController(from, to) ? 1.f : 0.f;
}

// ---- chain drives ---------------------------------------------------------------------------------------

ct::ChainState chainOf(BlockState const& s) {
    return ct::ChainState{axisValue(s), static_cast<ct::ChainPart>(std::clamp(s.get(kPart, 0), 0, 3)), s.get(kAlongFirst, 0) != 0};
}

float chainModifier(BlockState const& s) {
    return s.id == kChainGearshift ? ct::chainGearshiftModifier(std::clamp(s.get(kSignal, 0), 0, 15)) : 1.f;
}

float chainPropagate(NodeView const& from, NodeView const& to, ck::Offset d, bool, bool) {
    if (!from.state || !to.state || !isChainDrive(to.state->id)) return 0;
    if (std::abs(d.x) + std::abs(d.y) + std::abs(d.z) != 1) return 0;
    Face dir = ck::nearestFace(d.x, d.y, d.z);
    if (!ct::chainConnected(chainOf(*from.state), chainOf(*to.state), dir)) return 0;
    return chainModifier(*from.state) / chainModifier(*to.state);
}

KineticSpec chainDrive(bool gearshift) {
    std::vector<std::string> states{kAxis, kPart, kAlongFirst};
    if (gearshift) states.push_back(kSignal);
    auto spec      = axial(std::move(states), "shaft");
    spec.propagate = chainPropagate;
    return spec;
}

// ---- gauges ----------------------------------------------------------------------------------------------

Axis gaugeAxis(BlockState const& s) {
    return ct::gaugeRotationAxis(static_cast<Face>(std::clamp(s.get(kFacing, 2), 0, 5)), s.get(kAlongFirst, 0) != 0);
}

KineticSpec gauge() {
    auto spec            = base({kFacing, kAlongFirst});
    spec.rotationAxis    = [](BlockState const& s) -> std::optional<Axis> { return gaugeAxis(s); };
    spec.hasShaftTowards = [](BlockState const& s, Face f) { return ck::axisOf(f) == gaugeAxis(s); };
    spec.parts           = [](RenderView const& v, std::vector<RotorPart>& out) {
        out.push_back(RotorPart{"shaft", gaugeAxis(*v.state), 1.f, 0.f, false});
    };
    return spec;
}

// ---- encased cogwheels ------------------------------------------------------------------------------------

KineticSpec encasedCog(bool large) {
    auto spec            = base({kAxis, kTopShaft, kBottomShaft});
    spec.rotationAxis    = [](BlockState const& s) -> std::optional<Axis> { return axisValue(s); };
    spec.hasShaftTowards = [](BlockState const& s, Face f) {
        return ck::axisOf(f) == axisValue(s) && s.get(ck::isPositive(f) ? kTopShaft : kBottomShaft, 0) != 0;
    };
    spec.cog   = [large](BlockState const&) { return large ? CogType::Large : CogType::Small; };
    spec.parts = [large](RenderView const& v, std::vector<RotorPart>& out) {
        Axis axis = axisValue(*v.state);
        out.push_back(RotorPart{large ? "encased_large_cog" : "encased_cog", axis, 1.f, 0.f, large});
        if (v.state->get(kTopShaft, 0)) out.push_back(RotorPart{"half_pos", axis, 1.f, 0.f, large});
        if (v.state->get(kBottomShaft, 0)) out.push_back(RotorPart{"half_neg", axis, 1.f, 0.f, large});
    };
    if (large) spec.propagate = largeCogPropagate;
    return spec;
}

KineticSpec cogwheel(bool large) {
    auto spec = axial({kAxis}, large ? "large_cog" : "cog");
    spec.cog  = [large](BlockState const&) { return large ? CogType::Large : CogType::Small; };
    spec.parts = [large](RenderView const& v, std::vector<RotorPart>& out) {
        out.push_back(RotorPart{large ? "large_cog" : "cog", axisValue(*v.state), 1.f, 0.f, large});
    };
    if (large) spec.propagate = largeCogPropagate;
    return spec;
}

// ---- gearbox ------------------------------------------------------------------------------------------------

KineticSpec gearbox() {
    auto spec            = base({kAxis});
    spec.rotationAxis    = [](BlockState const& s) -> std::optional<Axis> { return axisValue(s); };
    spec.hasShaftTowards = [](BlockState const& s, Face f) { return ck::axisOf(f) != axisValue(s); };
    spec.axisModifier    = [](NodeView const& self, Face face) { return k::specs::gearboxModifier(self, face); };
    spec.parts           = [](RenderView const& v, std::vector<RotorPart>& out) {
        Axis box = axisValue(*v.state);
        for (Face f : ck::kFaces) {
            if (ck::axisOf(f) == box) continue;
            float m = 1;
            // Create's GearboxRenderer
            if (v.speed != 0 && v.sourceFacing) {
                Face src = *v.sourceFacing;
                if (ck::axisOf(src) == ck::axisOf(f)) m = src == f ? 1.f : -1.f;
                else if (ck::isPositive(src) == ck::isPositive(f)) m = -1.f;
            }
            out.push_back(RotorPart{halfBone(f), ck::axisOf(f), m, 0.f, false});
        }
    };
    return spec;
}

// ---- clutch / gearshift / sequenced gearshift ------------------------------------------------------------

KineticSpec splitShaft(bool reverse) {
    auto spec         = axial({kAxis, kPowered}, "shaft");
    spec.axisModifier = [reverse](NodeView const& self, Face face) {
        return k::specs::splitShaftModifier(self, face, self.state && self.state->get(kPowered, 0) != 0, reverse);
    };
    spec.parts = [reverse](RenderView const& v, std::vector<RotorPart>& out) {
        bool powered = v.state->get(kPowered, 0) != 0;
        splitHalves(v, out, [&](Face f) {
            if (v.sourceFacing && f != *v.sourceFacing && powered) return reverse ? -1.f : 0.f;
            return 1.f;
        });
    };
    return spec;
}

KineticSpec sequencedGearshift() {
    auto spec         = axial({kAxis, kSeqState, kSeqMod}, "shaft");
    spec.axisModifier = [](NodeView const& self, Face face) -> float {
        // Create's SequencedGearshiftBlockEntity.getRotationSpeedModifier
        if (!self.hasSource() || !self.state) return 1;
        auto src = self.sourceFacing();
        if (src && face == *src) return 1;
        return static_cast<float>(ct::stateToModifier(self.state->get(kSeqMod, 0)));
    };
    spec.parts = [](RenderView const& v, std::vector<RotorPart>& out) {
        int mod = ct::stateToModifier(v.state->get(kSeqMod, 0));
        splitHalves(v, out, [&](Face f) {
            if (!v.sourceFacing || f == *v.sourceFacing) return 1.f;
            return static_cast<float>(mod);
        });
    };
    return spec;
}

// ---- rotation speed controller ----------------------------------------------------------------------------

KineticSpec speedController() {
    auto spec      = axial({kAxis, kRevision}, "shaft");
    spec.propagate = [](NodeView const& from, NodeView const& to, ck::Offset, bool, bool) {
        return cogOnController(to, from) ? 1.f : 0.f;
    };
    spec.conveyedSpeed = [](NodeView const& self, NodeView const& other, bool selfIsFrom) -> std::optional<float> {
        if (!cogOnController(other, self)) return std::nullopt;
        bool wheelIsSource = self.source && *self.source == other.pos;
        return ct::controllerConveyedSpeed(self.speed, other.speed, targetSpeed(self.pos), self.hasSource(), wheelIsSource, !selfIsFrom);
    };
    return spec;
}

} // namespace

LV_REGISTER_KINETIC_BLOCK(kShaft, axial({kAxis}, "shaft"));
LV_REGISTER_KINETIC_BLOCK(kCogwheel, cogwheel(false));
LV_REGISTER_KINETIC_BLOCK(kLargeCogwheel, cogwheel(true));
LV_REGISTER_KINETIC_BLOCK(kAndesiteShaft, axial({kAxis}, "shaft"));
LV_REGISTER_KINETIC_BLOCK(kBrassShaft, axial({kAxis}, "shaft"));
LV_REGISTER_KINETIC_BLOCK(kAndesiteCog, encasedCog(false));
LV_REGISTER_KINETIC_BLOCK(kBrassCog, encasedCog(false));
LV_REGISTER_KINETIC_BLOCK(kAndesiteLargeCog, encasedCog(true));
LV_REGISTER_KINETIC_BLOCK(kBrassLargeCog, encasedCog(true));
LV_REGISTER_KINETIC_BLOCK(kGearbox, gearbox());
LV_REGISTER_KINETIC_BLOCK(kClutch, splitShaft(false));
LV_REGISTER_KINETIC_BLOCK(kGearshift, splitShaft(true));
LV_REGISTER_KINETIC_BLOCK(kChainDrive, chainDrive(false));
LV_REGISTER_KINETIC_BLOCK(kChainGearshift, chainDrive(true));
LV_REGISTER_KINETIC_BLOCK(kSequencedGearshift, sequencedGearshift());
LV_REGISTER_KINETIC_BLOCK(kSpeedController, speedController());
LV_REGISTER_KINETIC_BLOCK(kSpeedometer, gauge());
LV_REGISTER_KINETIC_BLOCK(kStressometer, gauge());

std::vector<std::string> const& allBlocks() {
    static std::vector<std::string> const ids{
        kShaft,      kCogwheel,      kLargeCogwheel,      kAndesiteShaft, kBrassShaft,      kAndesiteCog,
        kBrassCog,   kAndesiteLargeCog, kBrassLargeCog,   kGearbox,       kClutch,          kGearshift,
        kChainDrive, kChainGearshift, kSequencedGearshift, kSpeedController, kSpeedometer,  kStressometer,
    };
    return ids;
}

bool isSmallCogBlock(std::string_view id) { return id == kCogwheel || id == kAndesiteCog || id == kBrassCog; }
bool isLargeCogBlock(std::string_view id) { return id == kLargeCogwheel || id == kAndesiteLargeCog || id == kBrassLargeCog; }
bool isEncasedCog(std::string_view id) {
    return id == kAndesiteCog || id == kBrassCog || id == kAndesiteLargeCog || id == kBrassLargeCog;
}
bool isEncasedShaft(std::string_view id) { return id == kAndesiteShaft || id == kBrassShaft; }
bool isChainDrive(std::string_view id) { return id == kChainDrive || id == kChainGearshift; }
bool isGauge(std::string_view id) { return id == kSpeedometer || id == kStressometer; }
bool readsRedstone(std::string_view id) {
    return id == kClutch || id == kGearshift || id == kChainGearshift || id == kSequencedGearshift;
}
bool hasComparatorOutput(std::string_view id) { return isGauge(id) || id == kSequencedGearshift; }

} // namespace lv::feature::transmission
