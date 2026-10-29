#include "core/trains/Schedule.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lv::core::trains {

namespace {

int intOf(Json const& data, char const* key, int fallback) {
    auto it = data.find(key);
    if (it == data.end()) return fallback;
    if (it->is_number()) return it->get<int>();
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string strOf(Json const& data, char const* key) {
    auto it = data.find(key);
    return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// Rotation periods of the time of day condition (Create's index order).
constexpr int kRotations[] = {24000, 12000, 6000, 4000, 3000, 2000, 1000, 750, 500, 250};

} // namespace

std::vector<std::string_view> const& instructionIds() {
    static std::vector<std::string_view> v{
        ids::kDestination,
        ids::kPackageDelivery,
        ids::kPackageRetrieval,
        ids::kRename,
        ids::kThrottle
    };
    return v;
}

std::vector<std::string_view> const& conditionIds() {
    static std::vector<std::string_view> v{
        ids::kDelay,
        ids::kTimeOfDay,
        ids::kFluidThreshold,
        ids::kItemThreshold,
        ids::kRedstoneLink,
        ids::kPlayerCount,
        ids::kIdle,
        ids::kUnloaded,
        ids::kPowered
    };
    return v;
}

std::string pathOf(std::string_view id) {
    auto colon = id.find(':');
    return std::string(colon == std::string_view::npos ? id : id.substr(colon + 1));
}

std::string normaliseId(std::string_view id) { return "levitate:" + pathOf(id); }

Part Part::fromJson(Json const& j) {
    Part p;
    if (!j.is_object()) return p;
    p.id   = normaliseId(j.value("Id", std::string()));
    p.data = j.value("Data", Json::object());
    if (!p.data.is_object()) p.data = Json::object();
    return p;
}

Part defaultInstruction(std::string_view id) {
    Part p{normaliseId(id), Json::object()};
    if (p.id == ids::kDestination || p.id == ids::kRename || p.id == ids::kPackageRetrieval) p.data["Text"] = "";
    if (p.id == ids::kThrottle) p.data["Value"] = 100;
    return p;
}

Part defaultCondition(std::string_view id) {
    Part p{normaliseId(id), Json::object()};
    if (p.id == ids::kDelay || p.id == ids::kIdle) {
        p.data["Value"]    = 5;
        p.data["TimeUnit"] = 1;
    } else if (p.id == ids::kTimeOfDay) {
        p.data["Hour"]     = 8;
        p.data["Minute"]   = 0;
        p.data["Rotation"] = 5;
    } else if (p.id == ids::kItemThreshold || p.id == ids::kFluidThreshold) {
        p.data["Operator"]  = 0;
        p.data["Threshold"] = "10";
        p.data["Measure"]   = 0;
        p.data[p.id == ids::kItemThreshold ? "Item" : "Bucket"] = nullptr;
    } else if (p.id == ids::kRedstoneLink) {
        p.data["Frequency"] = Json::array({nullptr, nullptr});
        p.data["Inverted"]  = 0;
    } else if (p.id == ids::kPlayerCount) {
        p.data["Count"] = 1;
        p.data["Exact"] = 0;
    }
    return p;
}

bool supportsConditions(std::string_view instructionId) {
    auto id = normaliseId(instructionId);
    return id != ids::kRename && id != ids::kThrottle;
}

Entry Entry::makeDefault() {
    Entry e;
    e.instruction = defaultInstruction(ids::kDestination);
    e.conditions.push_back({defaultCondition(ids::kDelay)});
    return e;
}

Json Schedule::toJson() const {
    Json entriesJ = Json::array();
    for (auto const& e : entries) {
        Json columns = Json::array();
        for (auto const& col : e.conditions) {
            Json c = Json::array();
            for (auto const& p : col) c.push_back(p.toJson());
            columns.push_back(std::move(c));
        }
        entriesJ.push_back({{"Instruction", e.instruction.toJson()}, {"Conditions", columns}});
    }
    return {{"Entries", entriesJ}, {"Cyclic", cyclic}, {"Progress", savedProgress}};
}

Schedule Schedule::fromJson(Json const& j) {
    Schedule s;
    if (!j.is_object()) return s;
    s.cyclic        = j.value("Cyclic", true);
    s.savedProgress = j.value("Progress", 0);
    for (auto const& ej : j.value("Entries", Json::array())) {
        if (!ej.is_object()) continue;
        Entry e;
        e.instruction = Part::fromJson(ej.value("Instruction", Json::object()));
        if (e.instruction.id.empty() || e.instruction.id == "levitate:") continue;
        for (auto const& cj : ej.value("Conditions", Json::array())) {
            if (!cj.is_array()) continue;
            std::vector<Part> col;
            for (auto const& pj : cj) {
                auto p = Part::fromJson(pj);
                if (!p.id.empty() && p.id != "levitate:") col.push_back(std::move(p));
            }
            if (!col.empty()) e.conditions.push_back(std::move(col));
        }
        s.entries.push_back(std::move(e));
    }
    s.savedProgress = std::clamp(s.savedProgress, 0, std::max(0, int(s.entries.size()) - 1));
    return s;
}

// ---- conditions ---------------------------------------------------------------------------------

int waitTicks(Json const& data) {
    int value = std::max(0, intOf(data, "Value", 5));
    int unit  = intOf(data, "TimeUnit", 1);
    return value * (unit == 0 ? 1 : unit == 2 ? 1200 : 20);
}

int timeOfDayTarget(Json const& data) {
    int hour   = std::clamp(intOf(data, "Hour", 8), 0, 23);
    int minute = std::clamp(intOf(data, "Minute", 0), 0, 59);
    return ((hour + 18) % 24) * 1000 + int(std::ceil(minute / 60.0 * 1000));
}

int timeOfDayRotation(Json const& data) {
    int r = std::clamp(intOf(data, "Rotation", 5), 0, int(std::size(kRotations)) - 1);
    return kRotations[r];
}

bool timeOfDayReached(Json const& data, std::int64_t dayTime) {
    int  rot    = timeOfDayRotation(data);
    int  target = timeOfDayTarget(data);
    long long now  = ((dayTime % 24000) + 24000) % 24000;
    long long diff = now % rot - target % rot;
    return diff >= 0 && diff <= 40;
}

Clock clockOf(Json const& data) {
    int hour   = std::clamp(intOf(data, "Hour", 8), 0, 23);
    int minute = std::clamp(intOf(data, "Minute", 0), 0, 59);
    Clock c;
    c.pm     = hour >= 12;
    c.hour   = hour % 12 == 0 ? 12 : hour % 12;
    c.minute = minute;
    return c;
}

bool compareThreshold(int op, long long have, long long threshold) {
    switch (op) {
    case 1:
        return have < threshold;
    case 2:
        return have == threshold;
    default:
        return have > threshold;
    }
}

long long thresholdOf(Json const& data) {
    auto it = data.find("Threshold");
    if (it == data.end()) return 0;
    if (it->is_number()) return it->get<long long>();
    if (!it->is_string()) return 0;
    try {
        return std::stoll(it->get<std::string>());
    } catch (...) {
        return 0;
    }
}

bool stationMatches(std::string_view filter, std::string_view name) {
    // Create builds a regex from the filter: '*' -> ".*", the rest quoted; matches the whole name
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < name.size()) {
        if (p < filter.size() && filter[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < filter.size() && filter[p] == name[t]) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < filter.size() && filter[p] == '*') ++p;
    return p == filter.size();
}

std::string filterTitle(std::string_view filter) {
    std::string s;
    for (char c : filter)
        if (c != '*') s += c;
    auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}

bool tickCondition(Part const& c, Json& ctx, Driver const& d) {
    auto const& data = c.data;
    if (c.id == ids::kDelay) {
        int time = ctx.value("Time", 0);
        if (time >= waitTicks(data)) return true;
        ctx["Time"] = time + 1;
        return false;
    }
    if (c.id == ids::kIdle) return d.idleTicks() > waitTicks(data);
    if (c.id == ids::kTimeOfDay) return timeOfDayReached(data, d.dayTime());
    if (c.id == ids::kItemThreshold) {
        long long have = d.itemCount(data.value("Item", Json()), intOf(data, "Measure", 0) == 1);
        return compareThreshold(intOf(data, "Operator", 0), have, thresholdOf(data));
    }
    if (c.id == ids::kFluidThreshold) {
        long long have = d.fluidAmount(data.value("Bucket", Json()));
        return compareThreshold(intOf(data, "Operator", 0), have, thresholdOf(data) * 1000);
    }
    if (c.id == ids::kRedstoneLink) {
        bool powered = d.linkPowered(data.value("Frequency", Json()));
        return powered != (intOf(data, "Inverted", 0) != 0);
    }
    if (c.id == ids::kPlayerCount) {
        int want = intOf(data, "Count", 1), have = d.seatedPlayers();
        return intOf(data, "Exact", 0) == 0 ? have == want : have >= want;
    }
    if (c.id == ids::kUnloaded) return d.stationUnloaded();
    if (c.id == ids::kPowered) return d.stationPowered();
    return true; // unknown condition: don't hold the train forever
}

StatusText waitingStatus(Part const& c, Json const& ctx, Driver const& d) {
    auto const& data = c.data;
    auto durationText = [](int ticks) -> std::string {
        int seconds = (ticks + 19) / 20;
        if (seconds >= 60) return std::to_string(seconds / 60) + "m";
        seconds = ((seconds + 4) / 5) * 5; // Create rounds up to 5 s steps
        return std::to_string(seconds) + "s";
    };
    if (c.id == ids::kDelay) {
        int left = std::max(0, waitTicks(data) - ctx.value("Time", 0));
        return {"create.schedule.condition.delay.status", "Departs in {0}", {durationText(left)}};
    }
    if (c.id == ids::kIdle)
        return {"create.schedule.condition.idle.status", "Cargo Idle for {0}", {durationText(d.idleTicks())}};
    if (c.id == ids::kTimeOfDay) {
        auto clock = clockOf(data);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", clock.hour, clock.minute, clock.pm ? "pm" : "am");
        return {"create.schedule.condition.time_of_day.status", "Departs at {0}", {buf}};
    }
    if (c.id == ids::kItemThreshold || c.id == ids::kFluidThreshold) {
        bool      items = c.id == ids::kItemThreshold;
        long long have  = items ? d.itemCount(data.value("Item", Json()), intOf(data, "Measure", 0) == 1)
                                : d.fluidAmount(data.value("Bucket", Json())) / 1000;
        long long target = thresholdOf(data);
        int       op     = intOf(data, "Operator", 0);
        if (op == 0) target += 1;
        else if (op == 1) target -= 1;
        std::string unit = items ? (intOf(data, "Measure", 0) == 1 ? "stacks" : "items") : "B";
        return {"create.schedule.condition.threshold.status", "Cargo: {0}/{1} {2}", {std::to_string(have), std::to_string(target), unit}};
    }
    if (c.id == ids::kPlayerCount)
        return {"create.schedule.condition.player_count.status",
                "Passengers: {0}/{1}",
                {std::to_string(d.seatedPlayers()), std::to_string(intOf(data, "Count", 1))}};
    if (c.id == ids::kRedstoneLink) return {"create.schedule.condition.redstone_link.status", "Waiting for redstone link", {}};
    if (c.id == ids::kUnloaded) return {"create.schedule.condition.unloaded.status", "Waiting for chunk unload", {}};
    if (c.id == ids::kPowered) return {"create.schedule.condition.powered.status", "Waiting for redstone", {}};
    return {};
}

EtaText etaOf(int ticks) {
    EtaText t;
    if (ticks < 0 || ticks >= 11700) return t;
    if (ticks < 200) {
        t.kind = EtaText::Kind::Now;
        return t;
    }
    int minutes = ticks / 1200;
    int seconds = int(std::ceil(((ticks / 20) % 60) / 15.0)) * 15;
    if (seconds == 60) {
        ++minutes;
        seconds = 0;
    }
    if (minutes > 0) {
        t.kind  = EtaText::Kind::Minutes;
        t.value = minutes;
    } else {
        t.kind  = EtaText::Kind::Seconds;
        t.value = seconds;
    }
    return t;
}

// ---- runtime ------------------------------------------------------------------------------------

void Runtime::setSchedule(Schedule s, bool autoSchedule) {
    hasSchedule    = true;
    schedule       = std::move(s);
    isAutoSchedule = autoSchedule;
    currentEntry   = std::clamp(schedule.savedProgress, 0, std::max(0, int(schedule.entries.size()) - 1));
    state          = RuntimeState::PreTransit;
    paused         = false;
    completed      = false;
    cooldown       = 0;
    ticksInTransit = 0;
    currentTitle.clear();
    conditionProgress.clear();
    conditionContext.clear();
    predictionTicks.assign(schedule.entries.size(), -1);
}

Schedule Runtime::discard() {
    Schedule s      = schedule;
    s.savedProgress = currentEntry;
    hasSchedule     = false;
    schedule        = {};
    isAutoSchedule  = false;
    currentEntry    = 0;
    state           = RuntimeState::PreTransit;
    paused          = false;
    completed       = false;
    conditionProgress.clear();
    conditionContext.clear();
    currentTitle.clear();
    return s;
}

Entry const* Runtime::entry() const {
    if (!hasSchedule || currentEntry < 0 || currentEntry >= int(schedule.entries.size())) return nullptr;
    return &schedule.entries[currentEntry];
}

void Runtime::tick(Driver& driver) {
    if (!hasSchedule || paused || driver.derailed()) return;
    if (driver.navigating()) {
        ++ticksInTransit;
        return;
    }
    if (currentEntry >= int(schedule.entries.size())) {
        currentEntry = 0;
        if (!schedule.cyclic) {
            paused    = true;
            completed = true;
            return;
        }
    }
    if (schedule.entries.empty()) return;
    if (cooldown-- > 0) return;
    cooldown = 0;
    if (state == RuntimeState::InTransit) return;
    if (state == RuntimeState::PostTransit) {
        tickConditions(driver);
        return;
    }
    startInstruction(driver);
}

void Runtime::startInstruction(Driver& driver) {
    auto const& instr = schedule.entries[currentEntry].instruction;
    if (instr.id == ids::kRename) {
        currentTitle = instr.data.value("Text", std::string());
        driver.rename(currentTitle);
        ++currentEntry;
        return;
    }
    if (instr.id == ids::kThrottle) {
        int pct = std::clamp(intOf(instr.data, "Value", 100), 5, 100);
        driver.setThrottle(pct / 100.0);
        ++currentEntry;
        return;
    }
    StartResult r;
    std::string filter = strOf(instr.data, "Text");
    if (instr.id == ids::kDestination) r = driver.startDestination(filter);
    else if (instr.id == ids::kPackageDelivery) r = driver.startPackages(true, filter);
    else if (instr.id == ids::kPackageRetrieval) r = driver.startPackages(false, filter);
    else {
        ++currentEntry; // unknown instruction: skip it
        return;
    }
    switch (r) {
    case StartResult::NoMatch:
        driver.status("no_match", {filter}, false);
        break;
    case StartResult::NoPath:
        driver.status("no_path", {}, false);
        break;
    case StartResult::NoTarget:
        driver.status("no_package_target", {}, false);
        break;
    case StartResult::NoDriver:
        break; // the driver reported missing / opposite driver itself
    case StartResult::AlreadyThere:
    case StartResult::Navigating:
        if (failedLast) driver.status("navigation_success", {}, true);
        failedLast     = false;
        state          = RuntimeState::InTransit;
        ticksInTransit = 0;
        if (r == StartResult::AlreadyThere) destinationReached();
        return;
    }
    failedLast = true;
    cooldown   = kInterval;
}

void Runtime::tickConditions(Driver& driver) {
    auto const& e = schedule.entries[currentEntry];
    if (!supportsConditions(e.instruction.id) || e.conditions.empty()) {
        state = RuntimeState::PreTransit;
        ++currentEntry;
        return;
    }
    conditionProgress.resize(e.conditions.size(), 0);
    conditionContext.resize(e.conditions.size(), Json::object());
    for (size_t i = 0; i < e.conditions.size(); ++i) {
        auto const& column = e.conditions[i];
        int         p      = conditionProgress[i];
        if (p >= int(column.size())) {
            state = RuntimeState::PreTransit;
            ++currentEntry;
            return;
        }
        if (tickCondition(column[p], conditionContext[i], driver)) {
            conditionContext[i] = Json::object();
            ++conditionProgress[i];
        }
    }
}

void Runtime::destinationReached() {
    if (state != RuntimeState::InTransit) return;
    state = RuntimeState::PostTransit;
    if (currentEntry >= 0 && currentEntry < int(predictionTicks.size())) {
        int& learned = predictionTicks[currentEntry];
        learned      = learned < 0 ? ticksInTransit : (learned + ticksInTransit) / 2;
    }
    conditionProgress.clear();
    conditionContext.clear();
    if (auto const* e = entry()) {
        conditionProgress.assign(e->conditions.size(), 0);
        conditionContext.assign(e->conditions.size(), Json::object());
    }
}

void Runtime::transitInterrupted() {
    if (state != RuntimeState::InTransit) return;
    state    = RuntimeState::PreTransit;
    cooldown = 0;
}

std::string Runtime::title() const {
    if (!currentTitle.empty()) return currentTitle;
    if (!hasSchedule) return {};
    for (size_t k = 0; k < schedule.entries.size(); ++k) {
        auto const& e = schedule.entries[(currentEntry + k) % schedule.entries.size()];
        if (e.instruction.id == ids::kDestination) return filterTitle(strOf(e.instruction.data, "Text"));
    }
    return {};
}

int Runtime::remainingWaitTicks() const {
    auto const* e = entry();
    if (!e || e->conditions.empty()) return -1;
    auto const& column = e->conditions[0];
    int         total  = 0;
    for (auto const& c : column) {
        if (c.id != ids::kDelay) return -1;
        total += waitTicks(c.data);
    }
    if (state == RuntimeState::PostTransit && !conditionContext.empty() && !conditionProgress.empty()) {
        int done = 0;
        for (int i = 0; i < conditionProgress[0] && i < int(column.size()); ++i) done += waitTicks(column[i].data);
        done += conditionContext[0].value("Time", 0);
        total -= done;
    }
    return std::max(0, total);
}

Json Runtime::toJson() const {
    Json j;
    j["has"]       = hasSchedule;
    j["schedule"]  = schedule.toJson();
    j["auto"]      = isAutoSchedule;
    j["entry"]     = currentEntry;
    j["state"]     = int(state);
    j["paused"]    = paused;
    j["completed"] = completed;
    j["cooldown"]  = cooldown;
    j["transit"]   = ticksInTransit;
    j["progress"]  = conditionProgress;
    j["context"]   = conditionContext;
    j["title"]     = currentTitle;
    j["predict"]   = predictionTicks;
    return j;
}

Runtime Runtime::fromJson(Json const& j) {
    Runtime r;
    if (!j.is_object()) return r;
    r.hasSchedule    = j.value("has", false);
    r.schedule       = Schedule::fromJson(j.value("schedule", Json::object()));
    r.isAutoSchedule = j.value("auto", false);
    r.currentEntry   = j.value("entry", 0);
    r.state          = RuntimeState(std::clamp(j.value("state", 0), 0, 2));
    r.paused         = j.value("paused", false);
    r.completed      = j.value("completed", false);
    r.cooldown       = j.value("cooldown", 0);
    r.ticksInTransit = j.value("transit", 0);
    r.conditionProgress = j.value("progress", std::vector<int>{});
    for (auto const& c : j.value("context", Json::array())) r.conditionContext.push_back(c.is_object() ? c : Json::object());
    r.currentTitle    = j.value("title", std::string());
    r.predictionTicks = j.value("predict", std::vector<int>{});
    r.predictionTicks.resize(r.schedule.entries.size(), -1);
    return r;
}

} // namespace lv::core::trains
