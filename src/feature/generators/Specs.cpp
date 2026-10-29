// Generators (U7): kinetic specs of the unit's blocks and their value boxes.
//
// Every block is a generator (Create's GeneratingKineticBlockEntity) with Create's stress
// capacity from config/kinetics.json (hand crank / valve handles 8, water wheel 32, large water
// wheel 128, creative motor 16384 SU per RPM). Client models (client mod, kinetics renderer):
// rotor bones are modelled around +Y; blocks with a facing pick the bone on the facing's side.

#include "feature/generators/Generators.h"

#include <cmath>

#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"

namespace lv::feature::generators {

namespace k  = core::kinetics;
namespace vb = common::valuebox;
using kinetics::KineticSpec;

namespace {

Face faceState(k::BlockState const& s) {
    int v = s.get(kFacing, 1);
    return static_cast<Face>(v >= 0 && v <= 5 ? v : 1);
}

/// Hand crank / valve handle: the shaft end is behind the handle (Create: face == FACING.getOpposite()).
/// The kinetics renderer draws the short shaft (`shaft_pos` / `shaft_neg`); the handle itself
/// follows Create's independent handle angle (client/generators).
KineticSpec handleSpec(std::string texture) {
    KineticSpec s;
    s.states          = {kFacing};
    s.rotationAxis    = kinetics::specs::axisFromFacing(kFacing);
    s.hasShaftTowards = [](k::BlockState const& st, Face f) { return f == k::opposite(faceState(st)); };
    s.generator       = true;
    s.capacity        = 8;
    s.rotorState      = kRotor;
    s.rotorModel      = "geometry.levitate.hand_crank.rotor";
    s.rotorTexture    = std::move(texture);
    s.parts           = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        Face f = faceState(*v.state);
        out.push_back(k::RotorPart{k::isPositive(f) ? "shaft_pos" : "shaft_neg", k::axisOf(f), 1.f, 0.f, false});
    };
    return s;
}

/// Water wheel (DirectionalKineticBlock): shafts on both ends of the facing's axis; the whole
/// wheel turns. Rotor bones per wood type (`wheel_<wood>`) on one stacked texture.
KineticSpec waterWheelSpec() {
    KineticSpec s;
    s.states          = {kFacing, kWood};
    s.rotationAxis    = kinetics::specs::axisFromFacing(kFacing);
    s.hasShaftTowards = [](k::BlockState const& st, Face f) { return k::axisOf(f) == k::axisOf(faceState(st)); };
    s.generator       = true;
    s.capacity        = 32;
    s.rotorState      = kRotor;
    s.rotorModel      = "geometry.levitate.water_wheel.rotor";
    s.rotorTexture    = "levitate_water_wheel_rotor";
    s.parts           = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        int wood = std::clamp(v.state->get(kWood, cg::kDefaultWood), 0, static_cast<int>(cg::kWoods.size()) - 1);
        out.push_back(k::RotorPart{"wheel_" + std::to_string(wood), k::axisOf(faceState(*v.state)), 1.f, 0.f, false});
    };
    return s;
}

/// Large water wheel (RotatedPillarKineticBlock): shafts on both ends of its axis.
KineticSpec largeWheelSpec() {
    KineticSpec s;
    s.states       = {kAxisState, kWood};
    s.rotationAxis = kinetics::specs::axisFromState(kAxisState);
    s.hasShaftTowards = [](k::BlockState const& st, Face f) {
        return static_cast<int>(k::axisOf(f)) == std::clamp(st.get(kAxisState, 1), 0, 2);
    };
    s.generator    = true;
    s.capacity     = 128;
    s.rotorState   = kRotor;
    s.rotorModel   = "geometry.levitate.large_water_wheel.rotor";
    s.rotorTexture = "levitate_water_wheel_rotor";
    s.parts        = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        int wood = std::clamp(v.state->get(kWood, cg::kDefaultWood), 0, static_cast<int>(cg::kWoods.size()) - 1);
        auto axis = static_cast<Axis>(std::clamp(v.state->get(kAxisState, 1), 0, 2));
        out.push_back(k::RotorPart{"wheel_" + std::to_string(wood), axis, 1.f, 0.f, false});
    };
    return s;
}

/// Creative motor (DirectionalKineticBlock): one shaft end at its facing.
KineticSpec motorSpec() {
    KineticSpec s;
    s.states                = {kFacing};
    s.rotationAxis          = kinetics::specs::axisFromFacing(kFacing);
    s.hasShaftTowards       = [](k::BlockState const& st, Face f) { return f == faceState(st); };
    s.generator             = true;
    s.capacity              = 16384;
    s.initialGeneratedSpeed = [](k::BlockState const& st) {
        return cg::convertToDirection(static_cast<float>(cg::kMotorDefaultSpeed), faceState(st));
    };
    s.rotorState   = kRotor;
    s.rotorModel   = "geometry.levitate.creative_motor.rotor";
    s.rotorTexture = "levitate_creative_motor";
    s.parts        = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        Face f = faceState(*v.state);
        out.push_back(k::RotorPart{k::isPositive(f) ? "shaft_pos" : "shaft_neg", k::axisOf(f), 1.f, 0.f, false});
    };
    return s;
}

struct Registrar {
    Registrar() {
        kinetics::registerBlock(kHandCrank, handleSpec("levitate_hand_crank"));
        kinetics::registerBlock(kCopperValve, handleSpec("levitate_copper_valve_handle"));
        for (int c = 0; c < 16; ++c) {
            kinetics::registerBlock(valveId(c), handleSpec("levitate_" + std::string(cg::kDyeColours[static_cast<std::size_t>(c)]) + "_valve_handle"));
        }
        kinetics::registerBlock(kWaterWheel, waterWheelSpec());
        kinetics::registerBlock(kLargeWheel, largeWheelSpec());
        kinetics::registerBlock(kMotor, motorSpec());
    }
};
Registrar const registrar;

// ---- value boxes -------------------------------------------------------------------------------------------------

/// Create's MotorValueBox (ValueBoxTransform.Sided): on the four sides around the shaft (not the
/// bottom of a horizontal motor), 3.5 px inside the face, one pixel back from the shaft.
std::optional<core::common::Vec> motorBox(Block const& block, int face) {
    if (face < 0 || face > 5) return std::nullopt;
    Face facing = facingOf(block);
    auto side   = static_cast<Face>(face);
    if (k::axisOf(side) == k::axisOf(facing)) return std::nullopt;
    if (k::axisOf(facing) != Axis::Y && side == Face::Down) return std::nullopt;
    auto p = core::common::faceLocalPoint(face, 8, 8, -3.5);
    auto n = k::offsetOf(facing);
    p[0] -= n.x / 16.0;
    p[1] -= n.y / 16.0;
    p[2] -= n.z / 16.0;
    return p;
}

/// Create's ValveHandleValueBox: on the handle's front, 4.5 px out from the mounting face.
std::optional<core::common::Vec> valveBox(Block const& block, int face) {
    if (detail::suppressValveBox) return std::nullopt;
    if (face < 0 || face > 5 || static_cast<Face>(face) != facingOf(block)) return std::nullopt;
    return core::common::faceLocalPoint(face, 8, 8, 4.5 - 16);
}

std::string rpmText(core::common::ValueSettings s) {
    // KineticScrollValueBehaviour.formatSettings: the number and the direction arrow
    return std::to_string(std::max(1, std::abs(s.value))) + (s.row == 0 ? "⟳" : "⟲");
}

} // namespace

namespace detail {

void registerValueBoxes() {
    vb::BoxSpec motor;
    motor.id            = "speed";
    motor.label         = "create.kinetics.creative_motor.rotation_speed";
    motor.labelFallback = "Generated Speed in RPM";
    motor.board         = [](Block const&) { return core::common::signedBoard(cg::kMotorMaxSpeed, cg::kMotorMilestone, "⟳", "⟲"); };
    motor.centre        = motorBox;
    motor.get           = [](GlobalPos const& p) { return core::common::signedToSettings(motorSpeed(p)); };
    motor.set           = [](GlobalPos const& p, core::common::ValueSettings s, Player&) {
        setMotorSpeed(p, core::common::signedFromSettings(s, cg::kMotorMaxSpeed));
    };
    motor.format = [](Block const&, core::common::ValueSettings s) { return rpmText(s); };
    vb::registerBox(kMotor, motor);

    vb::BoxSpec valve;
    valve.id            = "angle";
    valve.label         = "create.kinetics.valve_handle.rotated_angle";
    valve.labelFallback = "Rotation when used";
    valve.board         = [](Block const&) { return core::common::signedBoard(cg::kValveMaxAngle, cg::kValveMilestone, "⟳", "⟲"); };
    valve.centre        = valveBox;
    valve.get           = [](GlobalPos const& p) { return core::common::signedToSettings(valveAngle(p)); };
    valve.set           = [](GlobalPos const& p, core::common::ValueSettings s, Player&) {
        setValveAngle(p, core::common::signedFromSettings(s, cg::kValveMaxAngle));
    };
    valve.format = [](Block const&, core::common::ValueSettings s) {
        return std::to_string(std::max(1, std::abs(s.value))) + "°";
    };
    vb::registerBox(kCopperValve, valve);
    for (int c = 0; c < 16; ++c) vb::registerBox(valveId(c), valve);
}

} // namespace detail

} // namespace lv::feature::generators
