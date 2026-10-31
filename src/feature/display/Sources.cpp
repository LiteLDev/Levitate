// Display unit (U24): the built-in display sources (Create's AllDisplaySources) for vanilla blocks
// and, by block id, for Create blocks owned by other units. Sources that need a unit's internal
// state (boiler, station, train observer, elevator contact, factory gauge, package address) are
// registered by those units through Display.h.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/scores/IdentityDefinition.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/Scoreboard.h"
#include "mc/world/scores/ScoreboardId.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/common/BlockData.h"
#include "feature/display/Internal.h"
#include "feature/packages/Packages.h"
#include "feature/steam/Steam.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Module.h"

namespace lv::feature::display {

namespace {
namespace items  = common::items;
namespace fluids = common::fluids;
namespace k      = lv::kinetics;

// ---- formatting -------------------------------------------------------------------------------

/// Create's CreateLang.number: up to two decimals, grouped thousands (en_US).
std::string number(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    auto        dot   = s.find('.');
    std::string whole = s.substr(0, dot), frac = dot == std::string::npos ? "" : s.substr(dot);
    bool        neg   = !whole.empty() && whole[0] == '-';
    if (neg) whole.erase(0, 1);
    std::string grouped;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        if (i && (whole.size() - i) % 3 == 0) grouped += ',';
        grouped += whole[i];
    }
    return (neg ? "-" : "") + grouped + frac;
}

bool onBoard(TargetStats const& stats) { return stats.targetId == "display_board"; }

std::string labelled(Context& ctx, std::string const& value) {
    auto label = ctx.config.value("Label", std::string{});
    return label.empty() ? value : label + " " + value;
}

int optionIndex(Context& ctx, char const* key) {
    auto it = ctx.config.find(key);
    return it != ctx.config.end() && it->is_number_integer() ? it->get<int>() : 0;
}

Option labelOption() {
    return {"Label", "create.display_source.label", "Attached Label", {}, {}, ""};
}

/// Percent / progress bar sources (Create's PercentOrProgressBarDisplaySource).
std::string percentOrBar(Context& ctx, TargetStats const& stats, double level, bool bar) {
    if (bar) {
        // Create's PercentOrProgressBarDisplaySource: the bar replaces the label; signs fit 6/9 of
        // the columns, display boards use wide flaps (MONOSPACE 7 / WIDE_MONOSPACE 9)
        int labelLen = static_cast<int>(ctx.config.value("Label", std::string{}).size());
        int width    = std::min(stats.maxColumns - (labelLen ? labelLen + 1 : 0), 128);
        if (stats.targetId == "sign") width = static_cast<int>(width * 6.0f / 9.0f);
        if (onBoard(stats)) width = static_cast<int>(width * 7.0f / 9.0f);
        if (width < 1) return "";
        return progressBar(level, width);
    }
    return labelled(ctx, std::to_string(std::clamp(static_cast<int>(level * 100), 0, 100)) + "%"); // Create: (int) truncation
}

std::string prettyId(std::string id) {
    if (auto c = id.find(':'); c != std::string::npos) id = id.substr(c + 1);
    bool up = true;
    for (auto& ch : id) {
        if (ch == '_') {
            ch = ' ';
            up = true;
        } else if (up) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            up = false;
        }
    }
    return id;
}

// ---- observed inventories (smart observer, threshold switch) ----------------------------------

/// The block a directional observer looks at: the face its `levitate:facing` (FacingID) state
/// points to (units may register their own sources with the same id to refine this).
std::optional<GlobalPos> observedPos(BlockSource& region, GlobalPos const& p) {
    auto const* b = loadedBlock(region, game::blockPos(p));
    if (!b) return std::nullopt;
    // U23's detectors: `levitate:target` 1 looks down, 2 up, else along `levitate:facing`
    int target = intState(*b, "levitate:target").value_or(0);
    int face   = target == 1 ? 0 : target == 2 ? 1 : intState(*b, "levitate:facing").value_or(intState(*b, "facing_direction").value_or(-1));
    if (face < 0 || face > 5) return std::nullopt;
    return offsetPos(p, face);
}

struct Counted {
    ItemStack sample;
    long long count = 0;
};

std::vector<Counted> countItems(Context& ctx) {
    std::vector<Counted> out;
    auto                 obs = observedPos(ctx.region, ctx.source);
    if (!obs) return out;
    common::Access at{ctx.region, game::blockPos(*obs), -1};
    auto           h = items::find(at);
    if (!h || !h->slots || !h->getSlot) return out;
    int n = h->slots(at);
    for (int i = 0; i < n; ++i) {
        auto s = h->getSlot(at, i);
        if (items::isEmpty(s) || !common::filterslot::matches(ctx.source, s)) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](Counted const& c) { return items::sameItemSameData(c.sample, s); });
        if (it == out.end()) out.push_back({items::withCount(s, 1), items::countOf(s)});
        else it->count += items::countOf(s);
    }
    return out;
}

std::vector<std::pair<std::string, long long>> countFluids(Context& ctx) {
    std::vector<std::pair<std::string, long long>> out;
    auto obs = observedPos(ctx.region, ctx.source);
    if (!obs) return out;
    auto filter = common::filterslot::get(ctx.source);
    for (auto const& t : fluids::tanks(ctx.region, game::blockPos(*obs), -1)) {
        if (t.contents.empty()) continue;
        if (!items::isEmpty(filter) && !common::filter::matchesFluid(filter, t.contents)) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](auto const& e) { return e.first == t.contents.fluid; });
        if (it == out.end()) out.emplace_back(t.contents.fluid, t.contents.amount);
        else it->second += t.contents.amount;
    }
    return out;
}

/// Create's FluidFormatter.asComponents: (amount, unit) — buckets with one decimal when shortened.
std::pair<std::string, std::string> fluidParts(long long mb, bool shortened) {
    if (shortened && mb >= 1000) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", mb / 1000.0);
        return {buf, tr("create.generic.unit.buckets", "B")};
    }
    return {std::to_string(mb), tr("create.generic.unit.millibuckets", "mB")};
}

/// Create's FluidFormatter.asString: "1000 mB", "1.5 B".
std::string fluidAmount(long long mb, bool shortened) {
    auto [v, unit] = fluidParts(mb, shortened);
    return v + " " + unit;
}

Option valueDisplayOption() {
    return {"ValueDisplay", "create.display_source.value_list.display", "Value Display",
            {"create.display_source.value_list.shortened", "create.display_source.value_list.full_number"},
            {"Shortened", "Full Number"}, 0};
}

/// Value list lines (Create's ValueListDisplaySource: "<value> <name>" when `valueFirst`, else
/// "<name> <value>"), largest first, as many as the target has rows (a lectern: 8 entries per
/// page, Create's ENTRIES_PER_PAGE).
Lines valueList(std::vector<std::pair<std::string, std::string>> entries, TargetStats const& stats, bool valueFirst = true) {
    Lines out;
    bool  book = stats.targetId == "lectern";
    int   rows = std::max(1, stats.maxRows) * (book ? 8 : 1);
    for (auto const& [value, name] : entries) {
        if (static_cast<int>(out.size()) >= rows) break;
        out.push_back(valueFirst ? value + " " + name : name + " " + value);
    }
    if (!book) return out;
    Lines pages; // condensePages: 8 entries per page
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i % 8 == 0) pages.emplace_back();
        else pages.back() += "\n";
        pages.back() += out[i];
    }
    return pages;
}

// ---- deaths (respawn anchor) ------------------------------------------------------------------
constexpr char kDbDeaths[] = "lv:display:deaths";
std::map<std::string, long long>& deaths() {
    static std::map<std::string, long long> m;
    return m;
}

// ---- scoreboard -------------------------------------------------------------------------------
Lines scoreboardLines(std::string const& objectiveName, TargetStats const& stats, Player const* viewer, bool shortened) {
    auto level = ll::service::getLevel();
    if (!level) return {};
    auto* board = level->tryGetScoreboard();
    if (!board) return {};
    auto* obj = board->getObjective(objectiveName);
    if (!obj) {
        auto msg = tr("create.display_source.scoreboard.objective_not_found", "'{}' not found", objectiveName);
        (void)viewer;
        return {msg};
    }
    static std::string nameBuf;
    auto resolver = [](ActorUniqueID id) -> std::string const& {
        nameBuf.clear();
        if (auto lv = ll::service::getLevel())
            if (auto* p = lv->getPlayer(id)) nameBuf = p->getRealName();
        return nameBuf;
    };
    std::vector<std::pair<long long, std::string>> scores;
    for (auto const& [id, value] : *obj->mScores) {
        IdentityDefinition const* def = id.mIdentityDef;
        if (!def) continue;
        scores.emplace_back(value, def->getName(resolver));
    }
    std::sort(scores.begin(), scores.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<std::pair<std::string, std::string>> entries;
    for (auto const& [v, n] : scores) entries.emplace_back(formatValue(v, shortened), n);
    return valueList(entries, stats, false); // Create: valueFirst() = false
}

// ---- enchanting table -------------------------------------------------------------------------
int enchantPower(BlockSource& region, GlobalPos const& p) {
    // Java's EnchantingTableBlock.BOOKSHELF_OFFSETS: the ring two blocks out, air in between
    int power = 0;
    for (int dx = -2; dx <= 2; ++dx) {
        for (int dz = -2; dz <= 2; ++dz) {
            if (std::abs(dx) != 2 && std::abs(dz) != 2) continue;
            for (int dy = 0; dy <= 1; ++dy) {
                BlockPos shelf{p.x + dx, p.y + dy, p.z + dz};
                BlockPos between{p.x + dx / 2, p.y + dy, p.z + dz / 2};
                auto const* b = loadedBlock(region, shelf);
                auto const* m = loadedBlock(region, between);
                if (b && m && b->getTypeName() == "minecraft:bookshelf" && (m->isAir() || game::replaceable(region, between))) ++power;
            }
        }
    }
    return std::min(power, 15);
}

Lines kineticSpeed(Context& ctx, TargetStats const& stats) {
    float speed = k::speedAt(ctx.source);
    if (optionIndex(ctx, "Directional") == 0) speed = std::fabs(speed);
    (void)stats;
    return {labelled(ctx, number(speed) + " " + tr("create.generic.unit.rpm", "RPM"))}; // Create: .space()
}

Lines kineticStress(Context& ctx, TargetStats const& stats) {
    auto net  = k::networkAt(ctx.source);
    int  mode = optionIndex(ctx, "Mode");
    double stress = net ? net->stress : 0, capacity = net ? net->capacity : 0;
    if (mode == 0 || mode == 1) return {percentOrBar(ctx, stats, capacity > 0 ? stress / capacity : 0, mode == 0)};
    double v    = mode == 2 ? stress : mode == 3 ? capacity : capacity - stress;
    if (capacity <= 0) v = 0;
    return {labelled(ctx, number(v) + (onBoard(stats) ? " " : "") + tr("create.generic.unit.stress", "su"))};
}

std::string twoDigits(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }

Lines timeOfDay(Context& ctx, TargetStats const&) {
    auto level = ll::service::getLevel();
    if (!level || k::speedAt(ctx.source) == 0) return {"--:--"};
    int dayTime = static_cast<int>(level->getTime() % 24000);
    int hours   = (dayTime / 1000 + 6) % 24;
    int minutes = (dayTime % 1000) * 60 / 1000;
    minutes     = minutes / 5 * 5;
    if (ctx.source.dim != 0) {
        // Create: non-natural dimensions show nonsense times
        static std::mt19937 rng{std::random_device{}()};
        hours   = static_cast<int>(rng() % 70) + 24;
        minutes = static_cast<int>(rng() % 40) + 60;
    }
    bool c12 = optionIndex(ctx, "Cycle") == 0;
    std::string suffix;
    if (c12) {
        suffix = " " + (hours > 11 ? tr("create.generic.daytime.pm", "pm") : tr("create.generic.daytime.am", "am"));
        hours %= 12;
        if (hours == 0) hours = 12;
    }
    return {labelled(ctx, (hours < 10 ? " " : "") + std::to_string(hours) + ":" + twoDigits(minutes) + suffix)}; // Create pads hours
}

Lines stopwatch(Context& ctx, TargetStats const&) {
    if (k::speedAt(ctx.source) == 0) return {"--:--"};
    if (!ctx.config.contains("StartTime")) ctx.config["StartTime"] = ctx.gameTime;
    long long diff    = static_cast<long long>(ctx.gameTime) - ctx.config.value("StartTime", 0LL);
    long long seconds = std::max(0LL, diff) / 20;
    long long hours = seconds / 3600, minutes = seconds / 60 % 60, secs = seconds % 60;
    std::string text;
    if (hours) text = std::to_string(hours) + ":";
    text += (minutes < 10 ? (hours ? "0" : " ") : "") + std::to_string(minutes) + ":";
    text += (secs < 10 ? "0" : "") + std::to_string(secs);
    return {labelled(ctx, text)};
}

struct Registration {
    Registration() {
        // ---- kinetics (U6's gauges) ----
        registerSource("levitate:speedometer", Source{
            .id = "kinetic_speed", .name = "create.display_source.kinetic_speed", .nameFallback = "Rotation Speed (RPM)",
            .text = kineticSpeed,
            .options = {{"Directional", "create.display_source.kinetic_speed.directional", "Include Direction",
                         {"create.display_source.kinetic_speed.absolute", "create.display_source.kinetic_speed.directional"},
                         {"Ignore Direction", "Include Direction"}, 0},
                        labelOption()},
        });
        registerSource("levitate:stressometer", Source{
            .id = "kinetic_stress", .name = "create.display_source.kinetic_stress", .nameFallback = "Network Stress",
            .text = kineticStress,
            .options = {{"Mode", "create.display_source.kinetic_stress.display", "Displayed Info",
                         {"create.display_source.kinetic_stress.progress_bar", "create.display_source.kinetic_stress.percent",
                          "create.display_source.kinetic_stress.current", "create.display_source.kinetic_stress.max",
                          "create.display_source.kinetic_stress.remaining"},
                         {"Progress Bar", "Percentage", "Stress in SU", "Total Capacity", "Remaining SU"}, 0},
                        labelOption()},
        });

        // ---- cuckoo clock ----
        for (auto const* clock : {"levitate:cuckoo_clock", "levitate:mysterious_cuckoo_clock"}) {
            registerSource(clock, Source{
                .id = "time_of_day", .name = "create.display_source.time_of_day", .nameFallback = "Time of Day",
                .text = timeOfDay,
                .options = {{"Cycle", "create.display_source.time.format", "Time Format",
                             {"create.display_source.time.12_hour", "create.display_source.time.24_hour"}, {"12-hour", "24-hour"}, 0},
                            labelOption()},
            });
            registerSource(clock, Source{
                .id = "stopwatch", .name = "create.display_source.stop_watch", .nameFallback = "Stopwatch",
                .text = stopwatch, .options = {labelOption()}, .refreshTicks = 20,
                .onSignalReset = [](Context& ctx) { ctx.config["StartTime"] = ctx.gameTime; },
                .onSelected    = [](Context& ctx) { ctx.config["StartTime"] = ctx.gameTime; },
            });
        }

        // ---- smart observer (U23): contents of the observed inventory / tank ----
        registerSource("levitate:content_observer", Source{
            .id = "count_items", .name = "create.display_source.count_items", .nameFallback = "Amount of matching Items",
            .text = [](Context& ctx, TargetStats const&) -> Lines {
                long long total = 0;
                for (auto const& c : countItems(ctx)) total += c.count;
                return {labelled(ctx, std::to_string(total))}; // Create: String.valueOf
            },
            .options = {labelOption()},
        });
        registerSource("levitate:content_observer", Source{
            .id = "list_items", .name = "create.display_source.list_items", .nameFallback = "List matching Items",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                auto list = countItems(ctx);
                std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.count > b.count; });
                bool shortened = optionIndex(ctx, "ValueDisplay") == 0;
                std::vector<std::pair<std::string, std::string>> entries;
                for (auto const& c : list) entries.emplace_back(formatValue(c.count, shortened), items::displayName(c.sample));
                return valueList(entries, stats);
            },
            .options = {valueDisplayOption()}, .multiLine = true,
        });
        registerSource("levitate:content_observer", Source{
            .id = "count_fluids", .name = "create.display_source.fluid_amount", .nameFallback = "Amount of matching Fluids",
            .text = [](Context& ctx, TargetStats const&) -> Lines {
                long long total = 0;
                for (auto const& [f, a] : countFluids(ctx)) total += a;
                return {labelled(ctx, fluidAmount(total, false))};
            },
            .options = {labelOption()},
        });
        registerSource("levitate:content_observer", Source{
            .id = "list_fluids", .name = "create.display_source.list_fluids", .nameFallback = "List matching Fluids",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                auto list = countFluids(ctx);
                std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
                bool shortened = optionIndex(ctx, "ValueDisplay") == 0;
                std::vector<std::pair<std::string, std::string>> entries;
                for (auto const& [f, a] : list) {
                    auto [v, unit] = fluidParts(a, shortened);
                    entries.emplace_back(v + unit, prettyId(f)); // Create: value, unit, name
                }
                return valueList(entries, stats);
            },
            .options = {valueDisplayOption()}, .multiLine = true,
        });

        // ---- threshold switch (U23): fill level of the observed inventory ----
        registerSource("levitate:stockpile_switch", Source{
            .id = "fill_level", .name = "create.display_source.fill_level", .nameFallback = "Container Fill Level",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                double level = 0;
                if (auto obs = observedPos(ctx.region, ctx.source)) {
                    common::Access at{ctx.region, game::blockPos(*obs), -1};
                    if (auto h = items::find(at); h && h->slots && h->getSlot) {
                        double have = 0, cap = 0;
                        for (int i = 0, n = h->slots(at); i < n; ++i) {
                            auto s = h->getSlot(at, i);
                            cap += 64;
                            have += items::countOf(s) * (64.0 / std::max(1, static_cast<int>(s.getMaxStackSize())));
                        }
                        level = cap > 0 ? have / cap : 0;
                    } else {
                        double have = 0, cap = 0;
                        for (auto const& t : fluids::tanks(ctx.region, game::blockPos(*obs), -1)) {
                            have += t.contents.amount;
                            cap += t.capacity;
                        }
                        level = cap > 0 ? have / cap : 0;
                    }
                }
                return {percentOrBar(ctx, stats, level, optionIndex(ctx, "Mode") == 1)};
            },
            .options = {{"Mode", "create.display_source.fill_level.display", "Display Format",
                         {"create.display_source.fill_level.percent", "create.display_source.fill_level.progress_bar"},
                         {"Percent", "Progress Bar"}, 0},
                        labelOption()},
        });

        // ---- fluid tanks (U18): Create's BoilerDisplaySource ----
        registerSource("levitate:fluid_tank", Source{
            .id = "boiler", .name = "create.display_source.boiler_status", .nameFallback = "Boiler Status",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                auto space = tr("create.display_source.boiler.not_enough_space", "Not enough space ");
                auto forIt = tr("create.display_source.boiler.for_boiler_status", "for Boiler Status");
                if (stats.maxRows < 2) return {space + forIt};
                if (stats.maxRows < 4) return {space, forIt};
                Lines out;
                auto  lines = steam::goggleLines(ctx.source);
                auto  tank  = steam::tank(ctx.source);
                if (!tank || !tank->boiler || lines.size() < 4) {
                    out = {tr("create.boiler.status_short", "Boiler:  {0}", tr("create.boiler.idle", "Idle")), "", "", ""};
                } else {
                    // "Boiler Status:  §a<level>§r" -> Create's status_short; the bars without the labels
                    auto        a = lines[0].find("\xC2\xA7" "a"), b = lines[0].rfind("\xC2\xA7" "r");
                    std::string level = a != std::string::npos && b != std::string::npos && b > a ? lines[0].substr(a, b - a + 3) : lines[0];
                    out.push_back(tr("create.boiler.status_short", "Boiler:  {0}", level));
                    for (int i = 1; i <= 3; ++i) {
                        auto const& l   = lines[static_cast<std::size_t>(i)];
                        auto        bar = l.find('|');
                        auto        cut = bar == std::string::npos ? l.size() : l.rfind("\xC2\xA7", bar);
                        out.push_back(cut == std::string::npos ? l : l.substr(cut));
                    }
                }
                if (stats.targetId == "lectern") { // one page
                    std::string page;
                    for (std::size_t i = 0; i < out.size(); ++i) page += (i ? "\n" : "") + out[i];
                    return {page};
                }
                return out;
            },
            .multiLine = true,
        });

        // ---- elevator contacts (U20): Create's CurrentFloorDisplaySource ----
        registerSource("levitate:elevator_contact", Source{
            .id = "current_floor", .name = "create.display_source.current_floor", .nameFallback = "Elevator Location",
            .text = [](Context& ctx, TargetStats const&) -> Lines {
                // the pistons unit keeps the contact's lastReportedCurrentFloor in its BlockData
                auto const* doc = common::blockdata::get(ctx.source);
                if (!doc || !doc->contains("pistons") || !(*doc)["pistons"].is_object()) return {""};
                return {(*doc)["pistons"].value("floor", std::string())};
            },
        });

        // ---- smart observer (U23, block id content_observer): Create's PackageAddressDisplaySource ----
        registerSource("levitate:content_observer", Source{
            .id = "read_package_address", .name = "create.display_source.read_package_address", .nameFallback = "Read Package Address",
            .text = [](Context& ctx, TargetStats const&) -> Lines {
                auto obs = observedPos(ctx.region, ctx.source);
                if (!obs) return {""};
                common::Access at{ctx.region, game::blockPos(*obs), -1};
                if (auto h = items::find(at); h && h->slots && h->getSlot) {
                    for (int i = 0, n = h->slots(at); i < n; ++i) {
                        auto s = h->getSlot(at, i);
                        if (packages::isPackage(s) && common::filterslot::matches(ctx.source, s)) return {labelled(ctx, packages::addressOf(s))};
                    }
                    return {""};
                }
                // no inventory: the packages looping around a chain conveyor there
                for (auto const& box : packages::loopingPackages(*obs))
                    if (common::filterslot::matches(ctx.source, box)) return {labelled(ctx, packages::addressOf(box))};
                return {""};
            },
            .options = {labelOption()},
        });

        // ---- belts, depots, ejectors (U11): the name of the item on them ----
        for (auto const* id : {"levitate:belt", "levitate:depot", "levitate:weighted_ejector"}) {
            registerSource(id, Source{
                .id = "item_names", .name = "create.display_source.combine_item_names", .nameFallback = "Combine Item Names",
                .text = [](Context& ctx, TargetStats const&) -> Lines {
                    common::Access at{ctx.region, game::blockPos(ctx.source), -1};
                    if (auto h = items::find(at); h && h->slots && h->getSlot) {
                        for (int i = 0, n = h->slots(at); i < n; ++i) {
                            auto s = h->getSlot(at, i);
                            if (!items::isEmpty(s)) return {labelled(ctx, items::displayName(s))};
                        }
                    }
                    return {""};
                },
                .options = {labelOption()},
            });
        }

        // ---- tunnels (U12 calls display::itemsPassed) ----
        for (auto const* id : {"levitate:andesite_tunnel", "levitate:brass_tunnel"}) {
            registerSource(id, Source{
                .id = "accumulate_items", .name = "create.display_source.accumulate_items", .nameFallback = "Accumulate Item Count",
                .text = [](Context& ctx, TargetStats const&) -> Lines {
                    long long total = ctx.config.value("Collected", 0LL) + ctx.config.value("lv:pending", 0LL);
                    ctx.config["Collected"]  = total;
                    ctx.config["lv:pending"] = 0;
                    return {labelled(ctx, std::to_string(total))};
                },
                .options = {labelOption()},
                .refreshTicks = 200, // Create's AccumulatedItemCountDisplaySource
                .onSignalReset = [](Context& ctx) {
                    ctx.config["Collected"]  = 0;
                    ctx.config["lv:pending"] = 0;
                },
            });
            registerSource(id, Source{
                .id = "item_throughput", .name = "create.display_source.item_throughput", .nameFallback = "Item Throughput",
                .text = [](Context& ctx, TargetStats const&) -> Lines {
                    // Create's ItemThroughputDisplaySource.provideLine: the pooled rate (Link.cpp's
                    // itemsPassed keeps it) per interval; inactive when no item came for twice the
                    // usual time between two batches
                    auto& conf = ctx.config;
                    if (conf.value("Inactive", false)) return {labelled(ctx, "0")};
                    double interval = 20 * std::pow(60, optionIndex(ctx, "Interval"));
                    double rate     = conf.value("Rate", 0.0) * interval;
                    if (rate > 0) {
                        long long diff = static_cast<long long>(ctx.gameTime) - conf.value("LastReceived", 0LL);
                        if (diff > 0 && diff > conf.value("LastReceivedAmount", 0) / rate * 2) conf["Inactive"] = true;
                    }
                    return {labelled(ctx, number(rate))};
                },
                .options = {{"Interval", "create.display_source.item_throughput.interval", "Interval",
                             {"create.display_source.item_throughput.interval.second", "create.display_source.item_throughput.interval.minute",
                              "create.display_source.item_throughput.interval.hour"},
                             {"per Second", "per Minute", "per Hour"}, 0},
                            labelOption()},
            });
        }

        // ---- seats (U22): the seated entity's name ----
        for (int c = 0; c < 16; ++c) {
            registerSource(std::string("levitate:") + kDyeNames[static_cast<std::size_t>(c)] + "_seat", Source{
                .id = "entity_name", .name = "create.display_source.entity_name", .nameFallback = "Entity Name",
                .text = [](Context& ctx, TargetStats const&) -> Lines {
                    auto level = ll::service::getLevel();
                    if (!level) return {""};
                    for (auto* a : level->getRuntimeActorList()) {
                        if (!a || static_cast<int>(a->getDimensionId()) != ctx.source.dim || !a->getVehicle()) continue;
                        auto const& p = a->getPosition();
                        if (std::floor(p.x) != ctx.source.x || std::floor(p.z) != ctx.source.z) continue;
                        if (p.y < ctx.source.y - 0.5 || p.y > ctx.source.y + 2.5) continue;
                        if (a->isPlayer()) return {labelled(ctx, static_cast<Player*>(a)->getRealName())};
                        auto tag = a->getNameTag();
                        return {labelled(ctx, tag.empty() ? prettyId(a->getTypeName()) : tag)};
                    }
                    return {""};
                },
                .options = {labelOption()},
            });
        }

        // ---- vanilla blocks ----
        registerSource("minecraft:target", Source{
            .id = "redstone_power", .name = "create.display_source.redstone_power", .nameFallback = "Redstone Power",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                auto const* b = loadedBlock(ctx.region, game::blockPos(ctx.source));
                int power = b ? intState(*b, "redstone_signal").value_or(signalAt(ctx.region, game::blockPos(ctx.source))) : 0;
                if (optionIndex(ctx, "Mode") == 1) return {percentOrBar(ctx, stats, power / 15.0, true)};
                return {labelled(ctx, std::to_string(power))};
            },
            .options = {{"Mode", "create.display_source.redstone_power.display", "Display Format",
                         {"create.display_source.redstone_power.number", "create.display_source.redstone_power.progress_bar"},
                         {"Number", "Progress Bar"}, 0},
                        labelOption()},
        });
        registerSource("minecraft:enchanting_table", Source{
            .id = "enchant_power", .name = "create.display_source.max_enchant_level", .nameFallback = "Max Enchanting Cost",
            .text = [](Context& ctx, TargetStats const&) -> Lines {
                // the bottom slot's cost (Java getEnchantmentCost slot 2): at least 2 per bookshelf ⚠
                int power = enchantPower(ctx.region, ctx.source);
                return {labelled(ctx, std::to_string(std::max(1 + (power >> 1), power * 2)))};
            },
            .options = {labelOption()},
        });
        auto scoreboard = Source{
            .id = "scoreboard", .name = "create.display_source.scoreboard", .nameFallback = "Scoreboard",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                auto objective = ctx.config.value("Objective", std::string{});
                return scoreboardLines(objective, stats, nullptr, optionIndex(ctx, "ValueDisplay") == 0);
            },
            .options = {{"Objective", "create.display_source.scoreboard.objective", "Objective ID", {}, {}, ""}, valueDisplayOption()},
            .multiLine = true,
        };
        for (auto const* id : {"minecraft:command_block", "minecraft:repeating_command_block", "minecraft:chain_command_block"}) {
            registerSource(id, scoreboard);
        }
        registerSource("minecraft:respawn_anchor", Source{
            .id = "death_count", .name = "create.display_source.player_deaths", .nameFallback = "Player Deaths",
            .text = [](Context& ctx, TargetStats const& stats) -> Lines {
                std::vector<std::pair<long long, std::string>> list;
                for (auto const& [n, c] : deaths()) list.emplace_back(c, n);
                std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
                // Create's StatTrackingDisplaySource: name first, full numbers, no options
                std::vector<std::pair<std::string, std::string>> entries;
                for (auto const& [c, n] : list) entries.emplace_back(formatValue(c, false), n);
                return valueList(entries, stats, false);
            },
            .multiLine = true,
        });
    }
};
Registration const gRegistration;

} // namespace

/// Tracks player deaths for the respawn anchor's source (Bedrock has no deaths statistic).
class DisplayDeathsModule {
public:
    void install() {
        game::db::onWorldReady([] {
            deaths().clear();
            if (auto j = game::db::load(kDbDeaths); j && j->is_object())
                for (auto const& [n, c] : j->items()) deaths()[n] = c.get<long long>();
        });
        game::db::onWorldClosed([] { deaths().clear(); });
        mDie = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDieEvent>([](ll::event::PlayerDieEvent& ev) {
            ++deaths()[ev.self().getRealName()];
            if (game::db::available()) game::db::save(kDbDeaths, Json(deaths()));
        });
    }
    void uninstall() { ll::event::EventBus::getInstance().removeListener(mDie); }

private:
    ll::event::ListenerPtr mDie;
};

} // namespace lv::feature::display

LV_REGISTER_MODULE(lv::feature::display::DisplayDeathsModule);
