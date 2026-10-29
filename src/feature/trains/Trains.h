#pragma once

// Internals of the trains unit (U27): train stations, bogeys, train controls, schedules and the
// trains themselves (Create 6.0.10's trains on the track graph of U5, carriages as contraptions
// of U4). Not a contract for other units: see feature/tracks/TrainsExt.h. SPEC §2.23 / §3.6.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/contraption/Geometry.h"
#include "core/trains/Motion.h"
#include "core/trains/Physics.h"
#include "core/trains/Schedule.h"

class Player;
class BlockSource;
class BlockPos;
class ItemStack;

namespace lv::feature::trains {

namespace ct = core::trains;
namespace cc = core::contraption;
using core::GlobalPos;
using Json = nlohmann::json;

// ---- ids ----------------------------------------------------------------------------------------
inline constexpr std::string_view kStation    = "levitate:track_station";
inline constexpr std::string_view kSmallBogey = "levitate:small_bogey";
inline constexpr std::string_view kLargeBogey = "levitate:large_bogey";
inline constexpr std::string_view kControls   = "levitate:controls";
inline constexpr std::string_view kSchedule   = "levitate:schedule";
/// Owned by other units: referenced by id only.
inline constexpr std::string_view kRailwayCasing = "levitate:railway_casing"; ///< U10
inline constexpr std::string_view kWrench        = "levitate:wrench";         ///< U8
inline constexpr std::string_view kBlazeBurner   = "levitate:blaze_burner";   ///< U14
inline constexpr std::string_view kTrainDoor     = "levitate:train_door";     ///< U10

// ---- block states -------------------------------------------------------------------------------
inline constexpr char kAssemblingState[] = "levitate:assembling";        ///< station 0/1
inline constexpr char kBogeyAxisState[]  = "levitate:bogey_axis";        ///< 0 x, 1 z

/// `config/trains.json`: Create's CTrains keys (the track ones are in config/tracks.json).
struct Config {
    int    version                     = 1;
    bool   trainsCauseDamage           = true;
    int    maxAssemblyLength           = 128;
    int    maxBogeyCount               = 20;
    double manualTrainSpeedModifier    = 0.75;
    double trainTopSpeed               = 28;
    double trainTurningTopSpeed        = 14;
    double trainAcceleration           = 3;
    double poweredTrainTopSpeed        = 40;
    double poweredTrainTurningTopSpeed = 20;
    double poweredTrainAcceleration    = 3;
};
[[nodiscard]] Config const&      config();
[[nodiscard]] ct::SpeedConfig    speedConfig();

// ---- stations -----------------------------------------------------------------------------------

enum class DoorMode : int { All = 0, North, East, South, West, None };

struct Station {
    GlobalPos   pos;          ///< the station block
    GlobalPos   track;        ///< the targeted track block
    int         fx = 1, fz = 0; ///< arrival direction (trains face it; the assembly area is behind)
    std::string name = "Track Station";
    DoorMode    door = DoorMode::All;
    bool        assembling = false;
    Json        autoSchedule; ///< schedule item (items::toJson) or null
    std::string lastTrainName;
    Json        lastError;    ///< last assembly error {key, fallback, args, carriage}

    [[nodiscard]] std::string pointId() const;
    [[nodiscard]] Json        toJson() const;
    [[nodiscard]] static Station fromJson(Json const& j);
    /// The station's edge point as a destination (nullopt when it isn't on the graph).
    [[nodiscard]] std::optional<ct::TrackPos> point() const;
};

// ---- trains -------------------------------------------------------------------------------------

struct Carriage {
    std::uint64_t        contraption = 0;
    std::vector<double>  bogeyOffsets; ///< bogey centres, blocks behind the train's front point
    std::vector<cc::Pos> bogeyLocal;   ///< bogey blocks in the contraption's local coordinates
    std::vector<int>     bogeySize;    ///< 0 small, 1 large
    cc::Vec3d            origin;       ///< contraption origin (first bogey block at assembly)
    /// Train Controls: local position and +1 forward / -1 backward.
    std::vector<std::pair<cc::Pos, int>> controls;
    bool                 forward = false, backward = false;
};

struct Navigation {
    std::string stationPos; ///< station key (GlobalPos::key)
    ct::Path    path;
    double      distance = 0; ///< left to the station point
    bool        behind   = false; ///< reversing (destination behind the train)
    bool        manual   = false; ///< "hold space to approach"
};

struct Train {
    std::uint64_t         id = 0;
    std::string           name;
    std::string           owner; ///< player UUID
    int                   dim = 0;
    ct::Body              body;
    std::vector<Carriage> carriages;
    int                   ax = 1, az = 0;  ///< assembly direction (local frame of every carriage)
    double                lift = 1;        ///< bogey block bottom above the graph position
    double                speed = 0, targetSpeed = 0, throttle = 1;
    bool                  derailed  = false;
    bool                  doubleEnded = false;
    int                   fuelTicks = 0;
    ct::Runtime           runtime;
    std::optional<Navigation> nav;
    std::string           currentStation; ///< station key while stopped at one
    int                   idleTicks = 0;
    int                   reattachCooldown = 0;
    bool                  invalid   = false;
    bool                  disassembling = false;

    // manual control (not saved)
    std::string controller; ///< player UUID
    int         keys = 0, steer = 0;
    bool        jump = false;
    int         lastInput = 0;
    int         controlDir = 1;  ///< +1: controlling through forward controls
    int         controlCarriage = 0;
    cc::Pos     controlLocal;
    std::string lastStatus;      ///< last status key sent (Create prints a failure once)
    bool        trackMissingSent = false;
    bool        spansDirty = true; ///< report the covered track to signals (U28)
    ct::Vec3d   lastFront, lastDir{1, 0, 0}; ///< world position / direction of the front point
    std::size_t cargoHash = 0;
    bool        testConductor = false; ///< selftests: a conductor in both directions
    std::vector<std::string> occupied; ///< edge points between the train's ends

    [[nodiscard]] Json               toJson() const;
    [[nodiscard]] static Train       fromJson(Json const& j);
    [[nodiscard]] ct::Stats          stats() const;
};

struct State {
    std::map<std::string, Station>          stations; ///< by GlobalPos::key
    std::map<std::uint64_t, Train>          trains;
    std::uint64_t                           nextTrainId = 1;
    std::uint64_t                           tick        = 0;
    std::uint64_t                           graphVersion = 0;
    bool                                    stationsDirty = false;
    std::vector<std::function<void()>>      deferred;
    /// Track targeting selections of the station item, by player UUID.
    struct Target {
        GlobalPos track;
        int       fx = 1, fz = 0;
    };
    std::map<std::string, Target>           targets;
    std::map<GlobalPos, std::pair<Target, std::string>> pendingStations; ///< placed this tick
    std::map<std::string, std::uint64_t>    lastUse; ///< debounce per player
};
State& state();

// ---- helpers (World.cpp) ------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] std::string  idAt(GlobalPos const& p);
[[nodiscard]] std::optional<int> intStateAt(GlobalPos const& p, char const* state);
bool                       setBlockAt(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {});
void                       actionBar(Player& player, std::string const& text);
void                       playSound(int dim, ct::Vec3d const& pos, std::string const& sound, float volume = 1, float pitch = 1);
[[nodiscard]] Player*      playerByUuid(std::string const& uuid);
[[nodiscard]] std::string  uuidOf(Player const& player);
/// Runs `fn` at the start of the next server tick.
void defer(std::function<void()> fn);
/// Whether `id` is a bogey block.
[[nodiscard]] bool isBogey(std::string_view id);
/// The straight shape axis of a track at `p` (0 x, 2 z) or -1 (not straight / not a track).
[[nodiscard]] int straightTrackAxis(GlobalPos const& p);

// ---- stations (Stations.cpp) --------------------------------------------------------------------
Station*             stationAt(GlobalPos const& pos);
Station*             stationByKey(std::string const& key);
/// Registers a station bound to `track` (arrival direction fx/fz). Returns false if not possible.
bool                 createStation(GlobalPos const& pos, GlobalPos const& track, int fx, int fz, std::string* errorKey = nullptr);
void                 removeStation(GlobalPos const& pos);
/// (Re-)adds every station's edge point (after graph changes).
void                 refreshStationPoints();
/// The train stopped at the station (nullptr if none).
Train*               trainAt(Station const& s);
/// Assembly track: bogeys by index (index -> size 0 small / 1 large).
std::map<int, int>   bogeysOf(Station const& s, int* trackLength = nullptr);
/// The assembly index of a track block for an assembling station (nullopt if outside).
std::optional<int>   assemblyIndex(Station const& s, GlobalPos const& track);
/// Railway casing on a track of an assembly area: creates or cycles a bogey. Returns the
/// message shown (empty: not an assembly track).
std::string          useCasingOnTrack(Player* player, GlobalPos const& track, bool hasCasing);
void                 setAssembling(Station& s, bool on, Player* by);
/// Assembles the train at the station. Returns the train id or sets `s.lastError`.
std::optional<std::uint64_t> assemble(Station& s, Player* by);
/// Starts disassembling the train at the station (done on the next tick).
bool                 disassemble(Station& s, Player* by);
[[nodiscard]] bool   canDisassemble(Station const& s);
void                 openStationScreen(Player& player, GlobalPos const& pos);
void                 registerStationScreens();
/// Predictions of trains heading to a station: (train name, ticks).
std::vector<std::pair<std::string, int>> arrivals(Station const& s);

// ---- trains (TrainLogic.cpp) --------------------------------------------------------------------
Train*       findTrain(std::uint64_t id);
Train*       trainOfContraption(std::uint64_t contraption);
void         tickTrains();
/// Recomputes and applies every carriage transform (after load / assembly).
void         placeCarriages(Train& t);
/// Starts navigation to a station. Returns the schedule start result.
/// `anyOf` (station keys) replaces the name filter when not empty (package instructions).
ct::StartResult navigateTo(
    Train&                          t,
    std::string const&              filter,
    bool                            manual      = false,
    std::string const&              onlyStation = {},
    std::vector<std::string> const& anyOf       = {}
);
void         cancelNavigation(Train& t);
/// Removes a train (carriages vanish, as Create's /create train remove).
void         removeTrain(std::uint64_t id);
void         statusMessage(Train& t, std::string const& key, std::vector<std::string> const& args, bool good);
/// Conductors facing the train's forward / backward direction.
[[nodiscard]] bool hasConductor(Train const& t, bool forward);
/// The train arrived at the station point (called by the movement and at assembly).
void         arriveAt(Train& t, Station& s);
/// Whether the world block / contraption the player looks at is a controls block of a train;
/// starts or stops controlling. Returns true when handled.
bool         tryControls(Player& player, double maxDistance = 5.5);
void         startControlling(Player& player, Train& t, int carriage, cc::Pos local, int direction);
void         stopControlling(Train& t, bool message = true);
/// Manual input of a controlling player (auth input packet, the client mod or selftests).
void         setInput(Player& player, int throttle, int steer, bool jump, bool sneak);
void         onGraphChanged();
/// Text of a train for commands and displays.
[[nodiscard]] std::string statusLine(Train const& t);

// ---- schedules (Schedules.cpp) ------------------------------------------------------------------
[[nodiscard]] std::optional<ct::Schedule> scheduleOf(ItemStack const& stack);
void         setScheduleOf(ItemStack& stack, ct::Schedule const& s);
[[nodiscard]] ItemStack makeScheduleItem(ct::Schedule const& s);
void         openScheduleEditor(Player& player, int slot);
void         registerScheduleScreens();
/// Right-click on a conductor (entity or blaze burner) of `t` with `held` (Create's
/// ScheduleItem.handScheduleTo / remove with an empty hand).
bool         handScheduleTo(Player& player, Train& t, int slot);

// ---- persistence / module -----------------------------------------------------------------------
void saveAll();
void loadAll();
void markStationsDirty();

// ---- hooks (Hooks.cpp) --------------------------------------------------------------------------
void installHooks();
void uninstallHooks();

// ---- client mod controls (Controls.cpp) ----------------------------------------------------------
/// Create's ControlsInputPacket: held keys [forward, back, left, right, jump, sneak].
void controlsInput(Player& player, Json const& pressed);
/// The player's client mod drives the train it controls (key input arrived on the channel): its
/// PlayerAuthInputPacket is then ignored for driving.
[[nodiscard]] bool drivesByChannel(Player const& player);
/// Create's ControlsStopControllingPacket.
void controlsStop(Player& player);
/// Create's TrainHUDUpdatePacket (throttle 1/18 .. 1).
void setThrottle(Player& player, double throttle);
/// Create's HonkPacket: the horn of the controlled train (its first steam whistle).
void setHonk(Player& player, bool honk);
/// Remaining honk ticks of a train (selftests).
[[nodiscard]] int honkTicksOf(std::uint64_t trainId);
/// A player started / stopped controlling `t`: tells the client mod (key capture, HUD).
void controlsChanged(Train const& t, Player* player, bool active);
/// Horn sounds and the controlling companions' HUD data (every tick).
void tickControls();
void resetControls();

} // namespace lv::feature::trains
