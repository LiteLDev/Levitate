#ifdef LV_CLIENT

// Native screen of the sequenced gearshift (U6, SPEC §4): Create's SequencedGearshiftScreen. Five
// rows; each row picks the instruction, its value (scroll; Shift scrolls by the instruction's
// step) and the speed modifier. Choosing End removes the rows below it, a non-End last row gets a
// new End row (up to five). The instructions are sent to the server when the screen closes, as
// Create's ConfigureSequencedGearshiftPacket.

#include <algorithm>

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/transmission/Transmission.h"
#include "mod/Module.h"

namespace lv::client::screen {

namespace {

namespace ct = core::transmission;

/// Create's ScrollInput with the instruction's own Shift step (45° / 5 m / 20 ticks).
class StepInput : public ScrollInput {
public:
    using ScrollInput::ScrollInput;
    int  shiftStep = 10;
    bool scroll(float mx, float my, int delta, bool shift) override {
        if (!enabled || !hovered(mx, my) || delta == 0) return false;
        int step = shift ? shiftStep : 1;
        int next = std::clamp(value + (delta > 0 ? step : -step), min, max);
        if (next != value) {
            value = next;
            if (onChange) onChange(value);
        }
        return true;
    }
};

class SequencerScreen : public Screen {
public:
    SequencerScreen(Json const& data, SendFn send)
    : Screen(ctr("create.gui.sequenced_gearshift.title", "Sequenced Gearshift"), 230, 130),
      mSend(std::move(send)) {
        mList = ct::instructionsFromJson(data.value("instructions", Json::array()));
    }

    void init() override {
        static constexpr char const* kDescriptive[] = {"Turn by angle", "Turn to move Piston/Pulley/Gantry", "Timed Delay",
                                                       "Await new Redstone Pulse", "End"};
        for (int t = 0; t < ct::kInstrTypes; ++t) {
            auto id = std::string("create.gui.sequenced_gearshift.instruction.") + ct::instrDef(static_cast<ct::Instr>(t)).id;
            mTypeNames.push_back(ctr(id, kDescriptive[t]));
        }
        static constexpr char const* kSpeeds[] = {"Double speed, Forwards", "Input speed, Forwards", "Input speed, Reversed", "Double speed, Reversed"};
        for (std::size_t i = 0; i < ct::kSpeedModifiers.size(); ++i) {
            mSpeedNames.push_back(ctr(std::string("create.gui.sequenced_gearshift.speed.") + ct::speedModifierId(ct::kSpeedModifiers[i]), kSpeeds[i]));
        }
        for (int row = 0; row < ct::kMaxInstructions; ++row) {
            float y   = 20.f + row * 20.f;
            auto& num = add<Label>(Rect{6, y + 4, 10, 10}, std::to_string(row + 1), color::kTextDark);
            auto& type = add<ScrollInput>(Rect{18, y, 92, 16}, "", 0, ct::kInstrTypes - 1, 0, [this, row](int v) {
                ct::setInstructionType(mList, row, static_cast<ct::Instr>(v));
                refresh();
            });
            type.options = mTypeNames;
            auto& value  = add<StepInput>(Rect{114, y, 50, 16}, "", 1, 360, 90, [this, row](int v) {
                if (row < static_cast<int>(mList.size())) mList[static_cast<std::size_t>(row)].value = v;
            });
            value.format = [this, row](int v) {
                if (row >= static_cast<int>(mList.size())) return std::string{};
                return ct::formatInstructionValue(mList[static_cast<std::size_t>(row)].type, v);
            };
            auto& speed = add<ScrollInput>(Rect{168, y, 56, 16}, "", 0, 3, 1, [this, row](int v) {
                if (row < static_cast<int>(mList.size())) mList[static_cast<std::size_t>(row)].speedModifier = ct::kSpeedModifiers[static_cast<std::size_t>(v)];
            });
            speed.format = [](int v) {
                static constexpr char const* kArrows[] = {">>", "->", "<-", "<<"};
                return std::string(kArrows[std::clamp(v, 0, 3)]);
            };
            mRows.push_back(Row{&num, &type, &value, &speed});
        }
        auto& done = add<IconButton>(Rect{206, 110, 18, 18}, "v", [] { close(); });
        done.tooltip = {ctr("create.gui.sequenced_gearshift.title", "Sequenced Gearshift")};
        refresh();
    }

    void onData(Json const& data) override {
        mList = ct::instructionsFromJson(data.value("instructions", Json::array()));
        refresh();
    }

    void onClose() override { mSend({{"op", "set"}, {"instructions", ct::instructionsToJson(mList)}}); }

private:
    struct Row {
        Label*       number;
        ScrollInput* type;
        StepInput*   value;
        ScrollInput* speed;
    };

    void refresh() {
        for (int row = 0; row < static_cast<int>(mRows.size()); ++row) {
            auto& r     = mRows[static_cast<std::size_t>(row)];
            bool  shown = row < static_cast<int>(mList.size());
            r.number->visible = r.type->visible = shown;
            r.value->visible = r.speed->visible = false;
            if (!shown) continue;
            auto const& in  = mList[static_cast<std::size_t>(row)];
            auto const& def = ct::instrDef(in.type);
            r.type->value   = static_cast<int>(in.type);
            r.type->tooltip = {ctr("create.gui.sequenced_gearshift.instruction", "Instruction"), mTypeNames[static_cast<std::size_t>(in.type)]};
            if (def.hasValue) {
                r.value->visible = true;
                r.value->min     = 1;
                r.value->max     = def.maxValue;
                r.value->value   = std::clamp(in.value, 1, def.maxValue);
                r.value->shiftStep = def.shiftStep;
                r.value->tooltip = {ctr(std::string("create.gui.sequenced_gearshift.instruction.") + def.id + "." + def.parameter, def.parameter)};
            }
            if (def.hasSpeed) {
                r.speed->visible = true;
                auto idx = std::find(ct::kSpeedModifiers.begin(), ct::kSpeedModifiers.end(), in.speedModifier) - ct::kSpeedModifiers.begin();
                r.speed->value   = static_cast<int>(idx);
                r.speed->tooltip = {ctr("create.gui.sequenced_gearshift.speed", "Speed, Direction"), mSpeedNames[static_cast<std::size_t>(idx)]};
            }
        }
    }

    SendFn                       mSend;
    std::vector<ct::Instruction> mList;
    std::vector<Row>             mRows;
    std::vector<std::string>     mTypeNames, mSpeedNames;
};

class SequencerScreenModule {
public:
    void install() {
        registerServerScreen("transmission.sequenced_gearshift", [](Json const& data, SendFn send) {
            return std::make_unique<SequencerScreen>(data, std::move(send));
        });
    }
    void uninstall() {}
};

} // namespace

} // namespace lv::client::screen

LV_REGISTER_MODULE(lv::client::screen::SequencerScreenModule);

#endif // LV_CLIENT
