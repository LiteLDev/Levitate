// Trains unit: the schedule item (Create's ScheduleItem): its data, the schedule editor
// (native screen for companions, forms for vanilla players) and handing schedules to
// conductors.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "core/common/Utf8.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/trains/Trains.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace items   = common::items;
namespace screens = common::screens;
namespace ids     = ct::ids;

namespace {
constexpr char kDataKey[] = "lv:schedule";
}

std::optional<ct::Schedule> scheduleOf(ItemStack const& stack) {
    if (stack.isNull() || items::idOf(stack) != kSchedule) return std::nullopt;
    auto j = items::getData(stack, kDataKey);
    if (j.is_null()) return ct::Schedule{};
    return ct::Schedule::fromJson(j);
}

void setScheduleOf(ItemStack& stack, ct::Schedule const& s) {
    items::setData(stack, kDataKey, s.toJson());
    // Create's ScheduleItem.appendHoverText: the destinations, the one it goes on from marked
    std::vector<std::string> lore;
    for (size_t i = 0; i < s.entries.size(); ++i) {
        auto const& e = s.entries[i];
        if (e.instruction.id != ids::kDestination) continue;
        bool current = int(i) == s.savedProgress && s.entries.size() > 1;
        lore.push_back(std::string(current ? "\xC2\xA7" "7-> \xC2\xA7" "e" : "\xC2\xA7" "7> \xC2\xA7" "6")
                       + e.instruction.data.value("Text", std::string()));
    }
    stack.setCustomLore(lore);
}

ItemStack makeScheduleItem(ct::Schedule const& s) {
    auto item = items::make(kSchedule);
    if (!item) return ItemStack{};
    setScheduleOf(*item, s);
    return *item;
}

// ---- texts --------------------------------------------------------------------------------------

namespace {

std::string trP(Player& p, std::string const& key, std::string_view fb) { return trFor(p, key, fb); }

std::string instructionName(Player& p, std::string_view id) {
    static std::map<std::string, std::string, std::less<>> fb{
        {"destination",       "Travel to Station"    },
        {"package_delivery",  "Deliver Package"      },
        {"package_retrieval", "Retrieve Package"     },
        {"rename",            "Update Schedule Title"},
        {"throttle",          "Limit Max Speed"      }
    };
    auto path = ct::pathOf(id);
    return trP(p, "create.schedule.instruction." + path, fb.contains(path) ? fb.find(path)->second : path);
}

std::string conditionName(Player& p, std::string_view id) {
    static std::map<std::string, std::string, std::less<>> fb{
        {"delay",           "Scheduled Delay"      },
        {"time_of_day",     "Time of Day"          },
        {"fluid_threshold", "Fluid Cargo Condition"},
        {"item_threshold",  "Item Cargo Condition" },
        {"redstone_link",   "Redstone Link"        },
        {"player_count",    "Players Seated"       },
        {"idle",            "Cargo Inactivity"     },
        {"unloaded",        "Chunk Unloaded"       },
        {"powered",         "Station Powered"      }
    };
    auto path = ct::pathOf(id);
    return trP(p, "create.schedule.condition." + path, fb.contains(path) ? fb.find(path)->second : path);
}

std::string durationText(Player& p, ct::Json const& d) {
    int         unit = d.value("TimeUnit", 1);
    static char const* keys[] = {"create.generic.unit.ticks", "create.generic.unit.seconds", "create.generic.unit.minutes"};
    static char const* fbs[]  = {"Ticks", "Seconds", "Minutes"};
    unit = std::clamp(unit, 0, 2);
    return std::to_string(d.value("Value", 5)) + " " + trP(p, keys[unit], fbs[unit]);
}

std::string instructionSummary(Player& p, ct::Part const& in) {
    std::string name = instructionName(p, in.id);
    if (in.id == ids::kDestination || in.id == ids::kRename || in.id == ids::kPackageRetrieval)
        return name + ": " + in.data.value("Text", std::string());
    if (in.id == ids::kThrottle)
        return core::formatMessageArgs(
            trP(p, "create.schedule.instruction.throttle.summary", "Change Max Speed to {0}"),
            std::to_string(in.data.value("Value", 100)) + "%"
        );
    return name;
}

std::string conditionSummary(Player& p, ct::Part const& c) {
    auto const& d = c.data;
    if (c.id == ids::kDelay) return core::formatMessageArgs(trP(p, "create.schedule.condition.delay_short", "Wait: {0}"), durationText(p, d));
    if (c.id == ids::kIdle) return core::formatMessageArgs(trP(p, "create.schedule.condition.idle_short", "Cargo Idle: {0}"), durationText(p, d));
    if (c.id == ids::kTimeOfDay) {
        auto clock = ct::clockOf(d);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", clock.hour, clock.minute, clock.pm ? "pm" : "am");
        return core::formatMessageArgs(trP(p, "create.schedule.condition.time_of_day.scheduled", "Scheduled Time: {0}"), std::string(buf));
    }
    if (c.id == ids::kPlayerCount) {
        int n = d.value("Count", 1);
        return core::formatMessageArgs(
            trP(p, n == 1 ? "create.schedule.condition.player_count.summary" : "create.schedule.condition.player_count.summary_plural", n == 1 ? "{0} Player" : "{0} Players"),
            n
        );
    }
    if (c.id == ids::kItemThreshold || c.id == ids::kFluidThreshold) {
        static char const* ops[] = {">", "<", "="};
        return conditionName(p, c.id) + " " + ops[std::clamp(d.value("Operator", 0), 0, 2)] + " " + d.value("Threshold", std::string("10"));
    }
    if (c.id == ids::kRedstoneLink)
        return d.value("Inverted", 0) ? trP(p, "create.schedule.condition.redstone_link_off", "Link Off")
                                      : trP(p, "create.schedule.condition.redstone_link_on", "Link On");
    return conditionName(p, c.id);
}

// ---- editing the item ---------------------------------------------------------------------------

ItemStack* heldSchedule(Player& p, int slot) {
    auto& inv = p.getInventory();
    if (slot < 0 || slot >= inv.getContainerSize()) return nullptr;
    auto const& it = inv.getItem(slot);
    if (it.isNull() || items::idOf(it) != kSchedule) return nullptr;
    return const_cast<ItemStack*>(&it);
}

bool writeSchedule(Player& p, int slot, ct::Schedule const& s) {
    auto* held = heldSchedule(p, slot);
    if (!held) return false;
    ItemStack copy = *held;
    setScheduleOf(copy, s);
    p.getInventory().setItem(slot, copy);
    p.sendInventory(false);
    return true;
}

std::optional<ct::Schedule> readSchedule(Player& p, int slot) {
    auto* held = heldSchedule(p, slot);
    if (!held) return std::nullopt;
    return scheduleOf(*held);
}

void mainForm(Player& p, int slot);
void entryForm(Player& p, int slot, int entry);

template <class T>
T resultOr(ll::form::CustomFormResult const& r, char const* key, T fallback) {
    auto it = r->find(key);
    if (it == r->end()) return fallback;
    if constexpr (std::is_same_v<T, std::string>) {
        if (auto const* v = std::get_if<std::string>(&it->second)) return *v;
    } else {
        if (auto const* v = std::get_if<uint64>(&it->second)) return T(*v);
        if (auto const* d = std::get_if<double>(&it->second)) return T(std::lround(*d));
    }
    return fallback;
}

void instructionForm(Player& p, int slot, int entry) {
    auto s = readSchedule(p, slot);
    if (!s || entry < 0 || entry >= int(s->entries.size())) return;
    auto const& in = s->entries[entry].instruction;
    std::vector<std::string> names;
    size_t                   cur = 0;
    for (size_t i = 0; i < ct::instructionIds().size(); ++i) {
        names.push_back(instructionName(p, ct::instructionIds()[i]));
        if (ct::instructionIds()[i] == in.id) cur = i;
    }
    ll::form::CustomForm form(trP(p, "create.schedule.instruction.editor", "Instruction Editor"));
    form.appendDropdown("type", trP(p, "create.schedule.instruction_type", "Next Action:"), names, cur);
    std::string textLabel = in.id == ids::kRename ? trP(p, "create.schedule.instruction.name_edit_box", "Schedule Title")
                                                  : trP(p, "create.schedule.instruction.filter_edit_box", "Station Name");
    form.appendInput("text", textLabel, "", in.data.value("Text", std::string()));
    form.appendSlider("throttle", trP(p, "create.schedule.instruction.throttle_edit_box", "Throttle"), 5, 100, 5, in.data.value("Value", 100));
    form.sendTo(p, [slot, entry](Player& pl, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        auto s = readSchedule(pl, slot);
        if (!s || entry < 0 || entry >= int(s->entries.size())) return;
        auto& e    = s->entries[entry];
        auto  type = std::min(resultOr<size_t>(r, "type", 0), ct::instructionIds().size() - 1);
        auto  id   = std::string(ct::instructionIds()[type]);
        if (id != e.instruction.id) {
            e.instruction = ct::defaultInstruction(id);
            if (ct::supportsConditions(id) && e.conditions.empty()) e.conditions.push_back({ct::defaultCondition(ids::kDelay)});
        }
        if (e.instruction.data.contains("Text")) e.instruction.data["Text"] = resultOr<std::string>(r, "text", "");
        if (id == ids::kThrottle) e.instruction.data["Value"] = std::clamp(resultOr<int>(r, "throttle", 100), 5, 100) / 5 * 5;
        writeSchedule(pl, slot, *s);
        entryForm(pl, slot, entry);
    });
}

void conditionForm(Player& p, int slot, int entry, int column, int index) {
    auto s = readSchedule(p, slot);
    if (!s || entry < 0 || entry >= int(s->entries.size())) return;
    auto& cols = s->entries[entry].conditions;
    if (column < 0 || index < 0 || column >= int(cols.size()) || index >= int(cols[column].size())) return;
    auto const& c = cols[column][index];
    auto const& d = c.data;
    std::vector<std::string> names;
    size_t                   cur = 0;
    for (size_t i = 0; i < ct::conditionIds().size(); ++i) {
        names.push_back(conditionName(p, ct::conditionIds()[i]));
        if (ct::conditionIds()[i] == c.id) cur = i;
    }
    ll::form::CustomForm form(trP(p, "create.schedule.condition.editor", "Condition Editor"));
    form.appendDropdown("type", trP(p, "create.schedule.condition_type", "Continue if/after:"), names, cur);
    if (c.id == ids::kDelay || c.id == ids::kIdle) {
        form.appendSlider("value", trP(p, "create.generic.duration", "Duration"), 0, 120, 1, d.value("Value", 5));
        form.appendDropdown(
            "unit",
            trP(p, "lv.trains.unit", "Unit"),
            {trP(p, "create.generic.unit.ticks", "Ticks"), trP(p, "create.generic.unit.seconds", "Seconds"), trP(p, "create.generic.unit.minutes", "Minutes")},
            size_t(std::clamp(d.value("TimeUnit", 1), 0, 2))
        );
    } else if (c.id == ids::kTimeOfDay) {
        form.appendSlider("hour", trP(p, "create.generic.daytime.hour", "Hour"), 0, 23, 1, d.value("Hour", 8));
        form.appendSlider("minute", trP(p, "create.generic.daytime.minute", "Minute"), 0, 59, 1, d.value("Minute", 0));
        static char const* rots[] = {"24", "12", "6", "4", "3", "2", "1", "0_45", "0_30", "0_15"};
        std::vector<std::string> r;
        for (auto const* k : rots) r.push_back(trP(p, std::string("create.schedule.condition.time_of_day.rotation.every_") + k, k));
        form.appendDropdown("rotation", trP(p, "create.schedule.condition.time_of_day.rotation", "Rotation"), r, size_t(std::clamp(d.value("Rotation", 5), 0, 9)));
    } else if (c.id == ids::kItemThreshold || c.id == ids::kFluidThreshold) {
        form.appendDropdown(
            "op",
            trP(p, "create.schedule.condition.threshold.train_holds", "Train holds"),
            {trP(p, "create.schedule.condition.threshold.greater", "more than"), trP(p, "create.schedule.condition.threshold.less", "less than"), trP(p, "create.schedule.condition.threshold.equal", "exactly")},
            size_t(std::clamp(d.value("Operator", 0), 0, 2))
        );
        form.appendInput("threshold", trP(p, "create.schedule.condition.threshold.x_units_of_item", "Amount"), "10", d.value("Threshold", std::string("10")));
        if (c.id == ids::kItemThreshold) {
            form.appendDropdown(
                "measure",
                trP(p, "create.schedule.condition.threshold.item_measure", "Item Measure"),
                {trP(p, "create.schedule.condition.threshold.items", "Items"), trP(p, "create.schedule.condition.threshold.stacks", "Stacks")},
                size_t(std::clamp(d.value("Measure", 0), 0, 1))
            );
        }
        form.appendToggle("offhand", trP(p, "lv.trains.filter_from_offhand", "Use the item in your off hand as the filter"), false);
    } else if (c.id == ids::kPlayerCount) {
        form.appendSlider("count", trP(p, "create.schedule.condition.player_count.players", "Players"), 0, 20, 1, d.value("Count", 1));
        form.appendDropdown(
            "exact",
            trP(p, "create.schedule.condition.player_count.condition", "Condition"),
            {trP(p, "create.schedule.condition.player_count.exactly", "Exactly"), trP(p, "create.schedule.condition.player_count.or_above", "Or above")},
            size_t(std::clamp(d.value("Exact", 0), 0, 1))
        );
    } else if (c.id == ids::kRedstoneLink) {
        form.appendToggle("inverted", trP(p, "create.schedule.condition.redstone_link.unpowered", "Link Off"), d.value("Inverted", 0) != 0);
        form.appendToggle("frequency", trP(p, "lv.trains.frequency_from_hands", "Use the items in your off hand and main hand as the frequency"), false);
    }
    form.appendToggle("remove", trP(p, "create.gui.schedule.rmb_remove", "Right-Click to Remove"), false);
    form.sendTo(p, [slot, entry, column, index](Player& pl, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        auto s = readSchedule(pl, slot);
        if (!s || entry < 0 || entry >= int(s->entries.size())) return;
        auto& cols = s->entries[entry].conditions;
        if (column < 0 || index < 0 || column >= int(cols.size()) || index >= int(cols[column].size())) return;
        auto& c = cols[column][index];
        if (resultOr<int>(r, "remove", 0)) {
            cols[column].erase(cols[column].begin() + index);
            if (cols[column].empty()) cols.erase(cols.begin() + column);
            writeSchedule(pl, slot, *s);
            entryForm(pl, slot, entry);
            return;
        }
        auto type = std::min(resultOr<size_t>(r, "type", 0), ct::conditionIds().size() - 1);
        auto id   = std::string(ct::conditionIds()[type]);
        if (id != c.id) {
            c = ct::defaultCondition(id);
            writeSchedule(pl, slot, *s);
            conditionForm(pl, slot, entry, column, index); // the new type's fields
            return;
        }
        auto& d = c.data;
        if (r->contains("value")) d["Value"] = resultOr<int>(r, "value", 5);
        if (r->contains("unit")) d["TimeUnit"] = resultOr<int>(r, "unit", 1);
        if (r->contains("hour")) d["Hour"] = resultOr<int>(r, "hour", 8);
        if (r->contains("minute")) d["Minute"] = resultOr<int>(r, "minute", 0);
        if (r->contains("rotation")) d["Rotation"] = resultOr<int>(r, "rotation", 5);
        if (r->contains("op")) d["Operator"] = resultOr<int>(r, "op", 0);
        if (r->contains("threshold")) d["Threshold"] = resultOr<std::string>(r, "threshold", "10");
        if (r->contains("measure")) d["Measure"] = resultOr<int>(r, "measure", 0);
        if (r->contains("count")) d["Count"] = resultOr<int>(r, "count", 1);
        if (r->contains("exact")) d["Exact"] = resultOr<int>(r, "exact", 0);
        if (r->contains("inverted")) d["Inverted"] = resultOr<int>(r, "inverted", 0);
        if (resultOr<int>(r, "frequency", 0)) {
            auto const& off  = pl.getOffhandSlot();
            auto const& main = pl.getSelectedItem();
            d["Frequency"]   = ct::Json::array({off.isNull() ? ct::Json() : items::toJson(off), main.isNull() ? ct::Json() : items::toJson(main)});
        }
        if (resultOr<int>(r, "offhand", 0)) {
            auto const& off = pl.getOffhandSlot();
            d[c.id == ids::kItemThreshold ? "Item" : "Bucket"] = off.isNull() ? ct::Json() : items::toJson(off);
        }
        writeSchedule(pl, slot, *s);
        entryForm(pl, slot, entry);
    });
}

void entryForm(Player& p, int slot, int entry) {
    auto s = readSchedule(p, slot);
    if (!s || entry < 0 || entry >= int(s->entries.size())) return;
    auto const& e = s->entries[entry];
    ll::form::SimpleForm form(std::to_string(entry + 1) + ". " + instructionName(p, e.instruction.id), instructionSummary(p, e.instruction));
    struct Btn {
        std::string text;
        std::function<void(Player&)> fn;
    };
    std::vector<Btn> buttons;
    buttons.push_back({trP(p, "create.schedule.instruction.editor", "Instruction Editor"), [slot, entry](Player& pl) { instructionForm(pl, slot, entry); }});
    if (ct::supportsConditions(e.instruction.id)) {
        for (int c = 0; c < int(e.conditions.size()); ++c) {
            for (int i = 0; i < int(e.conditions[c].size()); ++i) {
                std::string prefix = (c > 0 && i == 0) ? trP(p, "lv.trains.or", "or") + " " : (i > 0 ? trP(p, "lv.trains.and", "and") + " " : "");
                buttons.push_back({prefix + conditionSummary(p, e.conditions[c][i]), [slot, entry, c, i](Player& pl) { conditionForm(pl, slot, entry, c, i); }});
            }
            buttons.push_back({"+ " + trP(p, "create.gui.schedule.add_condition", "Add Condition"), [slot, entry, c](Player& pl) {
                                   auto s = readSchedule(pl, slot);
                                   if (!s || entry >= int(s->entries.size()) || c >= int(s->entries[entry].conditions.size())) return;
                                   s->entries[entry].conditions[c].push_back(ct::defaultCondition(ids::kDelay));
                                   writeSchedule(pl, slot, *s);
                                   entryForm(pl, slot, entry);
                               }});
        }
        buttons.push_back({"+ " + trP(p, "create.gui.schedule.alternative_condition", "Alternative Condition"), [slot, entry](Player& pl) {
                               auto s = readSchedule(pl, slot);
                               if (!s || entry < 0 || entry >= int(s->entries.size())) return;
                               s->entries[entry].conditions.push_back({ct::defaultCondition(ids::kDelay)});
                               writeSchedule(pl, slot, *s);
                               entryForm(pl, slot, entry);
                           }});
    }
    auto edit = [slot](std::function<void(ct::Schedule&)> fn) {
        return [slot, fn](Player& pl) {
            auto s = readSchedule(pl, slot);
            if (!s) return;
            fn(*s);
            writeSchedule(pl, slot, *s);
            mainForm(pl, slot);
        };
    };
    buttons.push_back({trP(p, "create.gui.schedule.move_up", "Move up"), edit([entry](ct::Schedule& s) {
                           if (entry > 0 && entry < int(s.entries.size())) std::swap(s.entries[entry], s.entries[entry - 1]);
                       })});
    buttons.push_back({trP(p, "create.gui.schedule.move_down", "Move down"), edit([entry](ct::Schedule& s) {
                           if (entry + 1 < int(s.entries.size())) std::swap(s.entries[entry], s.entries[entry + 1]);
                       })});
    buttons.push_back({trP(p, "create.gui.schedule.duplicate", "Duplicate"), edit([entry](ct::Schedule& s) {
                           if (entry < int(s.entries.size())) s.entries.insert(s.entries.begin() + entry + 1, s.entries[entry]);
                       })});
    buttons.push_back({trP(p, "create.gui.schedule.remove_entry", "Remove Action"), edit([entry](ct::Schedule& s) {
                           if (entry < int(s.entries.size())) s.entries.erase(s.entries.begin() + entry);
                       })});
    buttons.push_back({"<", [slot](Player& pl) { mainForm(pl, slot); }});
    for (auto& b : buttons) form.appendButton(b.text, b.fn);
    form.sendTo(p);
}

void mainForm(Player& p, int slot) {
    auto s = readSchedule(p, slot);
    if (!s) return;
    std::string content;
    if (s->entries.empty()) content = trP(p, "create.schedule.no_stops", "This Schedule does not have any Stops yet");
    ll::form::SimpleForm form(items::displayName(*heldSchedule(p, slot)), content);
    for (int i = 0; i < int(s->entries.size()); ++i) {
        std::string label = std::to_string(i + 1) + ". " + instructionSummary(p, s->entries[i].instruction);
        if (i == s->savedProgress && s->savedProgress > 0) label = "> " + label;
        form.appendButton(label, [slot, i](Player& pl) { entryForm(pl, slot, i); });
    }
    form.appendButton("+ " + trP(p, "create.gui.schedule.add_entry", "Add Action"), [slot](Player& pl) {
        auto s = readSchedule(pl, slot);
        if (!s) return;
        s->entries.push_back(ct::Entry::makeDefault());
        writeSchedule(pl, slot, *s);
        instructionForm(pl, slot, int(s->entries.size()) - 1);
    });
    form.appendButton(trP(p, "create.schedule.loop", "Loop Forever") + ": " + (s->cyclic ? "ON" : "OFF"), [slot](Player& pl) {
        auto s = readSchedule(pl, slot);
        if (!s) return;
        s->cyclic = !s->cyclic;
        writeSchedule(pl, slot, *s);
        mainForm(pl, slot);
    });
    if (s->savedProgress > 0) {
        form.appendButton(trP(p, "create.schedule.reset", "Reset Progress"), [slot](Player& pl) {
            auto s = readSchedule(pl, slot);
            if (!s) return;
            s->savedProgress = 0;
            writeSchedule(pl, slot, *s);
            mainForm(pl, slot);
        });
        form.appendButton(trP(p, "create.schedule.skip", "Skip current Stop"), [slot](Player& pl) {
            auto s = readSchedule(pl, slot);
            if (!s || s->entries.empty()) return;
            s->savedProgress = (s->savedProgress + 1) % int(s->entries.size());
            writeSchedule(pl, slot, *s);
            mainForm(pl, slot);
        });
    }
    form.sendTo(p);
}

/// An item of an editor slot (null: empty), as the condition data keeps it.
ct::Json slotItem(ct::Json const& j) {
    if (!j.is_object()) return nullptr;
    auto stack = items::fromJson(j);
    if (items::isEmpty(stack)) return nullptr;
    return items::toJson(stack);
}

int intIn(ct::Json const& d, char const* key, int fallback, int lo, int hi) {
    auto it = d.find(key);
    int  v  = it != d.end() && it->is_number() ? it->get<int>() : fallback;
    return std::clamp(v, lo, hi);
}

} // namespace

std::optional<ct::Part> sanitiseInstruction(ct::Part const& requested, ct::Part const& current) {
    auto id   = ct::normaliseId(requested.id);
    auto list = ct::instructionIds();
    if (std::find(list.begin(), list.end(), id) == list.end()) return std::nullopt;
    ct::Part out  = ct::defaultInstruction(id);
    auto const& d = requested.data.is_object() ? requested.data : ct::Json::object();
    if (out.data.contains("Text")) {
        // Create's edit boxes: station names / titles 25, package addresses 50 characters
        std::string text = d.value("Text", std::string());
        if (!d.contains("Text") && id == current.id) text = current.data.value("Text", std::string());
        size_t max = id == ids::kPackageRetrieval ? 50 : 25;
        out.data["Text"] = core::utf8Prefix(text, max);
    }
    if (id == ids::kThrottle) out.data["Value"] = intIn(d, "Value", 100, 5, 100) / 5 * 5;
    return out;
}

std::optional<ct::Part> sanitiseCondition(ct::Part const& requested, ct::Part const& current) {
    (void)current;
    auto id   = ct::normaliseId(requested.id);
    auto list = ct::conditionIds();
    if (std::find(list.begin(), list.end(), id) == list.end()) return std::nullopt;
    ct::Part    out = ct::defaultCondition(id);
    auto const& d   = requested.data.is_object() ? requested.data : ct::Json::object();
    auto&       o   = out.data;
    if (id == ids::kDelay || id == ids::kIdle) {
        o["Value"]    = intIn(d, "Value", 5, 0, 120);
        o["TimeUnit"] = intIn(d, "TimeUnit", 1, 0, 2);
    } else if (id == ids::kTimeOfDay) {
        o["Hour"]     = intIn(d, "Hour", 8, 0, 23);
        o["Minute"]   = intIn(d, "Minute", 0, 0, 59);
        o["Rotation"] = intIn(d, "Rotation", 5, 0, 9);
    } else if (id == ids::kItemThreshold || id == ids::kFluidThreshold) {
        o["Operator"] = intIn(d, "Operator", 0, 0, 2);
        // Create's threshold box takes digits only
        std::string t;
        for (char ch : d.value("Threshold", std::string("10")))
            if (ch >= '0' && ch <= '9' && t.size() < 4) t.push_back(ch);
        o["Threshold"] = t.empty() ? std::string("0") : t;
        o["Measure"]   = id == ids::kItemThreshold ? intIn(d, "Measure", 0, 0, 1) : 0;
        char const* slot = id == ids::kItemThreshold ? "Item" : "Bucket";
        o[slot]          = slotItem(d.value(slot, ct::Json()));
    } else if (id == ids::kRedstoneLink) {
        auto f         = d.value("Frequency", ct::Json::array());
        o["Frequency"] = ct::Json::array({slotItem(f.is_array() && f.size() > 0 ? f[0] : ct::Json()), slotItem(f.is_array() && f.size() > 1 ? f[1] : ct::Json())});
        o["Inverted"]  = intIn(d, "Inverted", 0, 0, 1);
    } else if (id == ids::kPlayerCount) {
        o["Count"] = intIn(d, "Count", 1, 0, 20);
        o["Exact"] = intIn(d, "Exact", 0, 0, 1);
    }
    return out;
}

namespace {

/// Texts the native editor shows (the client mod has the lang files too, but summaries need
/// the same rules on both sides: the server sends them).
ct::Json editorData(Player& p, ct::Schedule const& s) {
    ct::Json entries = ct::Json::array();
    for (auto const& e : s.entries) {
        ct::Json cols = ct::Json::array();
        for (auto const& col : e.conditions) {
            ct::Json c = ct::Json::array();
            for (auto const& part : col) c.push_back(conditionSummary(p, part));
            cols.push_back(c);
        }
        entries.push_back({{"title", instructionName(p, e.instruction.id)}, {"summary", instructionSummary(p, e.instruction)}, {"conditions", cols}, {"supports", ct::supportsConditions(e.instruction.id)}});
    }
    // the type selectors of the native instruction / condition editors (Create's menu order)
    ct::Json instr = ct::Json::array(), conds = ct::Json::array();
    for (auto id : ct::instructionIds()) instr.push_back({{"id", std::string(id)}, {"name", instructionName(p, id)}});
    for (auto id : ct::conditionIds()) conds.push_back({{"id", std::string(id)}, {"name", conditionName(p, id)}});
    // Create's DestinationSuggestions: station names, nearest first
    std::vector<std::pair<double, std::string>> near;
    auto                                        f = p.getFeetPos();
    for (auto const& [key, st] : state().stations) {
        double dx = st.pos.x + .5 - f.x, dz = st.pos.z + .5 - f.z;
        double d  = st.pos.dim == int(p.getDimensionId()) ? dx * dx + dz * dz : 1e18;
        near.emplace_back(d, st.name);
    }
    std::sort(near.begin(), near.end());
    ct::Json stations = ct::Json::array();
    std::set<std::string> seen;
    for (auto const& [d, name] : near)
        if (seen.insert(name).second && stations.size() < 16) stations.push_back(name);
    return {{"schedule", s.toJson()}, {"texts", entries}, {"instructions", instr}, {"conditions", conds}, {"stations", stations}};
}

} // namespace

void openScheduleEditor(Player& player, int slot) { screens::open(player, "trains.schedule", {{"slot", slot}}); }

void registerScheduleScreens() {
    screens::registerType({
        .id     = "trains.schedule",
        .data   = [](Player& p, ct::Json const& ctx) -> std::optional<ct::Json> {
            auto s = readSchedule(p, ctx.value("slot", -1));
            if (!s) return std::nullopt;
            return editorData(p, *s);
        },
        .action = [](Player& p, ct::Json const& ctx, ct::Json const& action) {
            int  slot = ctx.value("slot", -1);
            auto s    = readSchedule(p, slot);
            if (!s) return;
            std::string op = action.value("op", std::string());
            int         e  = action.value("entry", -1);
            bool        valid = e >= 0 && e < int(s->entries.size());
            if (op == "add") s->entries.push_back(ct::Entry::makeDefault());
            else if (op == "remove" && valid) s->entries.erase(s->entries.begin() + e);
            else if (op == "up" && valid && e > 0) std::swap(s->entries[e], s->entries[e - 1]);
            else if (op == "down" && valid && e + 1 < int(s->entries.size())) std::swap(s->entries[e], s->entries[e + 1]);
            else if (op == "duplicate" && valid) s->entries.insert(s->entries.begin() + e + 1, s->entries[e]);
            else if (op == "loop") s->cyclic = !s->cyclic;
            else if (op == "reset") s->savedProgress = 0;
            else if (op == "skip" && !s->entries.empty()) s->savedProgress = (s->savedProgress + 1) % int(s->entries.size());
            else if (op == "add_condition" && valid) {
                int c = action.value("column", -1);
                auto& cols = s->entries[e].conditions;
                if (c >= 0 && c < int(cols.size())) cols[c].push_back(ct::defaultCondition(ids::kDelay));
                else cols.push_back({ct::defaultCondition(ids::kDelay)});
            } else if (op == "remove_condition" && valid) {
                int  c    = action.value("column", -1), i = action.value("index", -1);
                auto& cols = s->entries[e].conditions;
                if (c >= 0 && c < int(cols.size()) && i >= 0 && i < int(cols[c].size())) {
                    cols[c].erase(cols[c].begin() + i);
                    if (cols[c].empty()) cols.erase(cols.begin() + c);
                }
            } else if (op == "cycle_instruction" && valid) {
                auto const& list = ct::instructionIds();
                auto        it   = std::find_if(list.begin(), list.end(), [&](std::string_view v) { return v == s->entries[e].instruction.id; });
                size_t      next = it == list.end() ? 0 : (size_t(it - list.begin()) + 1) % list.size();
                s->entries[e].instruction = ct::defaultInstruction(list[next]);
                if (ct::supportsConditions(list[next]) && s->entries[e].conditions.empty())
                    s->entries[e].conditions.push_back({ct::defaultCondition(ids::kDelay)});
            } else if (op == "set_instruction" && valid) {
                // the native instruction editor (Create's ScheduleScreen#stopEditing)
                auto& entry = s->entries[e];
                auto  next  = sanitiseInstruction(ct::Part::fromJson(action.value("part", ct::Json::object())), entry.instruction);
                if (!next) return;
                bool changed = next->id != entry.instruction.id;
                entry.instruction = std::move(*next);
                if (changed && ct::supportsConditions(entry.instruction.id) && entry.conditions.empty())
                    entry.conditions.push_back({ct::defaultCondition(ids::kDelay)});
                log().info("trains: {} edited instruction {} of a schedule: {}", p.getRealName(), e + 1, instructionSummary(p, entry.instruction));
            } else if (op == "set_condition" && valid) {
                int   c = action.value("column", -1), i = action.value("index", -1);
                auto& cols = s->entries[e].conditions;
                if (c < 0 || c >= int(cols.size()) || i < 0 || i >= int(cols[c].size())) return;
                auto next = sanitiseCondition(ct::Part::fromJson(action.value("part", ct::Json::object())), cols[c][i]);
                if (!next) return;
                cols[c][i] = std::move(*next);
                log().info("trains: {} edited condition {}.{}.{} of a schedule: {}", p.getRealName(), e + 1, c + 1, i + 1, conditionSummary(p, cols[c][i]));
            } else if (op == "edit_instruction" && valid) {
                writeSchedule(p, slot, *s);
                instructionForm(p, slot, e);
                return;
            } else if (op == "edit_condition" && valid) {
                writeSchedule(p, slot, *s);
                conditionForm(p, slot, e, action.value("column", 0), action.value("index", 0));
                return;
            } else {
                return;
            }
            writeSchedule(p, slot, *s);
        },
        .form = [](Player& p, ct::Json const& ctx) { mainForm(p, ctx.value("slot", -1)); },
    });
}

bool handScheduleTo(Player& player, Train& t, int slot) {
    auto&     inv  = player.getInventory();
    ItemStack held = slot >= 0 && slot < inv.getContainerSize() ? inv.getItem(slot) : ItemStack{};
    auto      sch  = scheduleOf(held);
    if (t.runtime.hasSchedule) {
        if (sch) {
            actionBar(player, trFor(player, "create.schedule.remove_with_empty_hand", "Remove current Schedule with an Empty Hand"));
            return true;
        }
        if (!held.isNull()) return false;
        if (t.runtime.paused && !t.runtime.completed) {
            t.runtime.paused = false;
            actionBar(player, trFor(player, "create.schedule.continued", "Schedule resumed"));
            log().info("trains: schedule of train #{} '{}' resumed", t.id, t.name);
            return true;
        }
        bool autoS = t.runtime.isAutoSchedule;
        auto back  = t.runtime.discard();
        cancelNavigation(t);
        if (!autoS) {
            inv.setItem(slot, makeScheduleItem(back));
            player.sendInventory(false);
        }
        actionBar(player, autoS ? trFor(player, "create.schedule.auto_removed_from_train", "Auto-Schedule discarded")
                                : trFor(player, "create.schedule.removed_from_train", "Schedule retrieved from Train"));
        log().info("trains: schedule removed from train #{} '{}'", t.id, t.name);
        return true;
    }
    if (!sch) return false;
    if (sch->entries.empty()) {
        actionBar(player, trFor(player, "create.schedule.no_stops", "This Schedule does not have any Stops yet"));
        return true;
    }
    t.runtime.setSchedule(*sch, false);
    advancements::award(player, "conductor"); // Create's ScheduleItem#handScheduleTo
    if (!player.isCreative()) {
        inv.setItem(slot, ItemStack{});
        player.sendInventory(false);
    }
    actionBar(player, "§a" + trFor(player, "create.schedule.applied_to_train", "Train is now following this Schedule"));
    log().info("trains: schedule with {} entries applied to train #{} '{}'", sch->entries.size(), t.id, t.name);
    return true;
}

} // namespace lv::feature::trains
