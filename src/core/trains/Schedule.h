#pragma once

// Train schedules (pure C++): Create 6.0.10's Schedule / ScheduleRuntime.
//
// A schedule is a list of entries; each entry has an instruction (travel to a station, rename,
// throttle, ...) and, for instructions that support them, wait conditions as columns: the
// conditions of one column are met one after another (AND), the first complete column (OR)
// moves the train on. The runtime is a state machine (pre transit -> in transit -> post transit)
// driven every tick by the game through `Driver`.
//
// Ids are "levitate:<path>" with Create's paths ("create:<path>" is accepted when loading).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::trains {

using Json = nlohmann::json;

namespace ids {
inline constexpr std::string_view kDestination     = "levitate:destination";
inline constexpr std::string_view kPackageDelivery = "levitate:package_delivery";
inline constexpr std::string_view kPackageRetrieval = "levitate:package_retrieval";
inline constexpr std::string_view kRename          = "levitate:rename";
inline constexpr std::string_view kThrottle        = "levitate:throttle";

inline constexpr std::string_view kDelay          = "levitate:delay";
inline constexpr std::string_view kTimeOfDay      = "levitate:time_of_day";
inline constexpr std::string_view kFluidThreshold = "levitate:fluid_threshold";
inline constexpr std::string_view kItemThreshold  = "levitate:item_threshold";
inline constexpr std::string_view kRedstoneLink   = "levitate:redstone_link";
inline constexpr std::string_view kPlayerCount    = "levitate:player_count";
inline constexpr std::string_view kIdle           = "levitate:idle";
inline constexpr std::string_view kUnloaded       = "levitate:unloaded";
inline constexpr std::string_view kPowered        = "levitate:powered";
} // namespace ids

/// Instruction ids in Create's menu order.
[[nodiscard]] std::vector<std::string_view> const& instructionIds();
/// Condition ids in Create's menu order.
[[nodiscard]] std::vector<std::string_view> const& conditionIds();
/// "levitate:delay" -> "delay" (also strips "create:").
[[nodiscard]] std::string pathOf(std::string_view id);
/// "create:delay" / "delay" -> "levitate:delay".
[[nodiscard]] std::string normaliseId(std::string_view id);

/// An instruction or a condition: its id and Create's data fields (`Text`, `Value`, ...).
struct Part {
    std::string id;
    Json        data = Json::object();

    [[nodiscard]] Json toJson() const { return {{"Id", id}, {"Data", data}}; }
    [[nodiscard]] static Part fromJson(Json const& j);
};

/// The default data of a new instruction / condition (Create's defaults).
[[nodiscard]] Part defaultInstruction(std::string_view id);
[[nodiscard]] Part defaultCondition(std::string_view id);
/// Rename and throttle take no wait conditions (Create's supportsConditions).
[[nodiscard]] bool supportsConditions(std::string_view instructionId);

struct Entry {
    Part                           instruction;
    std::vector<std::vector<Part>> conditions; ///< columns (OR) of conditions (AND)

    /// A new entry as Create's editor adds it: travel to a station, wait 5 seconds.
    [[nodiscard]] static Entry makeDefault();
};

struct Schedule {
    std::vector<Entry> entries;
    bool               cyclic        = true; ///< "Loop Forever"
    int                savedProgress = 0;    ///< entry to start at

    [[nodiscard]] Json            toJson() const;
    [[nodiscard]] static Schedule fromJson(Json const& j);
};

// ---- condition helpers --------------------------------------------------------------------------

/// Ticks of a `Value` + `TimeUnit` (0 ticks, 1 seconds, 2 minutes) condition.
[[nodiscard]] int waitTicks(Json const& data);
/// Time of day condition: the target day time (ticks) and the rotation period (ticks).
[[nodiscard]] int timeOfDayTarget(Json const& data);
[[nodiscard]] int timeOfDayRotation(Json const& data);
/// Whether day time `dayTime` (0..23999 continuing) fulfils the time of day condition.
[[nodiscard]] bool timeOfDayReached(Json const& data, std::int64_t dayTime);
/// The clock string of a day time target ("8:00 AM" style parts: hour 1..12, minute, pm).
struct Clock {
    int  hour   = 0;
    int  minute = 0;
    bool pm     = false;
};
[[nodiscard]] Clock clockOf(Json const& data);
/// Threshold comparison of Create (0 greater, 1 less, 2 equal).
[[nodiscard]] bool compareThreshold(int op, long long have, long long threshold);
/// The `Threshold` field as a number (Create stores a string; invalid -> 0).
[[nodiscard]] long long thresholdOf(Json const& data);

/// Create's station name filter: `*` matches anything, everything else literal, whole name.
[[nodiscard]] bool stationMatches(std::string_view filter, std::string_view name);
/// Create's title of a destination filter (the filter with '*' removed, trimmed).
[[nodiscard]] std::string filterTitle(std::string_view filter);

// ---- runtime ------------------------------------------------------------------------------------

enum class RuntimeState : int { PreTransit = 0, InTransit = 1, PostTransit = 2 };

/// What starting a "travel to" instruction gave.
enum class StartResult { Navigating, AlreadyThere, NoMatch, NoPath, NoDriver, NoTarget };

/// The game side of a running schedule.
class Driver {
public:
    virtual ~Driver() = default;
    // instructions
    virtual StartResult startDestination(std::string const& filter) = 0;
    virtual StartResult startPackages(bool delivery, std::string const& filter) = 0;
    virtual void        rename(std::string const& title)              = 0;
    virtual void        setThrottle(double throttle)                  = 0;
    [[nodiscard]] virtual bool navigating() const                    = 0;
    [[nodiscard]] virtual bool derailed() const                      = 0;
    // conditions
    [[nodiscard]] virtual std::int64_t dayTime() const                     = 0;
    [[nodiscard]] virtual long long    itemCount(Json const& filter, bool stacks) const = 0;
    [[nodiscard]] virtual long long    fluidAmount(Json const& filter) const = 0; ///< mB
    [[nodiscard]] virtual bool         linkPowered(Json const& frequency) const = 0;
    [[nodiscard]] virtual int          seatedPlayers() const                   = 0;
    [[nodiscard]] virtual int          idleTicks() const                       = 0;
    [[nodiscard]] virtual bool         stationPowered() const                  = 0;
    [[nodiscard]] virtual bool         stationUnloaded() const                 = 0;
    /// A status message for the train's owner (`train.status.<key>`).
    virtual void status(std::string const& key, std::vector<std::string> const& args, bool good) = 0;
};

/// Create's ScheduleRuntime (the schedule a train follows and where it is in it).
struct Runtime {
    static constexpr int kInterval = 40; ///< ticks between failed attempts

    bool               hasSchedule = false;
    Schedule           schedule;
    bool               isAutoSchedule = false;
    int                currentEntry   = 0;
    RuntimeState       state          = RuntimeState::PreTransit;
    bool               paused         = false;
    bool               completed      = false;
    int                cooldown       = 0;
    int                ticksInTransit = 0;
    bool               failedLast     = false; ///< a navigation failed: print success next time
    std::vector<int>   conditionProgress;
    std::vector<Json>  conditionContext;
    std::string        currentTitle;
    std::vector<int>   predictionTicks; ///< learned transit ticks per entry (-1 unknown)

    void setSchedule(Schedule s, bool autoSchedule);
    /// Removes the schedule; returns it with `savedProgress` = the current entry.
    Schedule discard();

    /// One tick (ScheduleRuntime.tick).
    void tick(Driver& driver);
    /// The train arrived at its destination (POST_TRANSIT, conditions start).
    void destinationReached();
    /// Navigation was cancelled (PRE_TRANSIT, retry next tick).
    void transitInterrupted();

    /// The current entry's instruction, nullptr when none.
    [[nodiscard]] Entry const* entry() const;
    /// The title shown on displays: the current title, else the next destination's filter title.
    [[nodiscard]] std::string title() const;
    /// Ticks the current wait still takes when column 0 only has delays (-1 unknown).
    [[nodiscard]] int remainingWaitTicks() const;

    [[nodiscard]] Json            toJson() const;
    [[nodiscard]] static Runtime  fromJson(Json const& j);

private:
    void tickConditions(Driver& driver);
    void startInstruction(Driver& driver);
};

/// One tick of a wait condition with its context (`Time` counters, ...). True when met.
[[nodiscard]] bool tickCondition(Part const& condition, Json& context, Driver const& driver);

/// The status line of the condition a waiting train shows on displays (Create's
/// getWaitingStatus): a translation key with its arguments.
struct StatusText {
    std::string              key;
    std::string              fallback;
    std::vector<std::string> args;
};
[[nodiscard]] StatusText waitingStatus(Part const& condition, Json const& context, Driver const& driver);

/// Station summary time column (Create's display source): "now" (< 200 t), " ~ " (unknown /
/// >= 11700 t), else minutes, or seconds rounded up to 15 s steps.
struct EtaText {
    enum class Kind { Now, Unknown, Minutes, Seconds } kind = Kind::Unknown;
    int value = 0;
};
[[nodiscard]] EtaText etaOf(int ticks);

} // namespace lv::core::trains
