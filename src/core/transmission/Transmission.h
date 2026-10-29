#pragma once

// Pure rules of the transmission blocks (U6, SPEC §2.1 / §3.6.1): chain drive shapes and ratios,
// gauge dials, sequenced gearshift instructions, the rotation speed controller's conveyed speed
// and Create's placement helper orderings. Behaviour reference: Create 6.0.10 (ChainDriveBlock,
// ChainGearshiftBlockEntity, GaugeBlock, SpeedGaugeBlockEntity, StressGaugeBlockEntity,
// Instruction, SequencerInstructions, SpeedControllerBlockEntity, IPlacementHelper).

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

#include "core/kinetics/Types.h"

namespace lv::core::transmission {

using kinetics::Axis;
using kinetics::Face;
using kinetics::Offset;

// ---- chain drives ------------------------------------------------------------------------------

/// Create's ChainDriveBlock.Part (state values: none 0, start 1, middle 2, end 3).
enum class ChainPart : int { None = 0, Start = 1, Middle = 2, End = 3 };

struct ChainState {
    Axis      axis       = Axis::Y;
    ChainPart part       = ChainPart::None;
    bool      alongFirst = false; ///< CONNECTED_ALONG_FIRST_COORDINATE
    friend bool operator==(ChainState const&, ChainState const&) = default;
};

/// The axis along which the chain runs (Create's getConnectionAxis).
[[nodiscard]] Axis chainConnectionAxis(ChainState const& s);
/// Create's ChainDriveBlock.updateShape for the neighbour on `face` (`neighbour` nullopt: not a
/// chain drive).
[[nodiscard]] ChainState chainUpdateShape(ChainState self, Face face, std::optional<ChainState> const& neighbour);
/// Create's areBlocksConnected: `s` passes rotation to `other` in direction `facing`.
[[nodiscard]] bool chainConnected(ChainState const& s, ChainState const& other, Face facing);
/// Create's ChainGearshiftBlockEntity.getModifierForSignal.
[[nodiscard]] float chainGearshiftModifier(int signal);

// ---- gauges --------------------------------------------------------------------------------------

/// Create's DirectionalAxisKineticBlock.getRotationAxis (gauges: FACING + AXIS_ALONG_FIRST_COORDINATE).
[[nodiscard]] Axis gaugeRotationAxis(Face facing, bool alongFirst);
/// Create's GaugeBlock.shouldRenderHeadOnFace (without the occlusion test).
[[nodiscard]] bool gaugeHeadOnFace(Face facing, bool alongFirst, Face face);
/// Create's SpeedGaugeBlockEntity.getDialTarget.
[[nodiscard]] float speedDialTarget(float speed, float mediumSpeed, float fastSpeed, float maxSpeed);
/// Create's StressGaugeBlockEntity dial: 0 without rotation, 1.125 overstressed, else stress / capacity.
[[nodiscard]] float stressDialTarget(bool stressEnabled, float speed, bool overstressed, double stress, double capacity);
/// Create's GaugeBlock.getAnalogOutputSignal: ceil(clamp(target * 14, 0, 15)).
[[nodiscard]] int gaugeComparatorSignal(float dialTarget);
/// The static needle position for vanilla clients (`levitate:dial`: the needle angle, 90° per 1.0
/// of the dial, in steps of 22.5° as Bedrock block models allow).
[[nodiscard]] int dialStateOf(float dialTarget);
inline constexpr int kDialStates = 6;
/// The dial value a `levitate:dial` state shows.
[[nodiscard]] float dialOfState(int state);
/// Needle angle in degrees for a dial value (Create: a quarter turn per 1.0).
[[nodiscard]] inline float dialAngle(float dial) { return 90.f * dial; }
/// Create's GaugeBlockEntity.tick: the needle moves 1/8 of the way to its target per tick.
[[nodiscard]] inline float dialStep(float state, float target) { return state + (target - state) * .125f; }

// ---- sequenced gearshift -------------------------------------------------------------------------

enum class Instr : int { TurnAngle = 0, TurnDistance = 1, Delay = 2, Await = 3, End = 4 };
inline constexpr int kInstrTypes = 5;
inline constexpr int kMaxInstructions = 5;

struct InstrDef {
    char const* id;          ///< "turn_angle" (translation key suffix)
    char const* parameter;   ///< "angle" / "distance" / "duration" / ""
    bool        hasValue;
    bool        hasSpeed;
    int         maxValue;
    int         shiftStep;
    int         defaultValue;
};
[[nodiscard]] InstrDef const& instrDef(Instr i);

/// Create's InstructionSpeedModifiers (FORWARD_FAST 2, FORWARD 1, BACK -1, BACK_FAST -2), in that order.
inline constexpr std::array<int, 4> kSpeedModifiers{2, 1, -1, -2};
[[nodiscard]] char const* speedModifierId(int modifier); ///< "forward_fast", ...

struct Instruction {
    Instr type          = Instr::TurnAngle;
    int   speedModifier = 1;
    int   value         = 90;
    friend bool operator==(Instruction const&, Instruction const&) = default;
};

/// Create's Instruction.createDefault: [turn 90° forward, end].
[[nodiscard]] std::vector<Instruction> defaultInstructions();
/// Create's Instruction.getDuration (ticks; -1 = wait for a pulse).
[[nodiscard]] int instructionDuration(Instruction const& i, float progress, float speed);
/// Create's Instruction.getTickProgress.
[[nodiscard]] float instructionTickProgress(Instruction const& i, float speed);
/// Create's Instruction.getSpeedModifier (0 for timing instructions).
[[nodiscard]] int instructionSpeedModifier(Instruction const& i);
/// Create's SequencerInstructions.formatValue ("90°", "5m", "1s" / "10t").
[[nodiscard]] std::string formatInstructionValue(Instr type, int value, std::string const& degreesUnit = "°");
/// `levitate:mod` state values (0, 1, 2, -1, -2 stored as 0..4).
[[nodiscard]] int modifierToState(int modifier);
[[nodiscard]] int stateToModifier(int state);
/// JSON (BlockData) of an instruction list; invalid entries are clamped, an empty list gives the default.
[[nodiscard]] nlohmann::json instructionsToJson(std::vector<Instruction> const& list);
[[nodiscard]] std::vector<Instruction> instructionsFromJson(nlohmann::json const& j);
/// The list after the screen changed row `index` to `type` (Create's SequencedGearshiftScreen
/// .instructionUpdated: the value resets, END truncates, a last non-END row gets an END after it).
void setInstructionType(std::vector<Instruction>& list, int index, Instr type);

// ---- rotation speed controller -------------------------------------------------------------------

/// Create's SpeedControllerBlockEntity.getConveyedSpeed. `controllerSpeed` / `wheelSpeed` are the
/// theoretical speeds, `targetingController` is true when the cog drives the controller.
[[nodiscard]] float controllerConveyedSpeed(
    float controllerSpeed,
    float wheelSpeed,
    int   targetSpeed,
    bool  controllerHasSource,
    bool  wheelIsControllerSource,
    bool  targetingController
);

// ---- placement helpers ---------------------------------------------------------------------------

/// Create's IPlacementHelper.orderedByDistance: the faces (passing `filter`) sorted by the distance
/// between their normal and the hit point relative to the block centre.
template <class Pred>
[[nodiscard]] std::vector<Face> orderedByDistance(double hx, double hy, double hz, Pred filter) {
    std::vector<std::pair<double, Face>> v;
    for (Face f : kinetics::kFaces) {
        if (!filter(f)) continue;
        auto   o  = kinetics::offsetOf(f);
        double dx = o.x - hx, dy = o.y - hy, dz = o.z - hz;
        v.emplace_back(dx * dx + dy * dy + dz * dz, f);
    }
    // stable: Create sorts a stream of Direction.values() by distance
    for (std::size_t i = 1; i < v.size(); ++i)
        for (std::size_t j = i; j > 0 && v[j].first < v[j - 1].first; --j) std::swap(v[j], v[j - 1]);
    std::vector<Face> out;
    for (auto const& [d, f] : v) out.push_back(f);
    return out;
}

} // namespace lv::core::transmission
