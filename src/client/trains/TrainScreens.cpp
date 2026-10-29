// Native trains screens (client mod, SPEC §4): the train station screen (idle / assembly /
// train present, door control, arriving trains) and the schedule editor. Both are opened by the
// server (screens::open) and send "screen.action" messages; text fields (names, station
// filters) are edited in the server's form, which the server opens for the action (SPEC §8).

#ifdef LV_CLIENT

#include <algorithm>

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "mod/Module.h"

namespace lv::client::trains {

using namespace lv::client::screen;

namespace {

class StationScreen : public Screen {
public:
    StationScreen(Json const& data, SendFn send) : Screen(data.value("name", std::string("Track Station")), 220, 168), mSend(std::move(send)), mData(data) {}

    void init() override { build(); }
    void onData(Json const& data) override {
        mData = data;
        title = data.value("name", title);
        widgets.clear();
        build();
    }

private:
    void line(float& y, std::string const& text, unsigned colour = color::kTextDark) {
        add<Label>(Rect{8, y, 204, 10}, text, colour);
        y += 11;
    }
    void button(float x, float y, float w, std::string const& text, Json action, std::vector<std::string> tip = {}, bool enabled = true) {
        auto& b   = add<IconButton>(Rect{x, y, w, 18}, text, [this, action] { mSend(action); });
        b.enabled = enabled;
        b.tooltip = std::move(tip);
    }

    void build() {
        float y = 20;
        auto& rename = add<IconButton>(Rect{196, 4, 18, 14}, "N", [this] { mSend({{"op", "rename"}}); });
        rename.tooltip = {ctr("lv.trains.station_name", "Station Name")};
        if (mData.value("assembling", false)) {
            line(y, ctr("create.station.assembly_title", "Train Assembly"), color::kTextGold);
            size_t n = mData.value("bogeys", size_t{0});
            line(y, n == 0 ? ctr("create.station.no_bogeys", "No Bogeys") : n == 1 ? ctr("create.station.one_bogey", "1 Bogey") : ctr("create.station.more_bogeys", "{0} Bogeys", n));
            line(y, ctr("create.station.how_to", "Use Train Casing on highlighted Tracks to create bogeys."), color::kTextGray);
            line(y, ctr("create.station.how_to_1", "Remove bogeys by breaking the block on top."), color::kTextGray);
            line(y, ctr("create.station.how_to_2", "Build carriages attached to one or two bogeys each."), color::kTextGray);
            if (mData["error"].is_object()) {
                line(y, ctr("create.station.failed", "Assembly Failed"), color::kButtonOff);
                if (int c = mData["error"].value("carriage", 0)) line(y, ctr("create.station.carriage_number", "Carriage {0}:", c));
                line(y, mData["error"].value("text", std::string()));
                line(y, ctr("create.station.retry", "Resolve this and retry"), color::kTextGray);
            }
            button(8, 140, 100, ctr("create.station.assemble_train", "Assemble Train"), {{"op", "assemble"}});
            button(112, 140, 100, ctr("create.station.cancel", "Cancel Assembly"), {{"op", "cancel"}});
            return;
        }
        if (mData.contains("train")) {
            auto const& t = mData["train"];
            line(y, t.value("name", std::string()), color::kTextGold);
            auto& rn   = add<IconButton>(Rect{196, y - 12, 18, 12}, "N", [this] { mSend({{"op", "train_name"}}); });
            rn.tooltip = {ctr("lv.trains.train_name", "Train Name")};
            bool can = t.value("canDisassemble", false);
            button(8, y, 120, ctr("create.station.disassemble_train", "Disassemble Train"), {{"op", "disassemble"}},
                   can ? std::vector<std::string>{} : std::vector<std::string>{ctr("create.station.train_not_aligned", "Cannot disassemble,"), ctr("create.station.train_not_aligned_1", "not all carriages aligned")},
                   can);
            y += 22;
            if (t.value("schedule", false)) {
                bool a = t.value("autoSchedule", false);
                button(8, y, 120, a ? ctr("create.station.remove_auto_schedule", "Discard Auto-Schedule") : ctr("create.station.remove_schedule", "Retrieve Schedule"), {{"op", "remove_schedule"}});
                y += 22;
            }
        } else {
            line(y, ctr("create.station.idle", "Station is Idle"), color::kTextGray);
            if (mData.contains("incoming")) line(y, mData["incoming"].value("name", std::string()) + " ...", color::kTextGold);
            if (mData.value("trackOk", false)) {
                button(8, y, 120, ctr("create.station.create_train", "Create new Train"), {{"op", "create"}});
                y += 22;
            } else {
                bool diag = mData.value("diagonal", false);
                line(y, ctr("create.station.no_assembly_diagonal", "Cannot build trains"), color::kButtonOff);
                line(y, diag ? ctr("create.station.no_assembly_diagonal_1", "on diagonal tracks") : ctr("create.station.no_assembly_curve_1", "on curved tracks"), color::kButtonOff);
            }
        }
        // door control
        static char const* keys[] = {"all", "north", "east", "south", "west", "none"};
        static char const* fb[]   = {"Open All", "North", "East", "South", "West", "None"};
        auto& door = add<ScrollInput>(Rect{8, y, 204, 14}, ctr("create.contraption.door_control", "Onboard Door Control"), 0, 5, mData.value("door", 0), [this](int v) {
            mSend({{"op", "door"}, {"mode", v}});
        });
        for (int i = 0; i < 6; ++i) door.options.push_back(ctr(std::string("create.contraption.door_control.") + keys[i] + ".short", fb[i]));
        y += 18;
        auto const& arr = mData.value("arrivals", Json::array());
        if (!arr.empty()) line(y, ctr("lv.trains.arrivals", "Arriving trains"), color::kTextGold);
        for (size_t i = 0; i < arr.size() && i < 4; ++i) line(y, arr[i].value("eta", std::string()) + "  " + arr[i].value("name", std::string()));
    }

    SendFn mSend;
    Json   mData;
};

class ScheduleScreen : public Screen {
public:
    ScheduleScreen(Json const& data, SendFn send) : Screen(ctr("item.levitate:schedule.name", "Train Schedule"), 256, 200), mSend(std::move(send)), mData(data) {}

    void init() override { build(); }
    void onData(Json const& data) override {
        mData = data;
        widgets.clear();
        build();
    }

private:
    void small(float x, float y, std::string const& sym, std::string const& tip, Json action) {
        auto& b   = add<IconButton>(Rect{x, y, 14, 12}, sym, [this, action] { mSend(action); });
        b.tooltip = {tip};
    }

    void build() {
        auto const& sch     = mData.value("schedule", Json::object());
        auto const& texts   = mData.value("texts", Json::array());
        int         entries = int(texts.size());
        int         pages   = std::max(1, (entries + 2) / 3);
        mPage               = std::clamp(mPage, 0, pages - 1);
        float y             = 18;
        if (entries == 0) add<Label>(Rect{8, y, 240, 10}, ctr("create.schedule.no_stops", "This Schedule does not have any Stops yet"), color::kTextGray);
        int progress = sch.value("Progress", 0);
        for (int e = mPage * 3; e < std::min(entries, mPage * 3 + 3); ++e) {
            auto const& t = texts[size_t(e)];
            std::string head = std::to_string(e + 1) + ". " + t.value("summary", std::string());
            add<Label>(Rect{8, y, 170, 10}, head, e == progress && progress > 0 ? color::kTextGold : color::kTextDark);
            small(180, y - 1, "E", ctr("create.gui.schedule.lmb_edit", "Left-Click to Edit"), {{"op", "edit_instruction"}, {"entry", e}});
            small(194, y - 1, "T", ctr("create.schedule.instruction_type", "Next Action:"), {{"op", "cycle_instruction"}, {"entry", e}});
            small(208, y - 1, "^", ctr("create.gui.schedule.move_up", "Move up"), {{"op", "up"}, {"entry", e}});
            small(222, y - 1, "v", ctr("create.gui.schedule.move_down", "Move down"), {{"op", "down"}, {"entry", e}});
            small(236, y - 1, "x", ctr("create.gui.schedule.remove_entry", "Remove Action"), {{"op", "remove"}, {"entry", e}});
            y += 13;
            if (t.value("supports", true)) {
                auto const& cols = t.value("conditions", Json::array());
                for (size_t c = 0; c < cols.size(); ++c) {
                    for (size_t i = 0; i < cols[c].size(); ++i) {
                        std::string prefix = (c > 0 && i == 0) ? ctr("lv.trains.or", "or") + " " : (i > 0 ? ctr("lv.trains.and", "and") + " " : "  ");
                        add<Label>(Rect{16, y, 190, 10}, prefix + cols[c][i].get<std::string>(), color::kTextGray);
                        small(208, y - 1, "E", ctr("create.gui.schedule.lmb_edit", "Left-Click to Edit"), {{"op", "edit_condition"}, {"entry", e}, {"column", c}, {"index", i}});
                        small(222, y - 1, "x", ctr("create.gui.schedule.rmb_remove", "Right-Click to Remove"), {{"op", "remove_condition"}, {"entry", e}, {"column", c}, {"index", i}});
                        y += 12;
                    }
                }
                small(16, y, "+", ctr("create.gui.schedule.add_condition", "Add Condition"), {{"op", "add_condition"}, {"entry", e}, {"column", int(cols.size()) - 1}});
                small(32, y, "|", ctr("create.gui.schedule.alternative_condition", "Alternative Condition"), {{"op", "add_condition"}, {"entry", e}, {"column", -1}});
                y += 15;
            }
            y += 3;
        }
        float by = 176;
        auto& add1 = add<IconButton>(Rect{8, by, 70, 18}, "+ " + ctr("create.gui.schedule.add_entry", "Add Action"), [this] { mSend({{"op", "add"}}); });
        (void)add1;
        auto& loop = add<IconButton>(Rect{82, by, 70, 18}, ctr("create.schedule.loop", "Loop Forever"), [this] { mSend({{"op", "loop"}}); });
        loop.toggled = sch.value("Cyclic", true);
        auto& reset   = add<IconButton>(Rect{156, by, 18, 18}, "R", [this] { mSend({{"op", "reset"}}); });
        reset.tooltip = {ctr("create.schedule.reset", "Reset Progress")};
        auto& skip    = add<IconButton>(Rect{176, by, 18, 18}, ">", [this] { mSend({{"op", "skip"}}); });
        skip.tooltip  = {ctr("create.schedule.skip", "Skip current Stop")};
        if (pages > 1) {
            add<IconButton>(Rect{204, by, 18, 18}, "<", [this] { --mPage; mRebuild = true; });
            add<IconButton>(Rect{228, by, 18, 18}, ">", [this] { ++mPage; mRebuild = true; });
        }
    }

    void tick() override {
        if (!mRebuild) return;
        mRebuild = false;
        widgets.clear();
        build();
    }

    SendFn mSend;
    Json   mData;
    int    mPage    = 0;
    bool   mRebuild = false;
};

} // namespace

class TrainScreensModule {
public:
    void install() {
        registerServerScreen("trains.station", [](Json const& data, SendFn send) { return std::make_unique<StationScreen>(data, std::move(send)); });
        registerServerScreen("trains.schedule", [](Json const& data, SendFn send) { return std::make_unique<ScheduleScreen>(data, std::move(send)); });
    }
    void uninstall() {}
};

} // namespace lv::client::trains

LV_REGISTER_MODULE(lv::client::trains::TrainScreensModule);

#endif // LV_CLIENT
