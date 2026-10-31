#pragma once

// Internal state of the belts unit (server thread). Not a contract: other units use BeltApi.h.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mc/world/item/ItemStack.h"
#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/belts/Belt.h"
#include "feature/belts/BeltApi.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lv::feature::belts {

namespace cb = core::belts;
using Json   = nlohmann::json;

// ---- ids and states ----------------------------------------------------------------------------------
inline constexpr char const* kShaftId          = "levitate:shaft";
inline constexpr char const* kTestShaftId      = "levitate:kinetics_test_shaft";
inline constexpr char const* kWrenchId         = "levitate:wrench";
inline constexpr char const* kAndesiteCasingId = "levitate:andesite_casing";
inline constexpr char const* kBrassCasingId    = "levitate:brass_casing";

inline constexpr char const* kFacing = "levitate:facing";
inline constexpr char const* kSlope  = "levitate:slope";
inline constexpr char const* kPart   = "levitate:part";
inline constexpr char const* kCasing = "levitate:casing";
inline constexpr char const* kDyed   = "levitate:dyed";
inline constexpr char const* kColor  = "levitate:color";
inline constexpr char const* kLid    = "levitate:lid";
inline constexpr char const* kRotor  = "levitate:rotor";
inline constexpr char const* kAxis   = "levitate:axis";

inline constexpr char const* kDisplayTag = "lv_belt_display";

/// Config section `belts` (Create's CKinetics / CLogistics keys of this unit).
struct BeltsConfig {
    int version = 1;
    /// Maximum length in blocks of mechanical belts (CKinetics.maxBeltLength).
    int maxBeltLength = 20;
    /// Max distance in blocks a weighted ejector can throw (CKinetics.maxEjectorDistance).
    int maxEjectorDistance = 32;
    /// Ticks until the next item launched by an ejector scans its path for blocks in the way
    /// (CKinetics.ejectorScanInterval, at least 10).
    int ejectorScanInterval = 120;
    /// Show items on belts / depots / in flight as item entities (the vanilla-visible display;
    /// players with the client mod get display items drawn by the mod instead).
    bool displayItems = true;
};
BeltsConfig const& config();

// ---- runtime -----------------------------------------------------------------------------------------

/// What the core item doesn't carry: the stack and the processors' fields.
struct StackData {
    ItemStack    stack;
    int          processingTime = -1;
    std::string  processedBy;
    Json         extra;
    std::int64_t  display = 0;  ///< display item entity (unique id), 0 none
    GlobalPos     shownOwner;   ///< owner of the client mod's display item (itemrender)
    std::uint64_t shownKey = 0; ///< its slot, 0 none
};

struct Belt {
    GlobalPos                           controller;
    int                                 length = 0;
    cb::Inventory                       inv;
    std::map<std::uint64_t, StackData>  stacks;
    cb::SegmentState                    state; ///< the controller's states (kept for when it is broken)
};

/// A launched item (weighted ejector).
struct Airborne {
    StackData  data;
    cb::Launch launch;
    double     t = 0;
    int        facing = 0;
    GlobalPos  origin;
    bool       tracked = false; ///< Create's trackedItem: scans its path for obstacles
};

enum class EjectorState : std::uint8_t { Charged, Launching, Retracting };

struct Depot {
    GlobalPos                          pos;
    bool                               ejector = false;
    std::optional<cb::Item>            held;
    std::vector<cb::Item>              incoming;
    std::map<std::uint64_t, StackData> stacks; ///< of `held` and `incoming`
    std::array<ItemStack, 8>           outputs{};
    // weighted ejector
    EjectorState          state     = EjectorState::Charged;
    float                 lid       = 0;
    int                   launchTicks = 0;
    bool                  powered   = false;
    int                   redstoneWatch = 0; ///< common::redstone watch of an ejector (0 none)
    int                   stackSize = 0; ///< 0 = any
    int                   targetH   = 1, targetV = 0;
    std::vector<Airborne> airborne;
    // Create's ejector obstacle scan: the tracked item's path hit a block early
    int                      scanCooldown = 0;
    std::optional<cb::Vec3d> earlyTarget;      ///< where items stop (on the hit face)
    GlobalPos                earlyTargetBlock; ///< the block that was hit (its belt input takes the items)
    double                   earlyTargetTime = 0;
};

struct State {
    std::map<GlobalPos, std::unique_ptr<Belt>>  belts;  ///< by controller (loaded)
    std::map<GlobalPos, std::unique_ptr<Depot>> depots; ///< loaded depots / ejectors
    std::set<GlobalPos>                         index;  ///< controllers, depots and ejectors of the world
    bool                                        indexDirty = false;
    std::uint64_t                               nextItemId = 1;
    std::uint64_t                               tick       = 0;
    int                                         editing    = 0; ///< > 0 while we change blocks ourselves
    std::set<std::int64_t>                      displays;       ///< display actors we own

    // per tick
    std::vector<std::function<void()>>               tasks;       ///< deferred world edits
    struct Removal {
        GlobalPos        pos;
        cb::SegmentState state;
    };
    std::vector<Removal>                             removals;    ///< belt segments that disappeared
    std::map<GlobalPos, bool>                        playerBroke; ///< pos -> creative
    std::map<GlobalPos, float>                       exploded;    ///< pos -> explosion radius (this tick)
    std::uint64_t                                    blownSegments = 0; ///< belt segments removed by explosions (selftests)
    struct PlaceIntent {
        std::string uuid;
        int         lookFacing = 0;
    };
    std::map<GlobalPos, PlaceIntent>                 placeIntents; ///< ejector placements this tick
    std::map<std::string, std::uint64_t>             lastUse;      ///< debounce per player

    // selections by player uuid
    struct ConnectorSelection {
        GlobalPos pos;
    };
    std::map<std::string, ConnectorSelection> connector;
    std::map<std::string, GlobalPos>          ejectorTargets;
};

State& state();

// ---- world helpers (BeltsModule.cpp) -----------------------------------------------------------------
BlockSource*    regionOf(int dim);
[[nodiscard]] bool loadedAt(GlobalPos const& p);
[[nodiscard]] Block const* blockAt(GlobalPos const& p);
[[nodiscard]] std::string  idAt(GlobalPos const& p);
[[nodiscard]] GlobalPos    offset(GlobalPos p, cb::Vec3i const& d);
[[nodiscard]] cb::Vec3i    vecOf(GlobalPos const& p);
/// Sets a block without our own change handlers reacting to it.
bool setBlockQuiet(GlobalPos const& p, Block const& block);
bool clearBlockQuiet(GlobalPos const& p);

/// The segment states of a belt block (nullopt: not a belt).
[[nodiscard]] std::optional<cb::SegmentState> segmentOf(Block const& block);
[[nodiscard]] std::optional<cb::SegmentState> segmentAt(GlobalPos const& p);
[[nodiscard]] Block const* beltBlock(cb::SegmentState const& s);
/// A plain shaft (Create's ShaftBlock.isShaft): its axis.
[[nodiscard]] std::optional<cb::Axis> shaftAxisAt(GlobalPos const& p);
/// The shaft block to put back where a pulley was.
[[nodiscard]] Block const* shaftBlock(cb::Axis axis);
[[nodiscard]] std::optional<ItemStack> shaftItem(int count = 1);
[[nodiscard]] std::optional<ItemStack> makeStack(std::string const& id, int count = 1);
[[nodiscard]] bool isDepotId(std::string const& id);

void spawnItem(GlobalPos const& dimRef, double x, double y, double z, ItemStack const& stack, double vx = 0, double vy = 0, double vz = 0);
/// Gives a stack to the player; what doesn't fit drops at the player (Create's placeItemBackInInventory).
void giveToPlayer(Player& player, ItemStack stack);
void playSound(GlobalPos const& at, std::string const& name, float volume, float pitch);
void actionbar(Player& player, std::string const& text);
void particle(GlobalPos const& dimRef, double x, double y, double z, std::string const& effect);
std::string uuidOf(Player const& player);

// ---- belt metadata ---------------------------------------------------------------------------------
struct SegmentInfo {
    GlobalPos controller;
    int       index  = 0;
    int       length = 0;
};
/// The controller / index / length of a segment (BlockData, falling back to walking the states).
[[nodiscard]] std::optional<SegmentInfo> segmentInfo(GlobalPos const& p);
/// Every segment from the controller (Create's getBeltChain).
[[nodiscard]] std::vector<GlobalPos> chainFrom(GlobalPos const& controller);
/// Writes controller / index / length to every segment of the belt containing `anySegment` and
/// makes sure the controller is indexed. Returns the controller.
std::optional<GlobalPos> rebuildBelt(GlobalPos const& anySegment);
/// The loaded runtime of a belt (created from BlockData on first use); nullptr if `controller`
/// is not a belt START.
Belt* beltFor(GlobalPos const& controller);
Belt* beltOfSegment(GlobalPos const& segment);
Depot* depotFor(GlobalPos const& pos);
void   indexAdd(GlobalPos const& p);
void   indexRemove(GlobalPos const& p);

std::uint64_t newItemId();
/// World position of an item on a belt.
[[nodiscard]] cb::Vec3d itemWorldPos(Belt const& belt, cb::Item const& item);
/// Drops every item of a belt / depot as item entities.
void ejectAll(Belt& belt);
void dropDepotContents(Depot& depot);
/// Ends the redstone watch of an ejector that goes away.
void releaseDepot(Depot& depot);
void removeDisplay(StackData& data);
void syncDisplay(GlobalPos const& dimRef, StackData& data, cb::Vec3d const& at);

// API conversions
TransportedItem toApi(cb::Item const& item, StackData const& data);
void            fromApi(TransportedItem const& api, cb::Item& item, StackData& data);

// ---- belts (BeltTransport.cpp) ------------------------------------------------------------------------
void  tickBelt(Belt& belt);
float beltSpeedRpm(Belt const& belt);
/// Create's ItemHandlerBeltSegment.insertItem (whole stack at a segment, from the top).
ItemStack insertOnSegment(GlobalPos const& segment, ItemStack const& stack, bool simulate);
ItemStack extractFromSegment(GlobalPos const& segment, int amount, std::function<bool(ItemStack const&)> const& filter, bool simulate);
/// Create's BeltBlockEntity.tryInsertingFromSide.
ItemStack insertFromSide(GlobalPos const& segment, TransportedItem const& item, int side, bool simulate);
bool      canInsertFromSide(GlobalPos const& segment, int side);
/// Create's applyToEachWithin on a belt (index-relative position).
bool applyOnBelt(Belt& belt, float position, float maxDistance, ProcessFn const& fn);

// ---- editing (BeltEdit.cpp) ---------------------------------------------------------------------------
/// Right-click with the belt connector on a block. Returns true when handled (cancel vanilla).
bool onConnectorUse(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock);
void clearConnectorSelection(Player& player);
/// Right-click on a belt (Create's BeltBlock.use + onWrenched). Returns true when handled.
bool onBeltUse(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock);
/// The wrench on a belt (Create's BeltSlicer.useWrench, then onWrenched). True when handled.
bool onBeltWrench(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock);
/// A belt segment disappeared (broken, exploded, replaced): destroy the chain (Create's onRemove).
void processRemovals();
/// Places the belt planned between two shafts (Create's createBelts). False if refused.
bool createBelt(GlobalPos const& start, GlobalPos const& end);
/// What the connector preview shows for a player with a shaft selected (Create's
/// BeltConnectorHandler): no line = looking at nothing (the first shaft sparkles).
struct ConnectorPreview {
    std::optional<cb::PreviewLine> line;
    bool                           valid = true;
};
[[nodiscard]] std::optional<ConnectorPreview> connectorPreviewFor(Player& player, GlobalPos const& first);
void connectorPreview();
/// Tells a companion player's client mod about its connector selection ("belts.connector").
void syncConnectorSelection(Player& player);

// ---- depots (Depots.cpp) --------------------------------------------------------------------------------
void tickDepot(Depot& depot);
/// Right-click on a depot / ejector (Create's SharedDepotBlockMethods.onUse). True when handled.
bool onDepotUse(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& hitInBlock);
/// Sneak + right-click with the ejector item: selects a target (Create's EjectorTargetHandler).
bool onEjectorItemUse(Player& player, GlobalPos const& clicked);
/// An ejector was placed: apply the player's selected target and facing.
void onEjectorPlaced(GlobalPos const& pos, State::PlaceIntent const& intent);
ItemStack depotInsert(Depot& depot, TransportedItem const& item, int side, bool simulate);
bool      depotCanAccept(Depot const& depot);
bool      applyOnDepot(Depot& depot, float maxDistance, ProcessFn const& fn);
void      registerDepotCapabilities();
void      registerEjectorBox();

// ---- registries (BeltsModule.cpp) -------------------------------------------------------------------
[[nodiscard]] Processor const*  processorFor(std::string const& blockId);
/// Removes a processor (selftests).
void unregisterProcessor(std::string const& blockId);
[[nodiscard]] PassFn const*     interactorFor(std::string const& blockId);
[[nodiscard]] DirectInput const* directInputFor(std::string const& blockId);
/// Create's BeltProcessingBehaviour.isBlocked: the block between belt and processor is in the way.
[[nodiscard]] bool processingBlocked(GlobalPos const& transporter);

// ---- hooks (BeltHooks.cpp) ------------------------------------------------------------------------------
void installHooks();
void uninstallHooks();
// ---- collision (BeltShapes.cpp) ---------------------------------------------------------------------------
/// The stepped collision boxes of a sloped segment (pixels: x0 y0 z0 x1 y1 z1); empty otherwise.
[[nodiscard]] std::vector<std::array<float, 6>> slopeCollision(cb::SegmentState const& s);
void installShapes();
void uninstallShapes();
[[nodiscard]] bool isDisplayActor(Actor const& actor);

// ---- persistence --------------------------------------------------------------------------------------
void loadWorld();
void saveWorld();
void closeWorld();

} // namespace lv::feature::belts
