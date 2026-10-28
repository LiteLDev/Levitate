#pragma once

// Internals of the packages unit (U25): runtime state of packagers, ports and chain conveyors,
// world helpers shared by the unit's .cpp files. Server thread only.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

#include "core/GlobalPos.h"
#include "core/packages/Chain.h"
#include "feature/packages/Packages.h"

class Block;
class BlockSource;
class Player;

namespace lv::feature::packages {

namespace cp = core::packages;

// ---- config (`config/packages.json`, Create's CLogistics / CKinetics keys) --------------------------
struct PackagesConfig {
    int version = 1;
    /// Maximum distance in blocks a Package Port can be placed at from its target.
    int packagePortRange = 5;
    /// The amount of packages a chain conveyor can carry at a time.
    int chainConveyorCapacity = 20;
    /// Maximum length in blocks of chain conveyor connections.
    int maxChainConveyorLength = 32;
    /// Maximum amount of connections each chain conveyor can have.
    int maxChainConveyorConnections = 4;
};
[[nodiscard]] PackagesConfig const& config();

// ---- kinds -------------------------------------------------------------------------------------------
enum class Kind { None, Packager, Repackager, Frogport, Postbox, ChainConveyor };
[[nodiscard]] Kind        kindOf(std::string_view blockId);
[[nodiscard]] char const* kindName(Kind k);

/// Block states.
inline constexpr char const* kFacing    = "levitate:facing";    // packagers: 0..5 (Bedrock face ids)
inline constexpr char const* kPowered   = "levitate:powered";   // packagers
inline constexpr char const* kLinked    = "levitate:linked";    // packager
inline constexpr char const* kDirection = "levitate:direction"; // postbox: Java 2D facing 0 S, 1 W, 2 N, 3 E
inline constexpr char const* kOpen      = "levitate:open";      // postbox
inline constexpr char const* kFlag      = "levitate:flag";      // postbox: holds packages
inline constexpr char const* kGoggles   = "levitate:goggles";   // frogport
inline constexpr char const* kRotor     = "levitate:rotor";     // chain conveyor (kinetics rotor model)

inline constexpr char const* kWrenchId = "levitate:wrench";
inline constexpr char const* kGogglesId = "levitate:goggles";

// ---- world helpers (World.cpp) -------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] bool         isLoaded(GlobalPos const& pos);
[[nodiscard]] Block const* blockAt(GlobalPos const& pos);
[[nodiscard]] std::string  idAt(GlobalPos const& pos);
[[nodiscard]] std::optional<int> stateAt(GlobalPos const& pos, std::string_view state);
/// Changes some states of the block at `pos` (keeps the others). False if nothing changed.
bool setStates(GlobalPos const& pos, std::vector<std::pair<std::string_view, int>> const& states);
/// The block at `pos` receives redstone power from a neighbour (polled: custom blocks get no
/// neighbour updates).
[[nodiscard]] bool redstonePowered(GlobalPos const& pos);
/// Text of a sign at `pos` (Create's getSign: non-blank lines of the first non-blank side joined
/// by spaces), nullopt when there is no sign.
[[nodiscard]] std::optional<std::string> signText(GlobalPos const& pos);

[[nodiscard]] GlobalPos  offset(GlobalPos const& p, cp::Offset o);
[[nodiscard]] GlobalPos  offsetFace(GlobalPos const& p, int face);
[[nodiscard]] cp::Offset diff(GlobalPos const& from, GlobalPos const& to);
[[nodiscard]] cp::V3     corner(GlobalPos const& p);
[[nodiscard]] cp::V3     centre(GlobalPos const& p);
[[nodiscard]] int        oppositeFace(int face);
[[nodiscard]] cp::Offset faceOffset(int face);
/// Java's Direction.orderedByNearest for a look vector, as Bedrock face ids.
[[nodiscard]] std::vector<int> nearestLookingFaces(cp::V3 look);

void playSound(int dim, cp::V3 at, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void particle(int dim, cp::V3 at, std::string const& effect);
void actionBar(Player& player, std::string const& text);
/// Spawns an item entity (optionally with a motion).
void dropItem(int dim, cp::V3 at, ItemStack const& stack, std::optional<cp::V3> motion = std::nullopt);
void giveOrDrop(Player& player, ItemStack const& stack);
[[nodiscard]] Player* playerById(std::int64_t uniqueId);
[[nodiscard]] cp::V3  eyePos(Player const& player);
[[nodiscard]] cp::V3  lookVec(Player const& player);
/// Removes `count` items with id `itemId` from the player's inventory (selected slot first, as
/// Create's getChainsFromInventory). With `simulate` only counts.
bool takeItems(Player& player, std::vector<std::string> const& itemIds, int count, bool simulate);
/// The item ids of the vanilla chain on this game version.
[[nodiscard]] std::vector<std::string> const& chainItemIds();
[[nodiscard]] bool                            isChainItem(ItemStack const& stack);

/// Runs `fn` at the start of the next server tick.
void defer(std::function<void()> fn);
[[nodiscard]] std::uint64_t currentTick();

// ---- block index (Module.cpp) ----------------------------------------------------------------------------
void track(GlobalPos const& pos, Kind kind);
void untrack(GlobalPos const& pos);
[[nodiscard]] std::vector<GlobalPos> tracked(Kind kind);

// ---- packagers (Packager.cpp) -------------------------------------------------------------------------------
struct QueuedBox {
    ItemStack box;
    int       count = 1;
};
struct PackagerState {
    ItemStack              held;
    ItemStack              previouslyUnwrapped;
    std::vector<QueuedBox> queue;
    int                    animationTicks = 0;
    bool                   animationInward = true;
    bool                   redstonePowered = false;
    int                    buttonCooldown  = 0;
    std::string            signAddress;
    bool                   lastPowerSeen = false; ///< polled neighbour power (edge detection)
};
inline constexpr int kPackagerCycle = 20;

PackagerState& packager(GlobalPos const& pos);
void           savePackager(GlobalPos const& pos);
void           tickPackager(GlobalPos const& pos, bool repackager, bool lazy);
/// Create's PackagerBlockEntity.unwrapBox (RepackagerBlockEntity for repackagers).
bool unwrapBox(GlobalPos const& pos, ItemStack const& box, bool simulate);
/// Player right-click (Create's PackagerBlock.useItemOn). True when consumed.
bool usePackager(Player& player, GlobalPos const& pos);
/// Facing for a packager placed at `pos` (Create's getStateForPlacement); nullopt = refused.
[[nodiscard]] std::optional<int> packagerPlacementFacing(Player& player, GlobalPos const& pos, bool repackager);
void onPackagerPlaced(GlobalPos const& pos, int facing);
void installPackagerCapabilities();

// ---- ports (Ports.cpp) ------------------------------------------------------------------------------------
struct PortTarget {
    enum class Type { Chain, Station } type = Type::Chain;
    cp::Offset                relative;      ///< from the port to the target block
    float                     chainPos = 0;  ///< chain: degrees on the loop / blocks along the chain
    std::optional<cp::Offset> connection;    ///< chain: travelling chain (none = the loop)
    bool                      flipped = false;
};
struct PortState {
    std::vector<ItemStack>    inventory = std::vector<ItemStack>(kPortSlots);
    std::string               address;
    bool                      acceptsPackages = true;
    std::optional<PortTarget> target;
    // frogport
    bool      animating      = false;
    int       animationTicks = 0; ///< ticks since the animation started (Create's animationProgress * 10)
    ItemStack animatedPackage;
    bool      depositing        = false;
    bool      failedLastExport  = false;
    float     passiveYaw        = 0;
    // postbox
    int  openTicks  = 0;
    bool registered = false;
};
inline constexpr int kFrogAnimationTicks = 10;

PortState& port(GlobalPos const& pos);
void       savePort(GlobalPos const& pos);
void       tickPort(GlobalPos const& pos, Kind kind, bool lazy);
/// `filter` of a port: its address when it accepts packages, nullopt for "send only".
[[nodiscard]] std::optional<std::string> portFilter(PortState const& s);
[[nodiscard]] bool                       isBackedUp(PortState const& s);
/// Create's FrogportBlockEntity.startAnimation (`deposit`: send `box` to the target).
void startFrogAnimation(GlobalPos const& pos, ItemStack const& box, bool deposit);
void anticipate(GlobalPos const& pos);
/// Opens the port screen (native for companions, a form otherwise).
void openPortScreen(Player& player, GlobalPos const& pos);
bool usePort(Player& player, GlobalPos const& pos, Kind kind);
/// The exact point a port's target is (Create's getExactTargetLocation).
[[nodiscard]] std::optional<cp::V3> targetLocation(GlobalPos const& portPos, PortTarget const& t);
/// Target selection made with a frogport / postbox item before placing it.
struct PendingTarget {
    PortTarget target;   ///< `relative` holds the absolute target block (offset from 0,0,0)
    GlobalPos  targetPos;
    cp::V3     exact;
    bool       postbox = false;
};
void setPendingTarget(Player& player, PendingTarget t);
void onPortPlaced(Player* player, GlobalPos const& pos, Kind kind);
void registerWithTarget(GlobalPos const& pos);
void deregisterFromTarget(GlobalPos const& pos);
void installPortCapabilities();
void installPortScreens();

// ---- chain conveyors (ChainConveyor.cpp) -----------------------------------------------------------------
struct ChainPackage {
    float     chainPosition = 0;
    ItemStack item;
    int       netId       = 0;
    bool      justFlipped = false;
    cp::V3    world;
};
struct ConnectedPort {
    float                     chainPosition = 0;
    std::optional<cp::Offset> connection;
    std::string               filter;
};
struct ConveyorState {
    std::set<cp::Offset>                         connections;
    std::vector<ChainPackage>                    looping;
    std::map<cp::Offset, std::vector<ChainPackage>> travelling;
    std::map<cp::Offset, ConnectedPort>          loopPorts;   ///< key: offset conveyor -> port
    std::map<cp::Offset, ConnectedPort>          travelPorts;
    cp::RoutingTable                             routing;
    bool                                         reversed     = false;
    bool                                         checkInvalid = true;
    std::map<cp::Offset, cp::ConnectionStats>    stats;
    bool                                         statsValid = false;
};
ConveyorState& conveyor(GlobalPos const& pos);
[[nodiscard]] bool hasConveyor(GlobalPos const& pos); ///< a chain conveyor block is at pos
void saveConveyor(GlobalPos const& pos);
void tickConveyor(GlobalPos const& pos, bool lazy);
void prepareStats(GlobalPos const& pos, ConveyorState& s);
[[nodiscard]] cp::V3 packagePosition(GlobalPos const& pos, ConveyorState& s, float chainPosition, std::optional<cp::Offset> connection);
[[nodiscard]] bool canAcceptPackagesFor(GlobalPos const& pos, std::optional<cp::Offset> connection);
bool addLoopingPackage(GlobalPos const& pos, ChainPackage box);
bool addTravellingPackage(GlobalPos const& pos, ChainPackage box, cp::Offset connection);
[[nodiscard]] int nextNetId();
bool connectConveyors(Player* player, GlobalPos const& a, GlobalPos const& b);
void disconnectConveyors(GlobalPos const& a, GlobalPos const& b, bool spawnDrops, Player* refundTo);
/// The chain under a player's crosshair (Create's ChainConveyorInteractionHandler).
struct ChainPick {
    GlobalPos                 conveyor;
    float                     chainPosition = 0;
    std::optional<cp::Offset> connection;
    cp::V3                    exact;
    double                    distance = 0;
};
[[nodiscard]] std::optional<ChainPick> pickChain(Player& player, bool dismantling, double maxDistance);
/// A package hanging on a chain under the crosshair.
struct PackagePick {
    GlobalPos                 conveyor;
    std::optional<cp::Offset> connection;
    int                       netId = 0;
    double                    distance = 0;
};
[[nodiscard]] std::optional<PackagePick> pickPackage(Player& player, double maxDistance);
bool takePackageFromChain(Player& player, PackagePick const& pick);
bool putPackageOnChain(Player& player, ChainPick const& pick);
void startRiding(Player& player, ChainPick const& pick);
void tickRiders();
void onConveyorRemoved(GlobalPos const& pos, nlohmann::json const& doc, bool creativeBreak);
bool useConveyorWithChain(Player& player, GlobalPos const& pos);
void clearChainSelection(Player& player, bool message);
void refundChainsOnWrench(Player& player, GlobalPos const& pos);
void installConveyorKinetics();
[[nodiscard]] bool conveyorPlacementBlocked(GlobalPos const& pos);

// ---- items (PackageItems.cpp) -------------------------------------------------------------------------------
/// Shift-use: opens the package into the player's inventory (Create's PackageItem.open).
void openPackage(Player& player);
/// Release after holding use: throws the package (Create's releaseUsing).
void throwPackage(Player& player, int ticksHeld);
void openPackageFilterForm(Player& player);
void installItemScreens();

// ---- client sync (Sync.cpp) ---------------------------------------------------------------------------------
/// Frog tongue animation for companions near the port.
void syncFrog(GlobalPos const& pos, PortState const& s);
/// Chains and packages of conveyors near companions (called every few ticks).
void syncConveyors();
void syncConveyorNow(GlobalPos const& pos);

/// Sends `payload` on `channel` to companions within 96 blocks of `near` (same dimension).
void broadcast(GlobalPos const& near, std::string const& channel, Json const& payload);
/// Packager tray animation for companions.
void syncPackager(GlobalPos const& pos, PackagerState const& s);

/// Removes everything (world closed).
void resetState();
void resetPackagers();
void resetPorts();
void resetConveyors();
/// Drops the runtime state of a block that is gone (its BlockData drops are handled separately).
void forgetPackager(GlobalPos const& pos);
void forgetPort(GlobalPos const& pos);
void forgetConveyor(GlobalPos const& pos);

} // namespace lv::feature::packages
