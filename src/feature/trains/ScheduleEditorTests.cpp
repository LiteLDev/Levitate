// Selftests of the native schedule editor's sub-screens (U38, Create's instruction / condition
// editors of ScheduleScreen): the "set_instruction" / "set_condition" actions the client mod sends,
// checked as Create's widgets limit them. No world blocks (dx 3860..3869 reserved).

#include <string>

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "feature/trains/Trains.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
using JsonT     = nlohmann::json;
namespace tn    = lv::feature::trains;
namespace ct    = lv::core::trains;
namespace ids   = lv::core::trains::ids;
namespace items = lv::feature::common::items;
namespace net   = lv::feature::net;

constexpr char kType[] = "trains.schedule";

JsonT act(Player& p, JsonT const& context, JsonT const& action) {
    (void)net::takeTestMessages(p);
    net::injectFromClient(p, "screen.action", {{"type", kType}, {"context", context}, {"action", action}});
    JsonT out;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "screen.update" && m.payload.value("type", std::string{}) == kType) out = m.payload.value("data", JsonT());
    return out;
}

ct::Schedule held(Player& p) { return tn::scheduleOf(p.getSelectedItem()).value_or(ct::Schedule{}); }

} // namespace

LV_REGISTER_SELFTEST("trains.schedule_editor_native", [](Context& ctx) {
    auto& p = ctx.player();
    ct::Schedule s;
    s.entries.push_back(ct::Entry::makeDefault());
    int slot = p.getSelectedItemSlot();
    ctx.require(ctx.give(p, tn::makeScheduleItem(s), slot) == slot, "schedule in hand");
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    tn::openScheduleEditor(p, slot);
    JsonT opened;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "screen.open" && m.payload.value("type", std::string{}) == kType) opened = m.payload.value("data", JsonT());
    ctx.check(opened.value("instructions", JsonT::array()).size() == ct::instructionIds().size(), "the editor gets the instruction types");
    ctx.check(opened.value("conditions", JsonT::array()).size() == ct::conditionIds().size(), "the editor gets the condition types");
    JsonT context{{"slot", slot}};

    // instruction editor: the station name field (25 characters)
    act(p, context, {{"op", "set_instruction"}, {"entry", 0}, {"part", {{"Id", std::string(ids::kDestination)}, {"Data", {{"Text", "Platform *"}}}}}});
    ctx.check(held(p).entries[0].instruction.data.value("Text", std::string()) == "Platform *", "the typed station name is stored");
    act(p, context, {{"op", "set_instruction"}, {"entry", 0}, {"part", {{"Id", std::string(ids::kDestination)}, {"Data", {{"Text", std::string(40, 'x')}}}}}});
    ctx.check(held(p).entries[0].instruction.data.value("Text", std::string()).size() == 25, "station names are cut at 25 characters");
    act(p, context, {{"op", "set_instruction"}, {"entry", 0}, {"part", {{"Id", "levitate:nonsense"}, {"Data", JsonT::object()}}}});
    ctx.check(held(p).entries[0].instruction.id == ids::kDestination, "an unknown instruction is refused");
    // the throttle scroll input: 5..100 in steps of 5
    act(p, context, {{"op", "set_instruction"}, {"entry", 0}, {"part", {{"Id", std::string(ids::kThrottle)}, {"Data", {{"Value", 37}}}}}});
    ctx.check(held(p).entries[0].instruction.id == ids::kThrottle, "the type selector changes the instruction");
    ctx.check(held(p).entries[0].instruction.data.value("Value", 0) == 35, "the throttle snaps to steps of 5");
    act(p, context, {{"op", "set_instruction"}, {"entry", 0}, {"part", {{"Id", std::string(ids::kDestination)}, {"Data", {{"Text", "B"}}}}}});
    ctx.check(!held(p).entries[0].conditions.empty(), "a travel instruction keeps its wait conditions");

    // condition editor: a delay is limited, a threshold takes digits only, slots take items
    act(p, context, {{"op", "set_condition"}, {"entry", 0}, {"column", 0}, {"index", 0}, {"part", {{"Id", std::string(ids::kDelay)}, {"Data", {{"Value", 500}, {"TimeUnit", 2}}}}}});
    auto c = held(p).entries[0].conditions[0][0];
    ctx.check(c.data.value("Value", 0) == 120 && c.data.value("TimeUnit", 0) == 2, "the duration is limited, the unit stored");
    auto stone = items::make("minecraft:stone", 3);
    ctx.require(stone.has_value(), "stone item");
    act(p, context, {{"op", "set_condition"}, {"entry", 0}, {"column", 0}, {"index", 0},
                     {"part", {{"Id", std::string(ids::kItemThreshold)}, {"Data", {{"Threshold", "12a34x5"}, {"Operator", 2}, {"Item", items::toJsonWithCount(*stone, 3)}}}}}});
    c = held(p).entries[0].conditions[0][0];
    ctx.check(c.id == ids::kItemThreshold, "the condition type changes");
    ctx.check(c.data.value("Threshold", std::string()) == "1234", "the threshold keeps 4 digits");
    ctx.check(c.data.value("Operator", 0) == 2, "the operator is stored");
    ctx.check(items::idOf(items::fromJson(c.data.value("Item", JsonT()))) == "minecraft:stone", "the reference item is stored");
    act(p, context, {{"op", "set_condition"}, {"entry", 0}, {"column", 0}, {"index", 0},
                     {"part", {{"Id", std::string(ids::kRedstoneLink)}, {"Data", {{"Frequency", JsonT::array({items::toJson(*stone), nullptr})}, {"Inverted", 1}}}}}});
    c = held(p).entries[0].conditions[0][0];
    ctx.check(c.data["Frequency"].is_array() && c.data["Frequency"].size() == 2 && c.data["Frequency"][1].is_null(), "the frequency slots are stored");
    ctx.check(c.data.value("Inverted", 0) == 1, "the link state is stored");
    act(p, context, {{"op", "set_condition"}, {"entry", 0}, {"column", 3}, {"index", 0}, {"part", {{"Id", std::string(ids::kDelay)}, {"Data", JsonT::object()}}}});
    ctx.check(held(p).entries[0].conditions.size() == 1 && held(p).entries[0].conditions[0][0].id == ids::kRedstoneLink, "an unknown column is refused");
    net::setTestCompanion(p, false);
    p.setSelectedItem(ItemStack::EMPTY_ITEM());
});
