// Trains unit: the train station's display sources (U24's Display.h): Create's "Train Station
// Summary" (trains heading to stations matching a filter, with their ETA) and "Train Schedule
// Status" (what the train waiting at the station is waiting for).

#include <algorithm>

#include "feature/display/Display.h"
#include "feature/trains/Trains.h"
#include "mod/I18n.h"
#include "mod/Module.h"

namespace lv::feature::trains {

namespace dsp = lv::feature::display;

namespace {

std::string etaText(int ticks) {
    auto e = ct::etaOf(ticks);
    switch (e.kind) {
    case ct::EtaText::Kind::Now:
        return tr("create.display_source.station_summary.now", "now");
    case ct::EtaText::Kind::Minutes:
        return std::to_string(e.value) + tr("create.display_source.station_summary.minutes", " min");
    case ct::EtaText::Kind::Seconds:
        return tr("create.display_source.station_summary.seconds", "{0}s", e.value);
    default:
        return " ~ ";
    }
}

struct Row {
    int         ticks;
    std::string train, title, platform;
};

std::vector<Row> summaryRows(dsp::Context& ctx) {
    auto* self   = stationAt(ctx.source);
    std::string filter = ctx.config.value("Filter", std::string());
    if (filter.empty() && self) filter = self->name;
    std::vector<Row> rows;
    for (auto& [key, s] : state().stations) {
        if (!ct::stationMatches(filter, s.name)) continue;
        // Create: the platform column shows the destination with the filter's literal parts removed
        std::string platform;
        if (filter.find('*') != std::string::npos) {
            platform = s.name;
            std::string lit;
            for (char c : filter) {
                if (c == '*') {
                    if (!lit.empty())
                        if (auto p = platform.find(lit); p != std::string::npos) platform.erase(p, lit.size());
                    lit.clear();
                } else {
                    lit += c;
                }
            }
            if (!lit.empty())
                if (auto p = platform.find(lit); p != std::string::npos) platform.erase(p, lit.size());
        }
        for (auto& [id, t] : state().trains) {
            if (t.invalid || !t.runtime.hasSchedule || t.runtime.paused) continue;
            int ticks = -2;
            if (t.currentStation == key) ticks = std::max(0, t.runtime.remainingWaitTicks());
            else if (t.nav && t.nav->stationPos == key) ticks = ct::predictTransitTicks(t.nav->distance, t.stats(), t.throttle);
            if (ticks == -2) continue;
            rows.push_back({ticks, t.name, t.runtime.title(), platform});
        }
    }
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) {
        int ta = a.ticks < 0 ? 1 << 30 : a.ticks, tb = b.ticks < 0 ? 1 << 30 : b.ticks;
        return ta != tb ? ta < tb : a.train < b.train;
    });
    return rows;
}

} // namespace

class TrainsDisplayModule {
public:
    void install() {
        dsp::Source summary;
        summary.id           = "station_summary";
        summary.name         = "create.display_source.station_summary";
        summary.nameFallback = "Train Station Summary";
        summary.multiLine    = true;
        summary.refreshTicks = 20;
        summary.options      = {{"Filter", "create.display_source.station_summary.filter", "Station name filter", {}, {}, ""}};
        summary.text         = [](dsp::Context& ctx, dsp::TargetStats const& stats) {
            dsp::Lines out;
            for (auto const& r : summaryRows(ctx)) {
                std::string line = etaText(r.ticks) + "  " + r.train;
                if (!r.title.empty()) line += "  " + r.title;
                if (!r.platform.empty()) line += "  " + r.platform;
                out.push_back(line);
                if (int(out.size()) >= std::max(1, stats.maxRows)) break;
            }
            while (int(out.size()) < std::max(1, stats.maxRows)) out.push_back(" . ");
            return out;
        };
        summary.boardText = [](dsp::Context& ctx, dsp::TargetStats const& stats) -> std::optional<std::vector<dsp::Lines>> {
            std::vector<dsp::Lines> out;
            for (auto const& r : summaryRows(ctx)) {
                out.push_back({etaText(r.ticks), r.train, r.title, r.platform});
                if (int(out.size()) >= std::max(1, stats.maxRows)) break;
            }
            while (int(out.size()) < std::max(1, stats.maxRows)) out.push_back({" . "});
            return out;
        };
        dsp::registerSource(std::string(kStation), summary);

        dsp::Source status;
        status.id           = "train_status";
        status.name         = "create.display_source.train_status";
        status.nameFallback = "Train Schedule Status";
        status.refreshTicks = 20;
        status.text         = [](dsp::Context& ctx, dsp::TargetStats const&) {
            auto* s = stationAt(ctx.source);
            auto* t = s ? trainAt(*s) : nullptr;
            if (!t || !t->runtime.hasSchedule || t->runtime.state != ct::RuntimeState::PostTransit) return dsp::Lines{""};
            int left = t->runtime.remainingWaitTicks();
            if (left < 0) return dsp::Lines{""};
            int         seconds = (left + 19) / 20;
            std::string when    = seconds >= 60 ? std::to_string(seconds / 60) + " " + tr("create.generic.unit.minutes", "Minutes")
                                                : std::to_string(((seconds + 4) / 5) * 5) + " " + tr("create.generic.unit.seconds", "Seconds");
            return dsp::Lines{tr("create.schedule.condition.delay.status", "Departs in {0}", when)};
        };
        dsp::registerSource(std::string(kStation), status);
    }
    void uninstall() {}
};

} // namespace lv::feature::trains

LV_REGISTER_MODULE(lv::feature::trains::TrainsDisplayModule);
