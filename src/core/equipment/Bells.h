#pragma once

// Peculiar / haunted bells (U31 / U49, SPEC §3.6.31), independent of the game.
//
// Create's AbstractBellBlock(Entity) on top of the vanilla BellBlock rules (placement, survival,
// which sides ring it, the swing), HauntedBellBlockEntity / HauntedBellPulser (who pulses when) and
// SoulPulseEffect / SoulPulseEffectHandler (the expanding search for spots where hostile mobs can
// spawn). Directions are Bedrock FacingIDs: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace lv::core::equipment {

/// Java's BellAttachType (block state `levitate:attachment`).
enum class BellAttach { Floor = 0, Ceiling = 1, SingleWall = 2, DoubleWall = 3 };

inline constexpr int kBellRingDuration    = 74; ///< AbstractBellBlockEntity.RING_DURATION
inline constexpr int kHauntedRecharge     = 65; ///< HauntedBellBlockEntity.RECHARGE_TICKS
inline constexpr int kHauntedEffectTicks  = 20; ///< HauntedBellBlockEntity.EFFECT_TICKS
inline constexpr int kHauntedRingDistance = 10; ///< HauntedBellBlockEntity.DISTANCE
inline constexpr int kHeldPulseDistance   = 3;  ///< HauntedBellPulser.DISTANCE
inline constexpr int kHeldPulseRecharge   = 8;  ///< HauntedBellPulser.RECHARGE_TICKS
inline constexpr int kHeldPulseWarmup     = 10; ///< HauntedBellPulser.WARMUP_TICKS
inline constexpr int kMovingPulseDistance = 3;  ///< HauntedBellMovementBehaviour.DISTANCE
inline constexpr int kSoulMaxDistance     = 11; ///< SoulPulseEffect.MAX_DISTANCE
inline constexpr int kSoulTicksPerLayer   = 6;  ///< SoulPulseEffect.TICKS_PER_LAYER
inline constexpr int kSoulWaitingTicks    = 100; ///< SoulPulseEffect.WAITING_TICKS

[[nodiscard]] int  oppositeFace(int face);
[[nodiscard]] bool horizontalFace(int face);
/// The face turned a quarter clockwise seen from above (Java's Direction#getClockWise).
[[nodiscard]] int  clockwise(int face);
[[nodiscard]] std::array<int, 3> faceOffset(int face);

struct BellState {
    BellAttach attach = BellAttach::Floor;
    int        facing = 3; ///< Java's FACING (horizontal)
    friend bool operator==(BellState const&, BellState const&) = default;
};

/// What placement / survival rules may ask about the world around the bell: whether the block in
/// direction `dir` from the bell has a full (sturdy) face towards the bell, and whether the block
/// above can carry a hanging block at the centre of its bottom face.
struct BellSupport {
    std::function<bool(int dir)> sturdy;
    std::function<bool()>        centreAbove;
};

/// Vanilla BellBlock#getStateForPlacement: `clickedFace` is the face of the block that was clicked
/// (the bell goes in front of it), `playerFacing` the player's horizontal direction. nullopt: the
/// bell can't be placed there.
[[nodiscard]] std::optional<BellState> placeBell(int clickedFace, int playerFacing, BellSupport const& support);
/// Vanilla BellBlock#canSurvive.
[[nodiscard]] bool bellSurvives(BellState const& s, BellSupport const& support);
/// Vanilla BellBlock#updateShape after the block in direction `changed` changed: nullopt = the bell
/// breaks, else the (maybe changed) state (double walls lose / gain a side).
[[nodiscard]] std::optional<BellState> bellAfterNeighbourChange(BellState const& s, int changed, BellSupport const& support);
/// AbstractBellBlock#canRingFrom: a click on face `hitFace` at `height` (0..1 within the block).
[[nodiscard]] bool bellRingsFrom(BellState const& s, int hitFace, double height);
/// The direction a redstone signal rings the bell (AbstractBellBlock#neighborChanged).
[[nodiscard]] int bellRedstoneDirection(BellState const& s);
/// BellRenderer#getSwingAngle (radians) after `ticks` ringing ticks.
[[nodiscard]] float bellSwingAngle(float ticks);

// ---- soul pulses -------------------------------------------------------------------------------

using Pos = std::array<int, 3>;

/// The offsets of layer `idx` (0: distance 1 .. 10: distance 11), SoulPulseEffect#getLayer.
[[nodiscard]] std::vector<Pos> const& soulLayer(int idx);

/// One SoulPulseEffect: it expands one layer every 6 ticks, then waits 100 ticks.
class SoulPulse {
public:
    SoulPulse(Pos origin, int distance, bool canOverlap);
    [[nodiscard]] bool finished() const { return mTicks <= -kSoulWaitingTicks; }
    [[nodiscard]] bool canOverlap() const { return mCanOverlap; }
    [[nodiscard]] Pos const& origin() const { return mOrigin; }
    [[nodiscard]] int distance() const { return mDistance; }
    [[nodiscard]] bool expanding() const { return mTicks > 0; }
    /// One tick: the spots of the layer it reached that `canSpawn` accepts (empty layers are
    /// skipped at once, as Create does); nullopt on ticks between layers.
    std::optional<std::vector<Pos>> tick(std::function<bool(Pos const&)> const& canSpawn);
    std::vector<Pos> added; ///< spots this pulse marked (non-overlapping pulses)

private:
    [[nodiscard]] int currentLayer() const { return mDistance - mTicks / kSoulTicksPerLayer - 1; }
    Pos  mOrigin;
    int  mDistance;
    bool mCanOverlap;
    int  mTicks;
};

/// SoulPulseEffectHandler: pulses that don't overlap never mark a spot another one marked while
/// both run. `mark` is called for each new spot.
class SoulPulses {
public:
    void add(SoulPulse pulse);
    void tick(std::function<bool(Pos const&)> const& canSpawn, std::function<void(Pos const&)> const& mark);
    void clear();
    [[nodiscard]] std::size_t size() const { return mPulses.size(); }
    /// A non-overlapping pulse of this origin and size is still expanding.
    [[nodiscard]] bool expanding(Pos const& origin, int distance) const;

private:
    std::vector<SoulPulse> mPulses;
    std::set<Pos>          mOccupied;
};

/// HauntedBellPulser: whether a player holding a haunted bell for `heldTicks` ticks (1 = the first
/// tick) pulses at game time `gameTime`. The first pulse comes after the warmup.
[[nodiscard]] bool heldBellPulses(int heldTicks, long long gameTime);

} // namespace lv::core::equipment
