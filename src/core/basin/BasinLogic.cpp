#include "core/basin/BasinLogic.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::basin {

// ---- blaze burner --------------------------------------------------------------------------------

HeatLevel burnerHeat(BurnerState const& s) {
    switch (s.fuel) {
    case Fuel::Special:
        return HeatLevel::Seething;
    case Fuel::Normal:
        return static_cast<double>(s.burnTime) / kMaxHeatCapacity < 0.0125 ? HeatLevel::Fading : HeatLevel::Kindled;
    default:
        return HeatLevel::Smouldering;
    }
}

bool tryUpdateFuel(BurnerState& s, FuelOffer offer, bool forceOverflow, bool simulate) {
    if (s.creative) return false;
    if (offer.fuel == Fuel::None) return false;
    if (static_cast<int>(offer.fuel) < static_cast<int>(s.fuel)) return false;
    int burn = offer.burnTime;
    if (offer.fuel == s.fuel) {
        if (s.burnTime <= kInsertionThreshold) {
            burn += s.burnTime;
        } else if (forceOverflow && offer.fuel == Fuel::Normal) {
            burn = s.burnTime < kMaxHeatCapacity ? std::min(s.burnTime + burn, kMaxHeatCapacity) : s.burnTime;
        } else {
            return false;
        }
    }
    if (simulate) return true;
    s.fuel     = offer.fuel;
    s.burnTime = burn;
    return true;
}

void tickBurner(BurnerState& s) {
    if (s.creative) return;
    if (s.burnTime > 0) --s.burnTime;
    if (s.burnTime > 0) return;
    if (s.fuel == Fuel::Special) {
        s.fuel     = Fuel::Normal;
        s.burnTime = kMaxHeatCapacity / 2;
    } else {
        s.fuel = Fuel::None;
    }
}

HeatLevel nextActiveLevel(HeatLevel h) { return static_cast<HeatLevel>(static_cast<int>(h) % 4 + 1); }

HeatLevel creativeNext(HeatLevel current) {
    auto next = nextActiveLevel(current);
    if (next == HeatLevel::Fading) next = nextActiveLevel(next);
    return next;
}

int burnerComparator(HeatLevel h) { return std::max(0, static_cast<int>(h) - 1); }

// ---- press ---------------------------------------------------------------------------------------

float pressModeHeadOffset(PressMode m) {
    switch (m) {
    case PressMode::Belt:
        return 19.f / 16.f;
    case PressMode::Basin:
        return 22.f / 16.f;
    default:
        return 1.f;
    }
}

int pressRunningTickSpeed(float speed) {
    if (speed == 0) return 0;
    float t = std::clamp(std::abs(speed) / 512.f, 0.f, 1.f);
    return static_cast<int>(1.f + t * (60.f - 1.f));
}

float pressHeadOffset(bool running, int prevRunningTicks, int runningTicks, float partialTicks) {
    if (!running) return 0;
    int   ticksNow = std::abs(runningTicks);
    float ticks    = static_cast<float>(prevRunningTicks) + partialTicks * static_cast<float>(ticksNow - prevRunningTicks);
    if (ticksNow < (kPressCycle * 2) / 3) {
        return std::clamp(static_cast<float>(std::pow(ticks / kPressCycle * 2, 3)), 0.f, 1.f);
    }
    return std::clamp((kPressCycle - ticks) / kPressCycle * 3, 0.f, 1.f);
}

// ---- mixer ---------------------------------------------------------------------------------------

namespace {
/// Minecraft's Mth.log2 for ints: floor(log2(v)) (0 for v <= 1).
int floorLog2(int v) {
    int r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}
} // namespace

int mixerProcessingTicks(float speed, int processingTime) {
    float recipeSpeed = processingTime != 0 ? static_cast<float>(processingTime) / 100.f : 1.f;
    float s           = std::abs(speed);
    int   ratio       = s > 0 ? static_cast<int>(512.f / s) : 0;
    int   ticks       = floorLog2(ratio) * static_cast<int>(std::ceil(recipeSpeed * 15)) + 1;
    return std::max(ticks, 1);
}

float mixerHeadOffset(bool running, int runningTicks, float partialTicks) {
    float offset = 0;
    auto  ease   = [](float num) { return (2 - std::cos(num * std::numbers::pi_v<float>)) / 2; };
    if (running) {
        if (runningTicks < 20) {
            offset = ease((runningTicks + partialTicks) / 20.f) - .5f;
        } else if (runningTicks <= 20) {
            offset = 1;
        } else {
            offset = ease((40 - runningTicks - partialTicks) / 20.f) - .5f;
        }
    }
    return offset + 7.f / 16.f;
}

float mixerHeadSpeed(float speed, bool running, int runningTicks) {
    if (running) {
        if (runningTicks < 15) return speed;
        if (runningTicks <= 20) return speed * 2;
        return speed;
    }
    return speed / 2;
}

// ---- tanks ---------------------------------------------------------------------------------------

int fillTanks(std::vector<Fluid>& tanks, Fluid const& stack, int capacity, bool simulate) {
    if (stack.empty()) return 0;
    // enforced variety: the tank holding this fluid, else the first empty one
    int target = -1;
    for (int i = 0; i < static_cast<int>(tanks.size()); ++i) {
        if (!tanks[i].empty() && tanks[i].id == stack.id) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        for (int i = 0; i < static_cast<int>(tanks.size()); ++i) {
            if (tanks[i].empty()) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) return 0;
    auto& t      = tanks[target];
    int   room   = capacity - (t.empty() ? 0 : t.amount);
    int   accept = std::clamp(stack.amount, 0, std::max(room, 0));
    if (!simulate && accept > 0) {
        if (t.empty()) t = Fluid{stack.id, 0};
        t.amount += accept;
    }
    return accept;
}

Fluid drainTanks(std::vector<Fluid>& tanks, std::string const& id, int amount, bool simulate) {
    for (auto& t : tanks) {
        if (t.empty()) continue;
        if (!id.empty() && t.id != id) continue;
        int   n   = std::min(amount, t.amount);
        Fluid out{t.id, n};
        if (!simulate) {
            t.amount -= n;
            if (t.amount <= 0) t = Fluid{};
        }
        return n > 0 ? out : Fluid{};
    }
    return {};
}

// ---- crafting ------------------------------------------------------------------------------------

bool canCompress(std::vector<std::string> const& keys) {
    if (keys.size() != 4 && keys.size() != 9) return false;
    if (keys.front().empty()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](std::string const& k) { return k == keys.front(); });
}

bool ignoredInAutomation(std::string const& recipeId) {
    static constexpr std::string_view suffix = "_manual_only";
    return recipeId.size() >= suffix.size() && recipeId.compare(recipeId.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int redstoneFromInventory(std::vector<int> const& counts, std::vector<int> const& maxStacks) {
    if (counts.empty()) return 0;
    float f    = 0;
    int   used = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] <= 0) continue;
        int max = i < maxStacks.size() && maxStacks[i] > 0 ? maxStacks[i] : 64;
        f += static_cast<float>(counts[i]) / static_cast<float>(max);
        ++used;
    }
    f /= static_cast<float>(counts.size());
    return static_cast<int>(std::floor(f * 14.f)) + (used > 0 ? 1 : 0);
}

} // namespace lv::core::basin
