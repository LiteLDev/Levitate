// Native trains screens (client mod, SPEC §4): the train station screen (idle / assembly /
// train present, door control, arriving trains) and the schedule editor. Both are opened by the
// server (screens::open) and send "screen.action" messages. Station and train names are typed into
// native text fields; the schedule's instruction / condition editors are native sub-screens
// (Create's editor of ScheduleScreen: type selector, text fields, scroll inputs and ghost item
// slots) that send the edited part to the server ("set_instruction" / "set_condition").

#ifdef LV_CLIENT

#include <algorithm>
#include <optional>


#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/trains/Schedule.h"
#include "feature/common/Items.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::trains {

using namespace lv::client::screen;
namespace items = feature::common::items;

namespace {

class StationScreen : public Screen {
public:
    // the station's name is the editable title (Create's station screen)
    StationScreen(Json const& data, SendFn send) : Screen("", 220, 168), mSend(std::move(send)), mData(data) {}

    void init() override { build(); }
    void onData(Json const& data) override {
        mData = data;
        // the screen is rebuilt with new data, but not while a name is being typed
        if (typing()) {
            mDirty = true;
            return;
        }
        rebuild();
    }
    void tick() override {
        if (mDirty && !typing()) rebuild();
    }

private:
    bool typing() const { return std::any_of(mFields.begin(), mFields.end(), [](TextField const* f) { return f->focused(); }); }
    void rebuild() {
        mDirty = false;
        mFields.clear();
        widgets.clear();
        build();
    }
    TextField& nameField(Rect r, std::string const& text, int maxLength, char const* op, std::string const& tip) {
        auto& f = add<TextField>(r, text, [this, text, op](std::string const& typed) {
            if (typed == text || typed.empty()) return;
            mSend({{"op", op}, {"text", typed}});
            log().info("trains: {} typed: '{}'", op, typed);
        });
        f.maxLength = maxLength;
        f.tooltip   = {tip};
        mFields.push_back(&f);
        return f;
    }

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
        nameField(Rect{8, 3, 204, 14}, mData.value("name", std::string("Track Station")), 25, "rename", ctr("lv.trains.station_name", "Station Name"));
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
            nameField(Rect{8, y - 1, 204, 13}, t.value("name", std::string()), 35, "train_name", ctr("lv.trains.train_name", "Train Name"));
            y += 15;
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

    SendFn                  mSend;
    Json                    mData;
    std::vector<TextField*> mFields;
    bool                    mDirty = false;
};

class ScheduleScreen : public Screen {
public:
    ScheduleScreen(Json const& data, SendFn send) : Screen(ctr("item.levitate:schedule.name", "Train Schedule"), 256, 200), mSend(std::move(send)), mData(data) {}

    void init() override { build(); }
    void onData(Json const& data) override {
        mData = data;
        if (mEdit) return; // the editor keeps its own copy until it is confirmed
        widgets.clear();
        build();
    }
    void onClose() override {
        // Create's ScheduleScreen#onClose: an open editor is applied
        if (mEdit) confirmEdit(false);
    }
    void clickInventory(int, ItemStack const& stack, Button b, bool) override {
        // the reference item / frequency slots take ghost items (the inventory is only read)
        if (!mEdit) return;
        if (items::isEmpty(stack)) setCursorItem(nullptr);
        else if (b == Button::Left) setCursorItem(items::toJsonWithCount(stack, 1));
    }

private:
    /// An open instruction / condition editor (Create's editor sub-screen of the schedule).
    struct Edit {
        bool condition = false;
        int  entry = 0, column = 0, index = 0;
        Json part; ///< {"Id", "Data"}
    };

    Json& field() { return mEdit->part["Data"]; }
    int   intOf(char const* key, int fallback) {
        auto const& d = field();
        return d.contains(key) && d[key].is_number() ? d[key].get<int>() : fallback;
    }

    void startEdit(bool condition, int entry, int column, int index) {
        auto const& entries = mData.value("schedule", Json::object()).value("Entries", Json::array());
        if (entry < 0 || entry >= int(entries.size())) return;
        auto const& e = entries[size_t(entry)];
        Edit        ed{condition, entry, column, index, Json()};
        if (condition) {
            auto const& cols = e.value("Conditions", Json::array());
            if (column < 0 || column >= int(cols.size()) || index < 0 || index >= int(cols[size_t(column)].size())) return;
            ed.part = cols[size_t(column)][size_t(index)];
        } else {
            ed.part = e.value("Instruction", Json::object());
        }
        if (!ed.part.contains("Data") || !ed.part["Data"].is_object()) ed.part["Data"] = Json::object();
        mEdit    = std::move(ed);
        mRebuild = true;
        log().info("trains: schedule {} editor opened for entry {}", condition ? "condition" : "instruction", entry + 1);
    }

    void confirmEdit(bool rebuild) {
        if (!mEdit) return;
        for (auto* f : mFields) f->blur(true);
        Json action{{"op", mEdit->condition ? "set_condition" : "set_instruction"}, {"entry", mEdit->entry}, {"part", mEdit->part}};
        if (mEdit->condition) action["column"] = mEdit->column, action["index"] = mEdit->index;
        mSend(action);
        log().info("trains: schedule {} editor confirmed: {}", mEdit->condition ? "condition" : "instruction", mEdit->part.value("Id", std::string()));
        mEdit.reset();
        setCursorItem(nullptr);
        if (rebuild) mRebuild = true;
    }

    /// A value of the edited part changed with the mouse wheel.
    ScrollInput& scrollField(float y, std::string const& label, char const* key, int min, int max, int fallback, std::vector<std::string> options = {}) {
        auto& s   = add<ScrollInput>(Rect{8, y, 240, 14}, label, min, max, std::clamp(intOf(key, fallback), min, max), [this, key](int v) {
            if (mEdit) field()[key] = v;
        });
        s.options = std::move(options);
        return s;
    }

    /// A ghost item slot of the edited part (`key`, or `key`[index] for the frequency).
    void itemSlot(float x, float y, char const* key, int index, std::vector<std::string> tip) {
        auto& slot = add<Slot>(Rect{x, y, 18, 18}, [this, key, index](Button b, bool) {
            if (!mEdit) return;
            Json item = b == Button::Left && cursorItem.is_object() ? cursorItem : Json();
            if (index < 0) field()[key] = item;
            else {
                if (!field()[key].is_array()) field()[key] = Json::array({nullptr, nullptr});
                field()[key][size_t(index)] = item;
            }
            mRebuild = true;
        });
        Json const& v = index < 0 ? field().value(key, Json()) : (field().value(key, Json::array()).size() > size_t(index) ? field()[key][size_t(index)] : Json());
        slot.stack   = iconOf(v);
        slot.tooltip = std::move(tip);
    }

    void buildEditor() {
        auto& ed   = *mEdit;
        auto  id   = ed.part.value("Id", std::string());
        auto  path = core::trains::pathOf(id);
        // type selector: Create's "Next Action:" / "Continue if/after:" scroll input
        auto const& types = mData.value(ed.condition ? "conditions" : "instructions", Json::array());
        int         cur   = 0;
        std::vector<std::string> names;
        for (size_t i = 0; i < types.size(); ++i) {
            names.push_back(types[i].value("name", std::string()));
            if (types[i].value("id", std::string()) == id) cur = int(i);
        }
        // the editor replaces the schedule's title (Create's editor sub-screen header)
        title = ed.condition ? ctr("create.schedule.condition.editor", "Condition Editor") : ctr("create.schedule.instruction.editor", "Instruction Editor");
        // (the scroll input adds ": " to its label: Create's "Next Action:" without its colon)
        auto bare = [](std::string s) {
            while (!s.empty() && (s.back() == ':' || s.back() == ' ')) s.pop_back();
            return s;
        };
        auto& type = add<ScrollInput>(Rect{8, 18, 240, 14}, bare(ed.condition ? ctr("create.schedule.condition_type", "Continue if/after:") : ctr("create.schedule.instruction_type", "Next Action:")), 0,
                                      std::max(0, int(types.size()) - 1), cur, [this, types](int v) {
            if (!mEdit || v < 0 || v >= int(types.size())) return;
            auto nid    = types[size_t(v)].value("id", std::string());
            auto p      = mEdit->condition ? core::trains::defaultCondition(nid) : core::trains::defaultInstruction(nid);
            mEdit->part = p.toJson();
            mRebuild    = true;
        });
        type.options  = names;
        float y       = 38;
        showInventory = false;
        height        = 200;
        if (!ed.condition) {
            if (field().contains("Text")) {
                bool        address = path == "package_retrieval", title = path == "rename";
                std::string base    = address ? "create.schedule.instruction.address_filter_edit_box" : title ? "create.schedule.instruction.name_edit_box" : "create.schedule.instruction.filter_edit_box";
                add<Label>(Rect{8, y, 240, 10}, ctr(base, address ? "Retrieve Packages with this Address" : title ? "Schedule Title" : "Station Name"), color::kTextDark);
                y += 11;
                auto& f = add<TextField>(Rect{8, y, 240, 14}, field().value("Text", std::string()), [this](std::string const& t) {
                    if (mEdit) field()["Text"] = t;
                });
                f.onChange  = [this](std::string const& t) {
                    if (mEdit) field()["Text"] = t;
                };
                f.maxLength = address ? 50 : 25;
                if (title) f.tooltip = {ctr(base + "_1", "Affects text shown on displays"), ctr(base + "_2", "Defaults to next destination's name")};
                else if (address)
                    f.tooltip = {ctr(base + "_1", "Use * as a text wildcard"), ctr(base + "_2", "Train may navigate anywhere to collect it"), ctr(base + "_3", "Add another instruction to make it return")};
                else f.tooltip = {ctr(base + "_1", "Use * as a text wildcard"), ctr(base + "_2", "Example: 'My Station, Platform *'"), ctr(base + "_3", "Train picks nearest unoccupied match")};
                mFields.push_back(&f);
                y += 18;
            }
            if (path == "throttle") {
                auto& t  = add<ScrollInput>(Rect{8, y, 240, 14}, ctr("create.schedule.instruction.throttle_edit_box", "Throttle"), 1, 20, std::clamp(intOf("Value", 100) / 5, 1, 20), [this](int v) {
                    if (mEdit) field()["Value"] = v * 5;
                });
                t.format  = [](int v) { return std::to_string(v * 5) + "%"; };
                t.tooltip = {ctr("create.schedule.instruction.throttle_edit_box_1", "Affects the top speed of the Train")};
            }
        } else if (path == "delay" || path == "idle") {
            scrollField(y, ctr("create.generic.duration", "Duration"), "Value", 0, 120, 5);
            y += 18;
            scrollField(y, ctr("lv.trains.unit", "Unit"), "TimeUnit", 0, 2, 1,
                        {ctr("create.generic.unit.ticks", "Ticks"), ctr("create.generic.unit.seconds", "Seconds"), ctr("create.generic.unit.minutes", "Minutes")});
        } else if (path == "time_of_day") {
            scrollField(y, ctr("create.generic.daytime.hour", "Hour"), "Hour", 0, 23, 8);
            y += 18;
            scrollField(y, ctr("create.generic.daytime.minute", "Minute"), "Minute", 0, 59, 0);
            y += 18;
            static char const* rots[] = {"24", "12", "6", "4", "3", "2", "1", "0_45", "0_30", "0_15"};
            std::vector<std::string> r;
            for (auto const* k : rots) r.push_back(ctr(std::string("create.schedule.condition.time_of_day.rotation.every_") + k, k));
            scrollField(y, ctr("create.schedule.condition.time_of_day.rotation", "Rotation"), "Rotation", 0, 9, 5, r);
        } else if (path == "item_threshold" || path == "fluid_threshold") {
            bool item     = path == "item_threshold";
            showInventory = true;
            height        = 120;
            scrollField(y, ctr("create.schedule.condition.threshold.train_holds", "Train holds"), "Operator", 0, 2, 0,
                        {ctr("create.schedule.condition.threshold.greater", "more than"), ctr("create.schedule.condition.threshold.less", "less than"), ctr("create.schedule.condition.threshold.equal", "exactly")});
            y += 18;
            itemSlot(8, y, item ? "Item" : "Bucket", -1,
                     {ctr("create.schedule.condition.threshold.place_item", "Reference Item"), ctr("create.schedule.condition.threshold.place_item_2", "Filters can be used"),
                      ctr("create.schedule.condition.threshold.place_item_3", "Leave empty to match any")});
            auto& t = add<TextField>(Rect{30, y + 2, 50, 14}, field().value("Threshold", std::string("10")), [this](std::string const& v) {
                if (mEdit) field()["Threshold"] = v;
            });
            t.onChange  = [this](std::string const& v) {
                if (mEdit) field()["Threshold"] = v;
            };
            t.maxLength = 4;
            t.filter    = [](char32_t ch) { return ch >= U'0' && ch <= U'9'; };
            t.tooltip   = {ctr("create.schedule.condition.threshold.x_units_of_item", "Amount")};
            mFields.push_back(&t);
            if (item) {
                auto& m   = add<ScrollInput>(Rect{86, y + 2, 162, 14}, ctr("create.schedule.condition.threshold.item_measure", "Item Measure"), 0, 1, std::clamp(intOf("Measure", 0), 0, 1), [this](int v) {
                    if (mEdit) field()["Measure"] = v;
                });
                m.options = {ctr("create.schedule.condition.threshold.items", "Items"), ctr("create.schedule.condition.threshold.stacks", "Stacks")};
            } else {
                add<Label>(Rect{86, y + 5, 162, 10}, ctr("create.schedule.condition.threshold.buckets", "Buckets"), color::kTextDark);
            }
        } else if (path == "redstone_link") {
            showInventory = true;
            height        = 120;
            itemSlot(8, y, "Frequency", 0, {ctr("create.logistics.firstFrequency", "Freq. #1")});
            itemSlot(28, y, "Frequency", 1, {ctr("create.logistics.secondFrequency", "Freq. #2")});
            auto& inv   = add<ScrollInput>(Rect{52, y + 2, 196, 14}, bare(ctr("create.schedule.condition.redstone_link.frequency_state", "Frequency state:")), 0, 1, std::clamp(intOf("Inverted", 0), 0, 1), [this](int v) {
                if (mEdit) field()["Inverted"] = v;
            });
            inv.options = {ctr("create.schedule.condition.redstone_link.powered", "Powered"), ctr("create.schedule.condition.redstone_link.unpowered", "Unpowered")};
        } else if (path == "player_count") {
            scrollField(y, ctr("create.schedule.condition.player_count.players", "Players"), "Count", 0, 20, 1);
            y += 18;
            scrollField(y, ctr("create.schedule.condition.player_count.condition", "Condition"), "Exact", 0, 1, 0,
                        {ctr("create.schedule.condition.player_count.exactly", "Exactly"), ctr("create.schedule.condition.player_count.or_above", "Or above")});
        }
        // Create's confirm button (I_CONFIRM) closes the editor
        add<IconButton>(Rect{226, height - 24, 22, 18}, "OK", [this] { confirmEdit(true); });
    }

    void small(float x, float y, std::string const& sym, std::string const& tip, Json action) {
        auto& b   = add<IconButton>(Rect{x, y, 14, 12}, sym, [this, action] { mSend(action); });
        b.tooltip = {tip};
    }

    void build() {
        mFields.clear();
        if (mEdit) {
            buildEditor();
            return;
        }
        showInventory       = false;
        height              = 200;
        title               = ctr("item.levitate:schedule.name", "Train Schedule");
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
            auto& ei   = add<IconButton>(Rect{180, y - 1, 14, 12}, "E", [this, e] { startEdit(false, e, 0, 0); });
            ei.tooltip = {ctr("create.gui.schedule.lmb_edit", "Left-Click to Edit")};
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
                        auto& ec   = add<IconButton>(Rect{208, y - 1, 14, 12}, "E", [this, e, c, i] { startEdit(true, e, int(c), int(i)); });
                        ec.tooltip = {ctr("create.gui.schedule.lmb_edit", "Left-Click to Edit")};
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

    SendFn                  mSend;
    Json                    mData;
    int                     mPage    = 0;
    bool                    mRebuild = false;
    std::optional<Edit>     mEdit;
    std::vector<TextField*> mFields;
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
