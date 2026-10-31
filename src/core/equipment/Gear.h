#pragma once

// Numbers of Create's worn / held equipment (U31, SPEC §3.6.31), independent of the game:
// backtank air, potato cannon projectile types and ballistics, linked controller buttons.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lv::core::equipment {

// ---- backtank (Create's BacktankBlockEntity / BacktankUtil) ---------------------------------------

/// Air added per fill step by a placed backtank turning at `rpm`: clamp(floor((|rpm|-100)/20), 1, 5).
[[nodiscard]] int backtankFillAmount(float rpm);
/// Ticks waited after each fill step: clamp(int(128 - |rpm|/5) - 108, 0, 20).
[[nodiscard]] int backtankFillDelay(float rpm);
/// Air one use of an air tool costs: max(airInBacktank / usesPerTank, 1) (0 uses: free).
[[nodiscard]] int airToolCost(int airInBacktank, int usesPerTank);
/// Create's pressure warnings when air goes from `before` to `after`: 0 none, 1 low (≤ max/10),
/// 2 depleted (≤ 1).
[[nodiscard]] int backtankWarning(int before, int after, int maxAir);
/// Width of the item bar (0..13).
[[nodiscard]] int backtankBarWidth(int air, int maxAir);
/// The durability value that shows `air` as the item bar of an item with `maxDurability`
/// (never the full damage, so the item does not break).
[[nodiscard]] int backtankBarDamage(int air, int maxAir, int maxDurability);
/// The remaining time label "mm:ss" for `air` (Create's (air - 1) seconds).
[[nodiscard]] std::string backtankTimeLeft(int air);

// ---- potato cannon (Create's PotatoCannonProjectileType) -------------------------------------------

/// How the client draws a projectile (Create's AllPotatoProjectileRenderModes).
enum class PotatoRender {
    Tumble,       ///< billboard + spin around Z (2·r16 °/tick) and X (r32 °/tick)
    TowardMotion, ///< along the motion, spinning `spin`·20 °/tick about it (sprite angle offset 140°)
};

enum class PotatoEffect {
    None,
    PlantPotato,
    PlantCarrot,
    SetOnFire,     ///< `effectSeconds` of fire before hitting
    Glowing,       ///< potion effects: `effectId` (Bedrock MobEffectIds), `effectTicks`, `effectAmplifier`
    Poison,
    Slowness,
    ChorusTeleport,
    GoldenApple,   ///< cures zombie villagers, else golden apple effects
    EnchantedGoldenApple,
    PlaceMelon,
    PlacePumpkin,
    Pufferfish,
    SuspiciousStew,
};

struct PotatoType {
    std::string              id; ///< Create's type name
    std::vector<std::string> items;
    float                    damage    = 1;
    int                      reload    = 10;
    float                    velocity  = 1;
    float                    knockback = 1;
    int                      split     = 1;
    float                    pitch     = 1;
    bool                     sticky    = false;
    float                    drag      = 0.99f;
    float                    gravity   = 1;
    PotatoEffect             effect    = PotatoEffect::None;
    int                      effectId = 0, effectTicks = 0, effectAmplifier = 0, effectSeconds = 0;
    bool                     consumed = false; ///< an entity hit uses the item up (no recovery roll, no drop)
    PotatoRender             render   = PotatoRender::Tumble;
    int                      spin     = 1; ///< TowardMotion spin
    std::string              dropItem;     ///< Create's dropStack: dropped when not recovered ("" none)
};

/// Create's built-in projectile types (data/create/potato_projectile/type).
[[nodiscard]] std::vector<PotatoType> const& potatoTypes();
/// The type an ammo item fires (nullptr: not ammo).
[[nodiscard]] PotatoType const* potatoTypeFor(std::string_view itemId);
/// Cooldown after a shot (×2/3 with a cannon in the other hand).
[[nodiscard]] int potatoCooldown(PotatoType const& type, bool dualWield);

struct Vec3f {
    double x = 0, y = 0, z = 0;
};
/// One tick of projectile motion: v = (v - (0, 0.05·gravity, 0)) · drag.
[[nodiscard]] Vec3f potatoStep(Vec3f v, PotatoType const& type);
/// Muzzle speed (blocks / tick): 2 · velocity multiplier.
[[nodiscard]] double potatoSpeed(PotatoType const& type);
/// Chance a projectile item is recovered (Create's PotatoProjectileEntity#setEnchantmentEffectsFromCannon):
/// 0.125 + 0.125 · Potato Recovery level with the enchantment, else 0.
[[nodiscard]] double potatoRecoveryChance(int recoveryLevel);
/// Every ammo item in a fixed order (the projectile entity's variant indexes its texture array).
[[nodiscard]] std::vector<std::string> const& potatoAmmoItems();
/// Index of an ammo item in potatoAmmoItems() (-1: not ammo).
[[nodiscard]] int potatoAmmoIndex(std::string_view itemId);
/// Create's stuck projectile: each tick the fall speed grows by 0.007 · gravity and the offset to the
/// entity drops by it. Returns the new offset y (and updates `fallSpeed`).
[[nodiscard]] double potatoStuckStep(double offsetY, double& fallSpeed, PotatoType const& type);
/// Create's pop of a stuck projectile: once it is below the entity's feet - 0.1.
[[nodiscard]] bool potatoStuckPops(double projectileY, double entityY);
/// Yaw / pitch of a motion in degrees (Bedrock's actor rotation: yaw 0 = +z, 90 = -x; pitch > 0 = down).
[[nodiscard]] double potatoYaw(Vec3f const& motion);
[[nodiscard]] double potatoPitch(Vec3f const& motion);

// ---- enchantments (Create's AllEnchantments: Capacity, Potato Recovery) ------------------------------

/// Create's two enchantments: weight 10, levels 1..3, costs 15 + 15·(L-1) .. 45 + 15·(L-1).
inline constexpr int kEnchantWeight   = 10;
inline constexpr int kEnchantMaxLevel = 3;
[[nodiscard]] int    enchantMinCost(int level);
[[nodiscard]] int    enchantMaxCost(int level);
/// The level of an enchantment with Create's costs available at `cost` (0: none), as Java's
/// getAvailableEnchantmentResults (the highest level whose range holds the cost).
[[nodiscard]] int enchantLevelForCost(int cost);
/// "I", "II", "III", ... (Java's enchantment.level.N).
[[nodiscard]] std::string enchantLevelName(int level);
/// Create's BacktankUtil#maxAir: airInBacktank + enchantedBacktankCapacity · Capacity level.
[[nodiscard]] int backtankMaxAir(int airInBacktank, int perLevel, int capacityLevel);
/// Java's anvil merge of one enchantment: equal levels go up by one (to the max), else the higher.
[[nodiscard]] int mergeEnchantLevel(int a, int b, int maxLevel = kEnchantMaxLevel);

/// java.util.Random (the enchanting table rolls with the player's enchantment seed + option).
class JavaRandom {
public:
    explicit JavaRandom(std::int64_t seed) { setSeed(seed); }
    void  setSeed(std::int64_t seed);
    int   next(int bits);
    int   nextInt(int bound);
    float nextFloat();

private:
    std::int64_t mSeed = 0;
};

/// One enchantment the table can give at a cost (Java's EnchantmentInstance with its weight).
struct EnchantCandidate {
    std::string key; ///< any id ("capacity", or a vanilla enchantment type number)
    int         level  = 1;
    int         weight = 1;
};
/// Java's EnchantmentHelper#selectEnchantment: modifies `cost` by the enchantability, picks weighted
/// enchantments from `available(modifiedCost)` while `nextInt(50) <= cost` (halved after each pick),
/// dropping the ones `compatible(last, other)` rejects. Returns the picks in order.
[[nodiscard]] std::vector<EnchantCandidate> selectEnchantments(
    JavaRandom&                                                                  random,
    int                                                                          enchantability,
    int                                                                          cost,
    std::function<std::vector<EnchantCandidate>(int)> const&                     available,
    std::function<bool(EnchantCandidate const&, EnchantCandidate const&)> const& compatible
);

// ---- netherite diving boots in lava ---------------------------------------------------------------

/// Create's DivingBootsItem#getMovementMultiplier (applied by LavaSwimmingMixin after each lava
/// travel step of a wearer of netherite diving boots): the factors for the motion (x, y, z).
/// `lavaGrounded` is the "LavaGrounded" marker the step reads and updates.
[[nodiscard]] Vec3f lavaBootsMultiplier(double yMotion, bool onGround, bool jumping, bool sprinting, bool eyeInLava, bool& lavaGrounded);

// ---- linked controller -----------------------------------------------------------------------------

/// The six buttons in Create's order: forward, back, left, right, jump, sneak.
inline constexpr int kControllerButtons = 6;
[[nodiscard]] std::string_view controllerButtonName(int button);
/// Frequency key of two frequency item ids (Create's Couple<Frequency>; empty items are "").
[[nodiscard]] std::string frequencyKey(std::string_view first, std::string_view second);

} // namespace lv::core::equipment
