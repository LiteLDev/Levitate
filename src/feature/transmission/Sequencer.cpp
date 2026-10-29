// Sequenced gearshift (U6): Create's SequencedGearshiftBlockEntity. Up to 5 instructions (turn by
// angle, turn for a piston distance, delay, await a redstone pulse, end) run one after another
// when the block gets powered. The current instruction is the block state `levitate:state`
// (index + 1, also the comparator output) and its speed modifier `levitate:mod`, so a step
// re-propagates the network as Create's detachKinetics does. The rest lives in BlockData.

#include <algorithm>
#include <unordered_map>

#include "ll/api/form/CustomForm.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"

#include "feature/common/BlockData.h"
#include "feature/common/Screens.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace blockdata = feature::common::blockdata;
namespace screens   = feature::common::screens;
using Json          = nlohmann::json;

namespace {

constexpr char kKey[]        = "sequencer";
constexpr char kScreenType[] = "transmission.sequenced_gearshift";

struct Run {
    std::vector<ct::Instruction> instructions = ct::defaultInstructions();
    int   current  = -1;
    int   duration = -1;
    float progress = 0;
    int   timer    = 0;
    bool  poweredPreviously = false;
};

Run load(GlobalPos const& pos) {
    Run r;
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains(kKey)) return r;
    auto const& j      = (*d)[kKey];
    r.instructions     = ct::instructionsFromJson(j.value("instructions", Json::array()));
    r.current          = j.value("current", -1);
    r.duration         = j.value("duration", -1);
    r.progress         = j.value("progress", 0.f);
    r.timer            = j.value("timer", 0);
    r.poweredPreviously = j.value("powered", false);
    return r;
}

void store(GlobalPos const& pos, Run const& r) {
    blockdata::edit(pos)[kKey] = Json{
        {"instructions", ct::instructionsToJson(r.instructions)},
        {"current",      r.current                            },
        {"duration",     r.duration                           },
        {"progress",     r.progress                           },
        {"timer",        r.timer                              },
        {"powered",      r.poweredPreviously                  },
    };
}

/// Last seen redstone power per position (Create reacts to neighbour updates, i.e. changes).
std::unordered_map<GlobalPos, bool, ck::GlobalPosHash>& lastPower() {
    static std::unordered_map<GlobalPos, bool, ck::GlobalPosHash> m;
    return m;
}

ct::Instruction const* instructionAt(Run const& r, int index) {
    return index >= 0 && index < static_cast<int>(r.instructions.size()) ? &r.instructions[static_cast<std::size_t>(index)] : nullptr;
}

/// Create's SequencedGearshiftBlockEntity.run.
void run(BlockSource& region, GlobalPos const& pos, Run& r, int index, bool powered) {
    auto const* instr = instructionAt(r, index);
    if (!instr || instr->type == ct::Instr::End) {
        r.current  = -1;
        r.duration = -1;
        r.progress = 0;
        r.timer    = 0;
        store(pos, r);
        if (!powered) setStates(region, pos, {{kSeqState, 0}, {kSeqMod, 0}});
        else setStates(region, pos, {{kSeqMod, 0}}); // idle: the output half is disconnected
        log().info("transmission: sequenced gearshift at {} finished", pos.key());
        return;
    }
    r.duration = ct::instructionDuration(*instr, 0, kinetics::theoreticalSpeedAt(pos));
    r.current  = index;
    r.progress = 0;
    r.timer    = 0;
    store(pos, r);
    setStates(region, pos, {{kSeqState, index + 1}, {kSeqMod, ct::modifierToState(ct::instructionSpeedModifier(*instr))}});
    log().info("transmission: sequenced gearshift at {} runs instruction {} ({})", pos.key(), index + 1, ct::instrDef(instr->type).id);
}

/// Create's onRedstoneUpdate (+ risingFlank).
void onRedstoneUpdate(BlockSource& region, GlobalPos const& pos, bool isPowered) {
    Run  r         = load(pos);
    bool isRunning = stateAt(region, pos, kSeqState, 0) != 0;
    if (!r.poweredPreviously && isPowered) {
        // risingFlank: an Await instruction continues
        if (auto const* instr = instructionAt(r, r.current); instr && instr->type == ct::Instr::Await) {
            r.poweredPreviously = true;
            store(pos, r);
            run(region, pos, r, r.current + 1, isPowered);
            r = load(pos);
        }
    }
    r.poweredPreviously = isPowered;
    store(pos, r);
    if (r.current != -1) return;
    if (isPowered == isRunning) return;
    if (!isPowered) {
        setStates(region, pos, {{kSeqState, 0}, {kSeqMod, 0}});
        return;
    }
    if (kinetics::speedAt(pos) == 0) return;
    run(region, pos, r, 0, isPowered);
}

} // namespace

void sequencerRedstone(BlockSource& region, GlobalPos const& pos, bool powered) {
    auto [it, fresh] = lastPower().try_emplace(pos, powered);
    if (fresh) {
        // first sight (placed or loaded): remember the power as the block entity would
        Run r = load(pos);
        if (!blockdata::get(pos) || !blockdata::get(pos)->contains(kKey)) store(pos, r);
        return;
    }
    if (it->second == powered) return;
    it->second = powered;
    onRedstoneUpdate(region, pos, powered);
}

void sequencerTick(BlockSource& region, GlobalPos const& pos) {
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains(kKey) || (*d)[kKey].value("current", -1) == -1) return;
    Run r = load(pos);
    if (r.duration < 0) return;
    if (r.timer < r.duration) {
        ++r.timer;
        if (auto const* instr = instructionAt(r, r.current)) r.progress += ct::instructionTickProgress(*instr, kinetics::theoreticalSpeedAt(pos));
        store(pos, r);
        return;
    }
    bool powered = lastPower().contains(pos) ? lastPower()[pos] : false;
    run(region, pos, r, r.current + 1, powered);
}

void sequencerSpeedChanged(GlobalPos const& pos, float previous, float now) {
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains(kKey)) return;
    Run r = load(pos);
    if (r.current == -1) return;
    if (std::abs(previous) == std::abs(now)) return;
    auto const* instr = instructionAt(r, r.current);
    if (!instr) return;
    ct::Instruction copy = *instr;
    if (kinetics::speedAt(pos) == 0) {
        if (auto* region = regionOf(pos.dim)) {
            bool powered = lastPower().contains(pos) ? lastPower()[pos] : false;
            run(*region, pos, r, -1, powered);
            r = load(pos);
        }
    }
    // update the instruction time for the new speed
    r.duration = ct::instructionDuration(copy, r.progress, kinetics::theoreticalSpeedAt(pos));
    r.timer    = 0;
    store(pos, r);
}

std::vector<ct::Instruction> sequencerInstructions(GlobalPos const& pos) { return load(pos).instructions; }

int sequencerCurrent(GlobalPos const& pos) { return load(pos).current; }

void setSequencerInstructions(GlobalPos const& pos, std::vector<ct::Instruction> list) {
    // Create's ConfigureSequencedGearshiftPacket: stop, then take the new list
    auto* region = regionOf(pos.dim);
    if (!region || idAt(*region, pos) != kSequencedGearshift) return;
    Run  r       = load(pos);
    bool powered = lastPower().contains(pos) ? lastPower()[pos] : false;
    run(*region, pos, r, -1, powered);
    r = load(pos);
    if (list.empty()) list = ct::defaultInstructions();
    if (list.size() > static_cast<std::size_t>(ct::kMaxInstructions)) list.resize(ct::kMaxInstructions);
    r.instructions = ct::instructionsFromJson(ct::instructionsToJson(list));
    store(pos, r);
    log().info("transmission: sequenced gearshift at {} configured ({} instructions)", pos.key(), r.instructions.size());
}

void clearSequencers() { lastPower().clear(); }

// ---- screen ---------------------------------------------------------------------------------------------

namespace {

std::optional<GlobalPos> posOf(Player& player, Json const& ctx) {
    auto p = ctx.value("pos", Json::array());
    if (!p.is_array() || p.size() != 4) return std::nullopt;
    GlobalPos pos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()};
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return std::nullopt;
    auto* region = regionOf(pos.dim);
    if (!region || idAt(*region, pos) != kSequencedGearshift) return std::nullopt;
    auto  eye = player.getPosition();
    double dx = pos.x + .5 - eye.x, dy = pos.y + .5 - eye.y, dz = pos.z + .5 - eye.z;
    if (dx * dx + dy * dy + dz * dz > 8.0 * 8.0) return std::nullopt; // Create: the screen closes out of reach
    return pos;
}

std::string instrName(Player& p, ct::Instr t) {
    auto const& def = ct::instrDef(t);
    static constexpr char const* kDescriptive[] = {"Turn by angle", "Turn to move Piston/Pulley/Gantry", "Timed Delay", "Await new Redstone Pulse", "End"};
    return trFor(p, std::string("create.gui.sequenced_gearshift.instruction.") + def.id + ".descriptive", kDescriptive[static_cast<int>(t)]);
}

std::string speedName(Player& p, int modifier) {
    static constexpr std::pair<int, char const*> kNames[] = {
        {2,  "Double speed, Forwards"},
        {1,  "Input speed, Forwards" },
        {-1, "Input speed, Reversed" },
        {-2, "Double speed, Reversed"},
    };
    for (auto const& [m, en] : kNames)
        if (m == modifier) return trFor(p, std::string("create.gui.sequenced_gearshift.speed.") + ct::speedModifierId(m), en);
    return {};
}

void openForm(Player& player, Json const& ctx) {
    auto pos = posOf(player, ctx);
    if (!pos) return;
    auto list = sequencerInstructions(*pos);
    ll::form::CustomForm form(trFor(player, "create.gui.sequenced_gearshift.title", "Sequenced Gearshift"));
    std::vector<std::string> types, speeds;
    for (int t = 0; t < ct::kInstrTypes; ++t) types.push_back(instrName(player, static_cast<ct::Instr>(t)));
    for (int m : ct::kSpeedModifiers) speeds.push_back(speedName(player, m));
    for (int i = 0; i < ct::kMaxInstructions; ++i) {
        ct::Instruction cur = i < static_cast<int>(list.size()) ? list[static_cast<std::size_t>(i)] : ct::Instruction{ct::Instr::End, 1, 1};
        auto            row = std::to_string(i + 1);
        form.appendLabel("§l" + trFor(player, "create.gui.sequenced_gearshift.instruction", "Instruction") + " " + row);
        form.appendDropdown("type" + row, trFor(player, "create.gui.sequenced_gearshift.instruction", "Instruction"), types, static_cast<std::size_t>(cur.type));
        form.appendSlider(
            "value" + row,
            trFor(player, "lv.transmission.sequencer.form.value", "Value (Turn: degrees, Piston: meters, Delay: ticks)"),
            1,
            600,
            1,
            std::clamp(cur.value, 1, 600)
        );
        auto sp = std::find(ct::kSpeedModifiers.begin(), ct::kSpeedModifiers.end(), cur.speedModifier) - ct::kSpeedModifiers.begin();
        form.appendDropdown("speed" + row, trFor(player, "create.gui.sequenced_gearshift.speed", "Speed, Direction"), speeds, static_cast<std::size_t>(sp));
    }
    form.sendTo(player, [ctx](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto pos = posOf(p, ctx);
        if (!pos) return;
        auto num = [&](std::string const& key) -> double {
            auto it = result->find(key);
            if (it == result->end()) return 0;
            if (auto const* v = std::get_if<uint64>(&it->second)) return static_cast<double>(*v);
            if (auto const* d = std::get_if<double>(&it->second)) return *d;
            return 0;
        };
        std::vector<ct::Instruction> list;
        for (int i = 0; i < ct::kMaxInstructions; ++i) {
            auto            row = std::to_string(i + 1);
            ct::Instruction in;
            in.type          = static_cast<ct::Instr>(std::clamp(static_cast<int>(num("type" + row)), 0, ct::kInstrTypes - 1));
            in.speedModifier = ct::kSpeedModifiers[static_cast<std::size_t>(std::clamp(static_cast<int>(num("speed" + row)), 0, 3))];
            auto const& def  = ct::instrDef(in.type);
            in.value         = def.hasValue ? std::clamp(static_cast<int>(std::lround(num("value" + row))), 1, def.maxValue) : 1;
            list.push_back(in);
            if (in.type == ct::Instr::End) break;
        }
        setSequencerInstructions(*pos, list);
    });
}

} // namespace

bool openSequencerScreen(Player& player, GlobalPos const& pos) {
    Json ctx{{"pos", {pos.dim, pos.x, pos.y, pos.z}}};
    return screens::open(player, kScreenType, ctx);
}

void registerSequencerScreen() {
    screens::registerType({
        .id   = kScreenType,
        .data = [](Player& player, Json const& ctx) -> std::optional<Json> {
            auto pos = posOf(player, ctx);
            if (!pos) return std::nullopt;
            return Json{{"instructions", ct::instructionsToJson(sequencerInstructions(*pos))}, {"current", sequencerCurrent(*pos)}};
        },
        .action = [](Player& player, Json const& ctx, Json const& action) {
            auto pos = posOf(player, ctx);
            if (!pos || action.value("op", std::string{}) != "set") return;
            setSequencerInstructions(*pos, ct::instructionsFromJson(action.value("instructions", Json::array())));
        },
        .form = openForm,
    });
}

} // namespace lv::feature::transmission
