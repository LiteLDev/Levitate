#include "core/equipment/Gear.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lv::core::equipment {

int backtankFillAmount(float rpm) {
    int v = static_cast<int>(std::floor((std::fabs(rpm) - 100.0f) / 20.0f));
    return std::clamp(v, 1, 5);
}

int backtankFillDelay(float rpm) {
    int v = static_cast<int>(128.0f - std::fabs(rpm) / 5.0f) - 108;
    return std::clamp(v, 0, 20);
}

int airToolCost(int airInBacktank, int usesPerTank) {
    if (usesPerTank <= 0) return 0;
    return std::max(airInBacktank / usesPerTank, 1);
}

int backtankWarning(int before, int after, int maxAir) {
    if (after >= before) return 0;
    if (before > 1 && after <= 1) return 2;
    int low = maxAir / 10;
    if (before > low && after <= low) return 1;
    return 0;
}

int backtankBarWidth(int air, int maxAir) {
    if (maxAir <= 0) return 0;
    return static_cast<int>(std::lround(13.0 * std::clamp(air, 0, maxAir) / maxAir));
}

int backtankBarDamage(int air, int maxAir, int maxDurability) {
    if (maxAir <= 0 || maxDurability <= 1) return 0;
    double frac = 1.0 - static_cast<double>(std::clamp(air, 0, maxAir)) / maxAir;
    int    dmg  = static_cast<int>(std::lround(frac * maxDurability));
    return std::clamp(dmg, 0, maxDurability - 1);
}

std::string backtankTimeLeft(int air) {
    int  secs = std::max(0, air - 1);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
    return buf;
}

namespace {
PotatoType make(std::string id, std::vector<std::string> items, float damage, int reload, float velocity, float knockback, int split,
                float pitch, bool sticky = false, PotatoEffect effect = PotatoEffect::None) {
    PotatoType t;
    t.id        = std::move(id);
    t.items     = std::move(items);
    t.damage    = damage;
    t.reload    = reload;
    t.velocity  = velocity;
    t.knockback = knockback;
    t.split     = split;
    t.pitch     = pitch;
    t.sticky    = sticky;
    t.effect    = effect;
    return t;
}
PotatoType potion(PotatoType t, int effectId, int ticks, int amplifier, bool consumed) {
    t.effectId        = effectId;
    t.effectTicks     = ticks;
    t.effectAmplifier = amplifier;
    t.consumed        = consumed;
    return t;
}
} // namespace

std::vector<PotatoType> const& potatoTypes() {
    using E = PotatoEffect;
    // Bedrock MobEffectIds: 2 slowness, 19 poison; Bedrock has no glowing effect (SPEC §8)
    static std::vector<PotatoType> const types = [] {
        std::vector<PotatoType> v;
        v.push_back(make("potato", {"minecraft:potato"}, 5, 15, 1.25f, 1.5f, 1, 1, false, E::PlantPotato));
        auto baked          = make("baked_potato", {"minecraft:baked_potato"}, 5, 15, 1.25f, 0.5f, 1, 1, false, E::SetOnFire);
        baked.effectSeconds = 3;
        v.push_back(baked);
        v.push_back(make("carrot", {"minecraft:carrot"}, 4, 12, 1.45f, 0.3f, 1, 1.5f, false, E::PlantCarrot));
        v.push_back(make("golden_carrot", {"minecraft:golden_carrot"}, 12, 15, 1.45f, 0.5f, 1, 1.5f));
        v.push_back(make("sweet_berry", {"minecraft:sweet_berries"}, 3, 10, 1.05f, 0.1f, 3, 1.25f));
        v.push_back(potion(make("glow_berry", {"minecraft:glow_berries"}, 2, 10, 1.05f, 0.05f, 2, 1.2f, false, E::Glowing), 0, 200, 0, true));
        v.push_back(make("chocolate_berry", {"levitate:chocolate_glazed_berries"}, 4, 10, 1.05f, 0.2f, 3, 1.25f));
        v.push_back(potion(make("poison_potato", {"minecraft:poisonous_potato"}, 5, 15, 1.25f, 0.05f, 1, 1, false, E::Poison), 19, 160, 0, false));
        v.push_back(make("chorus_fruit", {"minecraft:chorus_fruit"}, 3, 15, 1.2f, 0.05f, 1, 1, false, E::ChorusTeleport));
        v.push_back(make("apple", {"minecraft:apple"}, 5, 10, 1.45f, 0.5f, 1, 1.1f));
        v.push_back(potion(make("honeyed_apple", {"levitate:honeyed_apple"}, 6, 15, 1.35f, 0.1f, 1, 1.1f, false, E::Slowness), 2, 160, 1, false));
        v.push_back(make("golden_apple", {"minecraft:golden_apple"}, 1, 100, 1.45f, 0.05f, 1, 1.1f, false, E::GoldenApple));
        v.push_back(make("enchanted_golden_apple", {"minecraft:enchanted_golden_apple"}, 1, 100, 1.45f, 0.05f, 1, 1.1f, false, E::EnchantedGoldenApple));
        v.push_back(make("beetroot", {"minecraft:beetroot"}, 2, 5, 1.6f, 0.1f, 1, 1.6f));
        v.push_back(make("melon_slice", {"minecraft:melon_slice"}, 3, 8, 1.45f, 0.1f, 1, 1.5f));
        v.push_back(potion(make("glistering_melon", {"minecraft:glistering_melon_slice"}, 5, 8, 1.45f, 0.1f, 1, 1.5f, false, E::Glowing), 0, 100, 0, false));
        v.push_back(make("melon_block", {"minecraft:melon_block"}, 8, 20, 0.95f, 2.0f, 1, 0.9f, false, E::PlaceMelon));
        v.push_back(make("pumpkin_block", {"minecraft:pumpkin"}, 6, 15, 0.95f, 2.0f, 1, 0.9f, false, E::PlacePumpkin));
        v.push_back(make("pumpkin_pie", {"minecraft:pumpkin_pie"}, 7, 15, 1.1f, 0.05f, 1, 1.1f, true));
        v.push_back(make("cake", {"minecraft:cake"}, 8, 15, 1.1f, 0.1f, 1, 1, true));
        auto blaze          = make("blaze_cake", {"levitate:blaze_cake"}, 15, 20, 1.1f, 0.3f, 1, 1, true, E::SetOnFire);
        blaze.effectSeconds = 12;
        v.push_back(blaze);
        v.push_back(make("fish", {"minecraft:cod", "minecraft:cooked_cod", "minecraft:salmon", "minecraft:cooked_salmon", "minecraft:tropical_fish"},
                         4, 10, 1.3f, 0.6f, 1, 1.3f, true));
        v.push_back(make("pufferfish", {"minecraft:pufferfish"}, 4, 10, 1.1f, 0.4f, 1, 1.1f, true, E::Pufferfish));
        v.push_back(make("suspicious_stew", {"minecraft:suspicious_stew"}, 3, 40, 0.8f, 0.2f, 1, 1, false, E::SuspiciousStew));
        for (auto& t : v) {
            // Create's renderTowardMotion(140, spin); every other built-in type tumbles
            if (t.id == "carrot" || t.id == "fish" || t.id == "pufferfish" || t.id == "suspicious_stew") t.render = PotatoRender::TowardMotion, t.spin = 1;
            if (t.id == "golden_carrot" || t.id == "beetroot") t.render = PotatoRender::TowardMotion, t.spin = 2;
            // entity hit actions that report the item as used up: FoodEffects(recoverable = false) and
            // the golden apple's CureZombieVillager (cure, else golden apple food effects)
            if (t.effect == E::GoldenApple || t.effect == E::EnchantedGoldenApple || t.effect == E::Pufferfish) t.consumed = true;
            if (t.effect == E::SuspiciousStew) t.dropItem = "minecraft:bowl";
        }
        return v;
    }();
    return types;
}

std::vector<std::string> const& potatoAmmoItems() {
    static std::vector<std::string> const list = [] {
        std::vector<std::string> v;
        for (auto const& t : potatoTypes())
            for (auto const& i : t.items) v.push_back(i);
        return v;
    }();
    return list;
}

int potatoAmmoIndex(std::string_view itemId) {
    auto const& l = potatoAmmoItems();
    for (std::size_t i = 0; i < l.size(); ++i)
        if (l[i] == itemId) return static_cast<int>(i);
    return -1;
}

double potatoStuckStep(double offsetY, double& fallSpeed, PotatoType const& type) {
    fallSpeed += 0.007 * type.gravity;
    return offsetY - fallSpeed;
}

bool potatoStuckPops(double projectileY, double entityY) { return projectileY < entityY - 0.1; }

double potatoYaw(Vec3f const& m) { return -std::atan2(m.x, m.z) * 180.0 / 3.14159265358979323846; }

double potatoPitch(Vec3f const& m) {
    double h = std::sqrt(m.x * m.x + m.z * m.z);
    return -std::atan2(m.y, h) * 180.0 / 3.14159265358979323846;
}

// ---- enchantments --------------------------------------------------------------------------------------

int enchantMinCost(int level) { return 15 + 15 * (level - 1); }
int enchantMaxCost(int level) { return 45 + 15 * (level - 1); }

int enchantLevelForCost(int cost) {
    for (int l = kEnchantMaxLevel; l >= 1; --l)
        if (cost >= enchantMinCost(l) && cost <= enchantMaxCost(l)) return l;
    return 0;
}

std::string enchantLevelName(int level) {
    static constexpr char const* names[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    if (level >= 1 && level <= 10) return names[level - 1];
    return std::to_string(level);
}

int backtankMaxAir(int airInBacktank, int perLevel, int capacityLevel) { return airInBacktank + perLevel * std::max(0, capacityLevel); }

int mergeEnchantLevel(int a, int b, int maxLevel) {
    if (a == b) return std::min(a + 1, maxLevel);
    return std::max(a, b);
}

void JavaRandom::setSeed(std::int64_t seed) { mSeed = static_cast<std::int64_t>((static_cast<std::uint64_t>(seed) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)); }

int JavaRandom::next(int bits) {
    // Java's long arithmetic wraps: unsigned here (signed overflow is undefined in C++)
    auto s = (static_cast<std::uint64_t>(mSeed) * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
    mSeed  = static_cast<std::int64_t>(s);
    // Java: (int)(seed >>> (48 - bits)), the low 32 bits as a signed int
    return static_cast<int>(static_cast<std::uint32_t>(s >> (48 - bits)));
}

int JavaRandom::nextInt(int bound) {
    if (bound <= 0) return 0;
    if ((bound & -bound) == bound) return static_cast<int>((static_cast<std::int64_t>(bound) * next(31)) >> 31);
    int bits, val;
    do {
        bits = next(31);
        val  = bits % bound;
    } while (static_cast<std::int32_t>(static_cast<std::uint32_t>(bits) - static_cast<std::uint32_t>(val) + static_cast<std::uint32_t>(bound - 1)) < 0);
    return val;
}

float JavaRandom::nextFloat() { return static_cast<float>(next(24)) / static_cast<float>(1 << 24); }

namespace {
/// Java's WeightedRandom#getRandomItem.
std::optional<EnchantCandidate> weightedPick(JavaRandom& random, std::vector<EnchantCandidate> const& list) {
    int total = 0;
    for (auto const& c : list) total += c.weight;
    if (total <= 0) return std::nullopt;
    int r = random.nextInt(total);
    for (auto const& c : list) {
        r -= c.weight;
        if (r < 0) return c;
    }
    return std::nullopt;
}
} // namespace

std::vector<EnchantCandidate> selectEnchantments(
    JavaRandom&                                                                  random,
    int                                                                          enchantability,
    int                                                                          cost,
    std::function<std::vector<EnchantCandidate>(int)> const&                     available,
    std::function<bool(EnchantCandidate const&, EnchantCandidate const&)> const& compatible
) {
    std::vector<EnchantCandidate> out;
    if (enchantability <= 0) return out;
    cost += 1 + random.nextInt(enchantability / 4 + 1) + random.nextInt(enchantability / 4 + 1);
    float f = (random.nextFloat() + random.nextFloat() - 1.0f) * 0.15f;
    cost    = std::max(1, static_cast<int>(std::lround(static_cast<float>(cost) + static_cast<float>(cost) * f)));
    auto list = available(cost);
    if (list.empty()) return out;
    if (auto c = weightedPick(random, list)) out.push_back(*c);
    while (random.nextInt(50) <= cost) {
        if (!out.empty()) {
            auto const& last = out.back();
            std::erase_if(list, [&](EnchantCandidate const& c) { return c.key == last.key || !compatible(last, c); });
        }
        if (list.empty()) break;
        if (auto c = weightedPick(random, list)) out.push_back(*c);
        cost /= 2;
    }
    return out;
}

PotatoType const* potatoTypeFor(std::string_view itemId) {
    for (auto const& t : potatoTypes())
        for (auto const& i : t.items)
            if (i == itemId) return &t;
    return nullptr;
}

int potatoCooldown(PotatoType const& type, bool dualWield) { return dualWield ? type.reload * 2 / 3 : type.reload; }

Vec3f potatoStep(Vec3f v, PotatoType const& type) {
    v.y -= 0.05 * type.gravity;
    v.x *= type.drag;
    v.y *= type.drag;
    v.z *= type.drag;
    return v;
}

double potatoSpeed(PotatoType const& type) { return 2.0 * type.velocity; }

double potatoRecoveryChance(int recoveryLevel) { return recoveryLevel > 0 ? 0.125 + 0.125 * recoveryLevel : 0.0; }

std::string_view controllerButtonName(int button) {
    static constexpr std::string_view names[] = {"forward", "back", "left", "right", "jump", "sneak"};
    if (button < 0 || button >= kControllerButtons) return "";
    return names[button];
}

std::string frequencyKey(std::string_view first, std::string_view second) {
    return std::string(first) + "|" + std::string(second);
}

} // namespace lv::core::equipment
