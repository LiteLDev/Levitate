#pragma once

// Fluid machines (U19), game independent: Create's fluid ids, the potion fluid encoding, the
// item drain's item movement, the portable interface's connection timer, the hose pulley's
// offset and the fluid body searches of the hose pulley (Create's FluidDrainingBehaviour /
// FluidFillingBehaviour). Unit-tested in tests/core/fluids.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lv::core::fluids {

// ---- amounts and ids ---------------------------------------------------------------------------

inline constexpr int kBucket = 1000; ///< mB in a bucket
inline constexpr int kBottle = 250;  ///< mB in a bottle (Create's PotionFluidHandler / honey bottle)

inline constexpr std::string_view kWater     = "minecraft:water";
inline constexpr std::string_view kLava      = "minecraft:lava";
inline constexpr std::string_view kMilk      = "minecraft:milk";
inline constexpr std::string_view kHoney     = "levitate:honey";
inline constexpr std::string_view kChocolate = "levitate:chocolate";
inline constexpr std::string_view kTea       = "levitate:tea";
/// Prefix of every potion fluid: "levitate:potion/<bottle>/<potion>".
inline constexpr std::string_view kPotionPrefix = "levitate:potion/";

/// Create's PotionFluid.BottleType.
enum class Bottle : int { Regular = 0, Splash = 1, Lingering = 2 };

[[nodiscard]] std::string_view bottleName(Bottle b);
[[nodiscard]] std::optional<Bottle> bottleFromName(std::string_view name);
/// The Bedrock item of a bottle type ("minecraft:potion", "minecraft:splash_potion", ...).
[[nodiscard]] std::string_view bottleItem(Bottle b);
[[nodiscard]] std::optional<Bottle> bottleOfItem(std::string_view itemId);

/// A potion type: Bedrock's potion aux value and the Java name Create's data uses.
struct PotionType {
    int              aux;
    std::string_view name;  ///< "harming", "long_swiftness", ... (Java names; Bedrock-only: "decay")
    std::uint32_t    color; ///< RGB of the potion liquid
};
[[nodiscard]] std::vector<PotionType> const& potionTypes();
[[nodiscard]] PotionType const* potionByAux(int aux);
[[nodiscard]] PotionType const* potionByName(std::string_view name);

/// "levitate:potion/regular/harming".
[[nodiscard]] std::string potionFluid(Bottle bottle, std::string_view potion);
struct PotionFluid {
    Bottle      bottle = Bottle::Regular;
    std::string potion;
};
/// Decodes a potion fluid id (nullopt for other fluids or unknown potions).
[[nodiscard]] std::optional<PotionFluid> parsePotionFluid(std::string_view fluid);
[[nodiscard]] inline bool isPotionFluid(std::string_view fluid) { return fluid.starts_with(kPotionPrefix); }

/// The fluid in a potion item (Create's PotionFluidHandler.getFluidFromPotionItem): a regular
/// water bottle is plain water, everything else a potion fluid of its bottle type. Empty when
/// `itemId` is no potion item or `aux` no known potion.
[[nodiscard]] std::string fluidOfPotionItem(std::string_view itemId, int aux);
/// The filled bottle for 250 mB of `fluid` (Create's PotionFluidHandler.fillBottle): item id and
/// aux. nullopt when the fluid can't go into a glass bottle this way.
struct ItemRef {
    std::string id;
    int         aux = 0;
};
[[nodiscard]] std::optional<ItemRef> bottleFor(std::string_view fluid);

/// RGB colour used to draw a fluid (water, lava, honey, chocolate, tea, milk, potions).
[[nodiscard]] std::uint32_t fluidColor(std::string_view fluid);
/// Translation key of a fluid's name (Create's "fluid.create.honey", vanilla NeoForge's "fluid_type.minecraft.water"), as in the fluid registry.
[[nodiscard]] std::string fluidNameKey(std::string_view fluid);
/// The name of a potion fluid (Create's PotionFluidType.getDescriptionId: the potion item's name for
/// its bottle type): the vanilla item key ("item.minecraft.splash_potion.effect.swiftness"; long /
/// strong variants share their potion's name) and its English text ("Splash Potion of Swiftness").
struct NameKey {
    std::string key;
    std::string english;
};
[[nodiscard]] std::optional<NameKey> potionFluidName(Bottle bottle, std::string_view potion);

// ---- item drain ------------------------------------------------------------------------------

/// Faces in Bedrock FacingID order: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
[[nodiscard]] inline int oppositeFace(int face) { return face < 0 ? face : (face ^ 1); }
[[nodiscard]] inline bool verticalFace(int face) { return face == 0 || face == 1; }

/// The item travelling through an item drain (Create's TransportedItemStack in ItemDrainBlockEntity).
struct DrainItem {
    float beltPosition = 0;
    float prevBeltPosition = 0;
    int   travel = 3;   ///< face the item moves towards (Create's `insertedFrom` side), FacingID
    bool  checked = false; ///< reached the centre and was checked for emptying
};

inline constexpr float kDrainItemSpeed = 1 / 8.f; ///< Create: itemMovementSpeed
inline constexpr int   kDrainTime      = 20;      ///< Create: ItemDrainBlockEntity.FILLING_TIME
inline constexpr int   kDrainCapacity  = 1500;    ///< Create: the drain's tank

/// Result of one drain tick for the held item.
enum class DrainStep {
    Move,            ///< the item moved on
    ReachedCentre,   ///< the item is at the centre now: the caller checks whether it can be emptied
    AtOutput,        ///< the item is at the far edge: the caller tries to pass it on
};
/// Moves the item one tick (Create's tick without processing). `processingTicks` > 0 holds it.
DrainStep stepDrainItem(DrainItem& item);
/// Inserting an item from `face` of the drain (the side the item comes from; -1 = from above):
/// the item travels away from that side and starts at the centre when it comes from above / below.
[[nodiscard]] DrainItem insertDrainItem(int fromFace);

// ---- portable storage interface ----------------------------------------------------------------

inline constexpr int kPsiAnimation = 4; ///< Create: PortableStorageInterfaceBlockEntity.ANIMATION

/// Create's PortableStorageInterfaceBlockEntity transfer timer (server side).
struct PsiTimer {
    int  transferTimer = 0;
    int  timeout       = 60; ///< config psiTimeout
    int  keepAlive     = 0;  ///< refreshed (2) every tick by the moving interface while it is there
    bool connected     = false; ///< a contraption is attached (connectedEntity != null)

    /// A contraption arrived (startTransferringTo): connecting animation, then connected.
    void start();
    /// Content moved (onContentTransferred): stays connected for another `timeout` ticks.
    void transferred();
    /// One tick; `powered` disconnects. While kept alive the timer stops at the animation length
    /// (the contraption may leave, the connection stays); when the moving interface is gone the
    /// connection ends. Returns true when the connection ended this tick.
    bool tick(bool powered);
    /// Within the connected window (Create's isConnected).
    [[nodiscard]] bool isConnected() const;
    /// The contraption side may move on (the timer ran down to the animation).
    [[nodiscard]] bool releasesContraption() const { return transferTimer <= kPsiAnimation; }
    /// Connection animation progress 0..1 (client rendering).
    [[nodiscard]] float progress() const;
};

// ---- hose pulley -------------------------------------------------------------------------------

/// Create: KineticBlockEntity.convertToLinear (blocks per tick for a speed in RPM).
[[nodiscard]] inline float linearSpeed(float rpm) { return rpm / 512.f; }

/// One tick of the hose: new offset (blocks below the pulley) and whether it keeps moving.
/// `passable(n)` says whether the block `n` below the pulley lets the hose through.
struct HoseStep {
    float offset = 0;
    bool  moving = false;
};
[[nodiscard]] HoseStep stepHose(float offset, float rpm, std::function<bool(int)> const& passable);
/// The block the hose end is in (Create: worldPosition.below(ceil(offset))).
[[nodiscard]] int hoseEndDepth(float offset);

// ---- fluid bodies ------------------------------------------------------------------------------

struct Pos {
    int  x = 0, y = 0, z = 0;
    friend bool operator==(Pos const&, Pos const&) = default;
    friend auto operator<=>(Pos const&, Pos const&) = default;
};

/// What a fluid search sees at a position.
struct Cell {
    std::string fluid;           ///< fluid in the block ("" = none)
    bool        source   = false; ///< a source (or waterlogged) block of `fluid`
    bool        fillable = false; ///< a fluid source could be put here (air, replaceable, flowing fluid)
};
using CellQuery = std::function<Cell(Pos const&)>;

/// Create's FluidDrainingBehaviour search: the source blocks connected to `root` through blocks
/// of the same fluid (sources or flowing) within `range` blocks, ordered as Create drains them
/// (highest first, then farthest from the root first). `infinite` when more than `threshold`
/// fluid blocks were found (the hose pulley's infinite source rule).
struct DrainPlan {
    std::string      fluid;
    std::vector<Pos> sources;
    bool             infinite = false;
};
[[nodiscard]] DrainPlan planDrain(Pos const& root, CellQuery const& at, int range, int threshold);

/// Create's FluidFillingBehaviour search: the positions where `fluid` can be placed, reached from
/// `root` sideways and downwards through fillable blocks and blocks of the same fluid within
/// `range`, ordered lowest first, then nearest first. `infinite` when the connected body of the
/// same fluid holds more than `threshold` blocks (the fluid is then accepted without placing it,
/// unless the config allows filling infinite bodies).
struct FillPlan {
    std::vector<Pos> targets;
    bool             infinite = false;
};
[[nodiscard]] FillPlan planFill(Pos const& root, std::string_view fluid, CellQuery const& at, int range, int threshold);

} // namespace lv::core::fluids
