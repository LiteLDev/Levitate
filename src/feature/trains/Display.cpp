// Trains unit: the train station's display sources (U24's Display.h): Create's "Train Station
// Summary" (StationSummaryDisplaySource over GlobalTrainDisplayData: every upcoming destination of
// every running schedule whose name matches the filter, with its ETA) and "Train Schedule Status"
// (TrainStatusDisplaySource: what the train standing at the station waits for).

#include <algorithm>
#include <cmath>

#include "feature/advancements/Advancements.h"
#include "feature/display/Display.h"
#include "feature/trains/Trains.h"
#include "mod/I18n.h"
#include "mod/Module.h"

namespace lv::feature::trains {

namespace dsp = lv::feature::display;

namespace {

/// Create's GlobalTrainDisplayData.prepare: predictions whose destination matches the filter,
/// soonest first ("now" counts as 0, unknown last), then by train name, at most `maxRows`.
std::vector<SummaryRow> summaryRows(std::string const& filter, int maxRows) {
    struct P {
        int         ticks;
        std::string train, title, destination;
    };
    std::vector<P> all;
    for (auto& [id, t] : state().trains)
        for (auto const& p : predictionsOf(t))
            if (ct::stationMatches(filter, p.destination)) all.push_back({p.ticks, t.name, p.title, p.destination});
    auto compareTicks = [](int ticks) { return ticks == -1 ? 1 << 30 : ticks < 200 ? 0 : ticks; };
    std::stable_sort(all.begin(), all.end(), [&](P const& a, P const& b) {
        int ta = compareTicks(a.ticks), tb = compareTicks(b.ticks);
        return ta != tb ? ta < tb : a.train < b.train;
    });
    bool                    hasPlatform = filter.find('*') != std::string::npos;
    std::vector<SummaryRow> rows;
    for (auto const& p : all) {
        if (int(rows.size()) >= std::max(1, maxRows)) break;
        std::string platform;
        if (hasPlatform) {
            // the destination without the filter's literal parts, '*' as '?', trimmed
            platform = p.destination;
            std::size_t start = 0;
            while (start <= filter.size()) {
                auto        star = filter.find('*', start);
                std::string part = filter.substr(start, star == std::string::npos ? std::string::npos : star - start);
                if (!part.empty())
                    for (auto at = platform.find(part); at != std::string::npos; at = platform.find(part, at)) platform.erase(at, part.size());
                if (star == std::string::npos) break;
                start = star + 1;
            }
            std::replace(platform.begin(), platform.end(), '*', '?');
            auto b = platform.find_first_not_of(' ');
            platform = b == std::string::npos ? std::string() : platform.substr(b, platform.find_last_not_of(' ') - b + 1);
        }
        rows.push_back({p.ticks, p.train, p.title, platform});
    }
    return rows;
}

/// Create's time columns: (minutes, unit) — " ~ ", "now", "3 min", "45s".
std::pair<std::string, std::string> etaColumns(int ticks) {
    if (ticks == -1 || ticks >= 12000 - 15 * 20) return {" ", " ~ "};
    if (ticks < 200) return {" ", tr("create.display_source.station_summary.now", "now")};
    int min = ticks / 1200;
    int sec = int(std::ceil(((ticks / 20) % 60) / 15.0f)) * 15;
    if (sec == 60) {
        ++min;
        sec = 0;
    }
    if (min > 0) return {std::to_string(min), tr("create.display_source.station_summary.minutes", " min")};
    return {" ", tr("create.display_source.station_summary.seconds", "{0}s", sec)};
}

std::string fit(std::string s, int width, bool right = false) {
    width = std::max(0, width);
    if (int(s.size()) > width) s.resize(size_t(width));
    std::string pad(size_t(width - int(s.size())), ' ');
    return right ? pad + s : s + pad;
}

/// Create's StationSummaryDisplaySource.loadFlapDisplayLayout in characters: the train name gets
/// `NameColumn` % of the free width, the platform `PlatformColumn` characters.
struct Widths {
    int name = 0, destination = 0, platform = 0;
};
Widths layoutWidths(int boardWidthBlocks, int nameColumn, int platformColumn, bool hasPlatform) {
    constexpr float kMonospace = 7, kGap = 8, kTime = 20;
    float total         = boardWidthBlocks * 32.0f - 4 - kGap * 2;
    total               = total - kTime - kMonospace;
    float platformWidth = std::min(platformColumn * kMonospace, total - kGap);
    platformWidth       = std::floor(platformWidth / kMonospace) * kMonospace;
    if (hasPlatform) total = total - kGap - platformWidth;
    if (platformWidth == 0 && hasPlatform) total += kGap;
    Widths w;
    w.name        = int((nameColumn / 100.0f) * total / kMonospace);
    w.destination = int(std::round((1 - nameColumn / 100.0f) * total / kMonospace));
    w.platform    = hasPlatform ? int(platformWidth / kMonospace) : 0;
    return w;
}

std::vector<std::string> percentChoices(int max, bool percent) {
    std::vector<std::string> out;
    for (int i = 0; i <= max; ++i) out.push_back(std::to_string(i) + (percent ? "%" : ""));
    return out;
}

} // namespace

std::vector<SummaryRow> stationSummaryRows(std::string const& filter, int maxRows) { return summaryRows(filter, maxRows); }

class TrainsDisplayModule {
public:
    void install() {
        dsp::Source summary;
        summary.id           = "station_summary";
        summary.name         = "create.display_source.station_summary";
        summary.nameFallback = "Train Station Summary";
        summary.multiLine    = true;
        summary.refreshTicks = 20;
        auto names           = percentChoices(73, true);
        auto platforms       = percentChoices(16, false);
        summary.options      = {
            {"Filter", "create.display_source.station_summary.filter", "Station name filter", {}, {}, ""},
            {"NameColumn", "create.display_source.station_summary.train_name_column", "Train column size", names, names, 50},
            {"PlatformColumn", "create.display_source.station_summary.platform_column", "Platform column size", platforms, platforms, 3},
        };
        summary.onSelected = [](dsp::Context& ctx) {
            // Create's populateData: the filter starts as the station's own name
            if (ctx.config.contains("Filter") && !ctx.config["Filter"].get<std::string>().empty()) return;
            if (auto* self = stationAt(ctx.source)) ctx.config["Filter"] = self->name;
        };
        // Create: provideText is EMPTY, the summary only shows on display boards
        summary.text      = [](dsp::Context&, dsp::TargetStats const&) { return dsp::Lines{}; };
        summary.boardText = [](dsp::Context& ctx, dsp::TargetStats const& stats) -> std::optional<std::vector<dsp::Lines>> {
            std::string filter = ctx.config.value("Filter", std::string());
            if (filter.empty())
                if (auto* self = stationAt(ctx.source)) filter = self->name;
            bool hasPlatform = filter.find('*') != std::string::npos;
            auto rows        = summaryRows(filter, stats.maxRows);
            // Create's StationSummaryDisplaySource#provideFlapDisplayText: a train listed on a board
            if (!rows.empty()) advancements::awardOwner(ctx.link, "display_board_0");
            int  nameColumn = std::clamp(ctx.config.value("NameColumn", 50), 0, 73);
            int  platformCol = std::clamp(ctx.config.value("PlatformColumn", 3), 0, 16);
            // the board's width in blocks from its character count (Create: 16·w − 2 / 3.5 per row)
            int  blocks = std::max(1, int(std::round((stats.maxColumns * 3.5 + 2) / 16)));
            auto w      = layoutWidths(blocks, nameColumn, platformCol, hasPlatform);
            std::vector<dsp::Lines> out;
            for (auto const& r : rows) {
                auto [minutes, unit] = etaColumns(r.ticks);
                std::string line     = fit(minutes, 1, true) + fit(unit, 3, true) + " " + fit(r.train, w.name) + " " + fit(r.title, w.destination);
                if (hasPlatform) line += fit(r.platform, w.platform, true);
                out.push_back({line});
            }
            while (int(out.size()) < std::max(1, stats.maxRows)) out.push_back({"     . "});
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
            return dsp::Lines{t ? waitingStatusText(*t) : std::string()};
        };
        dsp::registerSource(std::string(kStation), status);
    }
    void uninstall() {}
};

} // namespace lv::feature::trains

LV_REGISTER_MODULE(lv::feature::trains::TrainsDisplayModule);
