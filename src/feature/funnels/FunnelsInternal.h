#pragma once

// Internals of the item logistics unit (U12): funnels, tunnels, chutes, item vaults, creative
// crate. See docs/SPEC.md §2.7 / §3.6.12 (U12). Server thread only.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

#include "core/GlobalPos.h"
#include "core/funnels/Logistics.h"

class Actor;
class Block;
class BlockSource;
class ItemActor;
class Player;

namespace lv::feature::funnels {

namespace cf = core::funnels;
using core::GlobalPos;
using Json = nlohmann::json;

inline constexpr char kAndesiteFunnel[] = "levitate:andesite_funnel";
inline constexpr char kBrassFunnel[]    = "levitate:brass_funnel";
inline constexpr char kAndesiteTunnel[] = "levitate:andesite_tunnel";
inline constexpr char kBrassTunnel[]    = "levitate:brass_tunnel";
inline constexpr char kChute[]          = "levitate:chute";
inline constexpr char kSmartChute[]     = "levitate:smart_chute";
inline constexpr char kItemVault[]      = "levitate:item_vault";
inline constexpr char kCreativeCrate[]  = "levitate:creative_crate";
// items of other units, referenced by id only
inline constexpr char kWrench[]              = "levitate:wrench";
inline constexpr char kIndustrialIronBlock[] = "levitate:industrial_iron_block";
inline constexpr char kMechanicalArm[]       = "levitate:mechanical_arm";

// block states
inline constexpr char kFacing[]     = "levitate:facing";
inline constexpr char kExtracting[] = "levitate:extracting";
inline constexpr char kPowered[]    = "levitate:powered";
inline constexpr char kFlap[]       = "levitate:flap";
inline constexpr char kAxis[]       = "levitate:axis";
inline constexpr char kShape[]      = "levitate:shape";
inline constexpr char kLen[]        = "levitate:len";
inline constexpr char kU[]          = "levitate:u";
inline constexpr char kV[]          = "levitate:v";
inline constexpr char kLarge[]      = "levitate:large";

enum class Kind { None, AndesiteFunnel, BrassFunnel, AndesiteTunnel, BrassTunnel, Chute, SmartChute, Vault, Crate };
[[nodiscard]] Kind kindOf(std::string_view id);
[[nodiscard]] inline bool isFunnel(Kind k) { return k == Kind::AndesiteFunnel || k == Kind::BrassFunnel; }
[[nodiscard]] inline bool isTunnel(Kind k) { return k == Kind::AndesiteTunnel || k == Kind::BrassTunnel; }
[[nodiscard]] inline bool isChute(Kind k) { return k == Kind::Chute || k == Kind::SmartChute; }

/// `config/funnels.json` (Create's CLogistics keys this unit uses, same defaults).
struct Config {
    int version = 1;
    /// Ticks a funnel waits between item transfers.
    int defaultExtractionTimer = 8;
    /// Stacks a vault holds per block.
    int vaultCapacity = 20;
    /// Ticks a brass tunnel waits between distributions.
    int brassTunnelTimer = 10;
    /// Ticks between a chute's air flow checks (Create's CKinetics fanBlockCheckRate).
    int fanBlockCheckRate = 30;
};
[[nodiscard]] Config const& config();

namespace detail {

// ---- world helpers ------------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     toBlockPos(GlobalPos const& p);
[[nodiscard]] GlobalPos    toGlobal(int dim, BlockPos const& p);
[[nodiscard]] BlockPos     offset(BlockPos const& p, int face, int n = 1);
[[nodiscard]] GlobalPos    offset(GlobalPos const& p, int face, int n = 1);
/// The block if its chunk is loaded.
[[nodiscard]] Block const* loadedBlock(BlockSource& region, BlockPos const& pos);
[[nodiscard]] std::string  idAt(BlockSource& region, BlockPos const& pos);
[[nodiscard]] Kind         kindAt(BlockSource& region, BlockPos const& pos);
[[nodiscard]] int          state(Block const& block, char const* name, int fallback = 0);
/// Replaces some of our integer states of the block at `pos` (no-op if unchanged).
bool setStates(BlockSource& region, BlockPos const& pos, std::vector<std::pair<std::string_view, int>> const& states);
/// Create's Level.hasNeighborSignal approximation (see SPEC §8).
[[nodiscard]] bool powered(BlockSource& region, BlockPos const& pos);
/// The face's top / side can carry (Create's canSupportRigidBlock / hasBlockSolidSide).
[[nodiscard]] bool solidFace(BlockSource& region, BlockPos const& pos, int face);
[[nodiscard]] bool replaceableAt(BlockSource& region, BlockPos const& pos);

/// Spawns an item entity (Create's ItemEntity with the default pickup delay) with a velocity.
ItemActor* spawnItem(BlockSource& region, ItemStack const& stack, Vec3 const& pos, Vec3 const& velocity);
/// Item entities whose position lies in the box.
[[nodiscard]] std::vector<ItemActor*> itemsIn(BlockSource& region, Vec3 const& min, Vec3 const& max);
/// Any item entity (or other loose entity) in the box.
void playSound(BlockSource& region, BlockPos const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void giveOrDrop(Player& player, ItemStack const& stack);

// ---- per-block documents ------------------------------------------------------------------------
/// `BlockData[pos]["funnels"]` (nullptr when absent).
[[nodiscard]] Json const* docOf(GlobalPos const& pos);
/// `BlockData[pos]["funnels"]` for writing.
[[nodiscard]] Json& editDoc(GlobalPos const& pos);

/// Extraction like Create's ItemHelper.extract with an ExtractionCountMode: `exact` only extracts
/// when `amount` items of one kind are there.
[[nodiscard]] ItemStack extractCounted(
    BlockSource&                         region,
    BlockPos const&                      pos,
    int                                  face,
    std::function<bool(ItemStack const&)> const& filter,
    int                                  amount,
    bool                                 exact,
    bool                                 simulate
);

/// The amount setting of a brass funnel / smart chute (value box "Extracted Amount").
[[nodiscard]] cf::AmountSetting amountOf(GlobalPos const& pos);
void                            setAmount(GlobalPos const& pos, cf::AmountSetting s);
/// Registers the "Extracted Amount" value box on a block (Create's FilteringBehaviour count).
void registerAmountBox(std::string const& blockId, std::function<std::optional<std::array<double, 3>>(Block const&, int face)> centre);

// ---- per kind -----------------------------------------------------------------------------------
// funnels
void installFunnels();
void tickFunnel(BlockSource& region, GlobalPos const& pos, Block const& block, std::uint64_t tick);
bool useFunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, int face, ItemStack& held);
bool wrenchFunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block);
void placeFunnel(Player* player, BlockSource& region, GlobalPos const& pos, bool sneaking, Vec3 const& look);
/// Inserts into the inventory a funnel is attached to (Create's FunnelBlock.tryInsert).
[[nodiscard]] ItemStack funnelInsert(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, bool simulate);
void                    forgetFunnel(GlobalPos const& pos);
// chutes
void installChutes();
void beginChuteTick();
void tickChute(BlockSource& region, GlobalPos const& pos, Block const& block, std::uint64_t tick);
bool useChute(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, ItemStack const& held);
bool wrenchChute(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block);
void placeChute(Player* player, BlockSource& region, GlobalPos const& pos, int clickedFace, bool sneaking);
void updateChuteShape(BlockSource& region, GlobalPos const& pos);
void forgetChute(GlobalPos const& pos);
[[nodiscard]] ItemStack chuteItem(GlobalPos const& pos);
// tunnels
void installTunnels();
void tickTunnel(BlockSource& region, GlobalPos const& pos, Block const& block, std::uint64_t tick);
bool useTunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, ItemStack const& held);
bool wrenchTunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block);
void updateTunnelShape(BlockSource& region, GlobalPos const& pos);
void forgetTunnel(GlobalPos const& pos);
// vaults
void installVaults();
void tickVaults(std::uint64_t tick);
bool wrenchVault(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, int face);
void placeVault(Player* player, BlockSource& region, GlobalPos const& pos, int clickedFace, bool sneaking);
/// A vault block changed (placed, removed, axis): its structure is formed again next tick.
void vaultChanged(GlobalPos const& pos);
void forgetVaults();
/// The comparator signal of the vault at `pos` (0..15), nullopt if not a vault.
[[nodiscard]] std::optional<int> vaultSignal(GlobalPos const& pos);
/// The vault structure containing `pos` (formed on demand).
[[nodiscard]] std::optional<cf::VaultStructure> vaultStructure(GlobalPos const& pos);
// crate
void installCrate();

// display items (client-only item entities showing items inside blocks)
void        showItem(GlobalPos const& owner, ItemStack const& stack, Vec3 const& pos);
void        hideItem(GlobalPos const& owner);
std::size_t shownItems();
/// Sends the display items to the players around them (once per tick).
void flushDisplays();

/// Contents a broken block leaves behind (drops them at the position next tick).
void dropLater(GlobalPos const& pos, std::vector<ItemStack> stacks);
/// Runs `fn` on the next server tick (world edits after an interaction or a block change).
void defer(std::function<void()> fn);

} // namespace detail

} // namespace lv::feature::funnels
