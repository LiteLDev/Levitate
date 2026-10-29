#pragma once

// Fluid tank multiblocks (U18, SPEC §3.6.18): Create 6.0.10's ConnectivityHandler for vertical
// tanks (FluidTankBlockEntity), independent of the game.
//
// Each tank block is a *part*; a structure is a square base of `width` (1..maxWidth) by
// `height` (1..maxHeight) parts whose *controller* is the part at the lowest x, y, z. Only the
// controller holds the fluid of the whole structure (capacity = width² · height · per block).
// The world is reached through `TankAccess` (one kind of tank: fluid tanks and creative tanks
// never connect).

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lv::core::steam {

struct Pos3 {
    int x = 0, y = 0, z = 0;
    friend bool operator==(Pos3 const&, Pos3 const&) = default;
    friend auto operator<=>(Pos3 const&, Pos3 const&) = default;
    [[nodiscard]] Pos3 offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
};

struct TankPart {
    Pos3 controller;
    int  width  = 1;
    int  height = 1;
    // controller only (non-controllers keep these empty)
    std::string fluid;
    std::int64_t amount = 0; ///< mB
    bool        window = true;

    [[nodiscard]] bool isController(Pos3 const& self) const { return controller == self; }
};

struct TankParams {
    int  maxWidth         = 3;    ///< Create's FluidTankBlockEntity.MAX_SIZE
    int  maxHeight        = 32;   ///< config fluidTankMaxHeight
    int  capacityPerBlock = 8000; ///< config fluidTankCapacity (buckets) · 1000
    bool creative         = false;
};

/// The tanks of one kind in the world.
class TankAccess {
public:
    virtual ~TankAccess() = default;
    /// The part at `pos` (nullopt: no tank of this kind there, or not loaded).
    [[nodiscard]] virtual std::optional<TankPart> get(Pos3 const& pos) = 0;
    virtual void set(Pos3 const& pos, TankPart const& part) = 0;
};

/// Create's `formMulti` starting from `frontier` (newly placed or split parts): every part that
/// is its own controller tries to become the min corner of the largest structure (ties: the
/// wider one); the largest candidates are built first. Fluids of absorbed parts are poured into
/// the new controller (what does not fit is lost; fluids of different kinds never connect).
/// Returns the positions whose part changed.
std::set<Pos3> formMulti(TankAccess& world, std::vector<Pos3> frontier, TankParams const& params);

/// Create's `splitMulti` when `removed` (a part, already gone from the world) leaves its
/// structure `removedPart`: the remaining parts become 1×1 tanks, the fluid is shared out in
/// y, x, z order (each up to one block's capacity; the rest is lost), then they re-form.
/// Returns the positions whose part changed.
std::set<Pos3> removePart(TankAccess& world, Pos3 const& removed, TankPart const& removedPart,
                          TankParams const& params);

/// Capacity of a structure in mB.
[[nodiscard]] std::int64_t capacityOf(TankPart const& controller, TankParams const& params);

/// Create's FluidTankBlock.Shape: 0 plain, 1 window, 2 window_nw, 3 window_sw, 4 window_ne,
/// 5 window_se (setWindows: width 1 → window, width 2 → corner windows, width 3 → the middle of
/// each side).
[[nodiscard]] int windowShape(bool window, int width, int xOffset, int zOffset);

/// Create's luminosity of a part at `yOffset` (onFluidStackChanged + updateStateLuminosity):
/// `fluidLight` (0..15) / 1.2 up to the fill level, 1 above it when the fluid glows, 0 without
/// windows.
[[nodiscard]] int partLight(int fluidLight, std::int64_t amount, std::int64_t capacity, int height, int yOffset, bool window);

/// Create's ComparatorUtil.fractionToRedstoneLevel(amount / capacity).
[[nodiscard]] int comparatorLevel(std::int64_t amount, std::int64_t capacity);

/// Fill fraction 0..1.
[[nodiscard]] double fillState(std::int64_t amount, std::int64_t capacity);

} // namespace lv::core::steam
