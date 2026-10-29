#include "core/fluids/Fluids.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace lv::core::fluids {

// ---- bottles and potions -----------------------------------------------------------------------

std::string_view bottleName(Bottle b) {
    switch (b) {
    case Bottle::Splash:
        return "splash";
    case Bottle::Lingering:
        return "lingering";
    default:
        return "regular";
    }
}

std::optional<Bottle> bottleFromName(std::string_view name) {
    if (name == "regular") return Bottle::Regular;
    if (name == "splash") return Bottle::Splash;
    if (name == "lingering") return Bottle::Lingering;
    return std::nullopt;
}

std::string_view bottleItem(Bottle b) {
    switch (b) {
    case Bottle::Splash:
        return "minecraft:splash_potion";
    case Bottle::Lingering:
        return "minecraft:lingering_potion";
    default:
        return "minecraft:potion";
    }
}

std::optional<Bottle> bottleOfItem(std::string_view itemId) {
    if (itemId == "minecraft:potion") return Bottle::Regular;
    if (itemId == "minecraft:splash_potion") return Bottle::Splash;
    if (itemId == "minecraft:lingering_potion") return Bottle::Lingering;
    return std::nullopt;
}

std::vector<PotionType> const& potionTypes() {
    // Bedrock potion aux values; names as Java's potion registry (Create's recipes use them).
    static std::vector<PotionType> const t{
        {0,  "water",                0x385DC6},
        {1,  "mundane",              0x385DC6},
        {2,  "long_mundane",         0x385DC6},
        {3,  "thick",                0x385DC6},
        {4,  "awkward",              0x385DC6},
        {5,  "night_vision",         0x1F1FA1},
        {6,  "long_night_vision",    0x1F1FA1},
        {7,  "invisibility",         0x7F8392},
        {8,  "long_invisibility",    0x7F8392},
        {9,  "leaping",              0x22FF4C},
        {10, "long_leaping",         0x22FF4C},
        {11, "strong_leaping",       0x22FF4C},
        {12, "fire_resistance",      0xE49A3A},
        {13, "long_fire_resistance", 0xE49A3A},
        {14, "swiftness",            0x7CAFC6},
        {15, "long_swiftness",       0x7CAFC6},
        {16, "strong_swiftness",     0x7CAFC6},
        {17, "slowness",             0x5A6C81},
        {18, "long_slowness",        0x5A6C81},
        {19, "water_breathing",      0x2E5299},
        {20, "long_water_breathing", 0x2E5299},
        {21, "healing",              0xF82423},
        {22, "strong_healing",       0xF82423},
        {23, "harming",              0x430A09},
        {24, "strong_harming",       0x430A09},
        {25, "poison",               0x4E9331},
        {26, "long_poison",          0x4E9331},
        {27, "strong_poison",        0x4E9331},
        {28, "regeneration",         0xCD5CAB},
        {29, "long_regeneration",    0xCD5CAB},
        {30, "strong_regeneration",  0xCD5CAB},
        {31, "strength",             0x932423},
        {32, "long_strength",        0x932423},
        {33, "strong_strength",      0x932423},
        {34, "weakness",             0x484D48},
        {35, "long_weakness",        0x484D48},
        {36, "decay",                0x352A27},
        {37, "turtle_master",        0x7B6F7A},
        {38, "long_turtle_master",   0x7B6F7A},
        {39, "strong_turtle_master", 0x7B6F7A},
        {40, "slow_falling",         0xFFEFD1},
        {41, "long_slow_falling",    0xFFEFD1},
        {42, "strong_slowness",      0x5A6C81},
        {43, "wind_charged",         0xBDC9FF},
        {44, "weaving",              0x78695A},
        {45, "oozing",               0x99FFA3},
        {46, "infested",             0x8C9B8C},
    };
    return t;
}

PotionType const* potionByAux(int aux) {
    for (auto const& p : potionTypes())
        if (p.aux == aux) return &p;
    return nullptr;
}

PotionType const* potionByName(std::string_view name) {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    for (auto const& p : potionTypes())
        if (p.name == name) return &p;
    return nullptr;
}

std::string potionFluid(Bottle bottle, std::string_view potion) {
    if (potion.starts_with("minecraft:")) potion.remove_prefix(10);
    return std::string(kPotionPrefix) + std::string(bottleName(bottle)) + "/" + std::string(potion);
}

std::optional<PotionFluid> parsePotionFluid(std::string_view fluid) {
    if (!fluid.starts_with(kPotionPrefix)) return std::nullopt;
    fluid.remove_prefix(kPotionPrefix.size());
    auto slash = fluid.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    auto bottle = bottleFromName(fluid.substr(0, slash));
    auto potion = fluid.substr(slash + 1);
    if (!bottle || !potionByName(potion)) return std::nullopt;
    return PotionFluid{*bottle, std::string(potion)};
}

std::string fluidOfPotionItem(std::string_view itemId, int aux) {
    auto bottle = bottleOfItem(itemId);
    auto const* p = potionByAux(aux);
    if (!bottle || !p) return {};
    if (*bottle == Bottle::Regular && p->aux == 0) return std::string(kWater);
    return potionFluid(*bottle, p->name);
}

std::optional<ItemRef> bottleFor(std::string_view fluid) {
    if (fluid == kWater) return ItemRef{"minecraft:potion", 0};
    auto pf = parsePotionFluid(fluid);
    if (!pf) return std::nullopt;
    auto const* p = potionByName(pf->potion);
    if (!p) return std::nullopt;
    return ItemRef{std::string(bottleItem(pf->bottle)), p->aux};
}

std::uint32_t fluidColor(std::string_view fluid) {
    if (fluid == kWater) return 0x3F76E4;
    if (fluid == kLava) return 0xE0601C;
    if (fluid == kMilk) return 0xF4F4F0;
    if (fluid == kHoney) return 0xEAAE2F;
    if (fluid == kChocolate) return 0x622020;
    if (fluid == kTea) return 0xA9774A;
    if (fluid == "minecraft:powder_snow") return 0xF0F6FF;
    if (auto pf = parsePotionFluid(fluid))
        if (auto const* p = potionByName(pf->potion)) return p->color;
    return 0x808080;
}

std::string fluidNameKey(std::string_view fluid) {
    if (isPotionFluid(fluid)) return "fluid_type.create.potion";
    auto colon = fluid.find(':');
    std::string_view ns   = colon == std::string_view::npos ? std::string_view("minecraft") : fluid.substr(0, colon);
    std::string_view path = colon == std::string_view::npos ? fluid : fluid.substr(colon + 1);
    if (ns == "levitate") return "fluid_type.create." + std::string(path);
    return "fluid_type." + std::string(ns) + "." + std::string(path);
}

// ---- item drain ------------------------------------------------------------------------------

DrainItem insertDrainItem(int fromFace) {
    if (fromFace < 0) fromFace = 1; // from above
    DrainItem item;
    item.travel           = oppositeFace(fromFace);
    item.beltPosition     = verticalFace(fromFace) ? .5f : 0.f;
    item.prevBeltPosition = item.beltPosition;
    item.checked          = false;
    return item;
}

DrainStep stepDrainItem(DrainItem& item) {
    item.prevBeltPosition = item.beltPosition;
    item.beltPosition += kDrainItemSpeed;
    if (!item.checked && item.beltPosition >= .5f) {
        item.checked      = true;
        item.beltPosition = .5f;
        return DrainStep::ReachedCentre;
    }
    if (item.beltPosition > 1) {
        item.beltPosition = 1;
        return DrainStep::AtOutput;
    }
    return DrainStep::Move;
}

// ---- portable storage interface ----------------------------------------------------------------

void PsiTimer::start() {
    connected     = true;
    transferTimer = timeout + kPsiAnimation * 2;
}

void PsiTimer::transferred() {
    if (!connected) return;
    transferTimer = timeout + kPsiAnimation;
}

bool PsiTimer::tick(bool powered) {
    auto stop = [&] {
        bool was  = connected;
        connected = false;
        return was;
    };
    if (keepAlive > 0 && --keepAlive == 0) {
        transferTimer = kPsiAnimation - 1; // the contraption left: close
        return stop();
    }
    transferTimer = std::min(transferTimer, kPsiAnimation * 2 + timeout);
    bool canDecrement = transferTimer > kPsiAnimation || (transferTimer > 0 && keepAlive == 0);
    if (canDecrement) {
        --transferTimer;
        if (transferTimer <= 0 || powered) return stop();
    } else if (powered && connected) {
        return stop();
    }
    return false;
}

bool PsiTimer::isConnected() const {
    return connected && transferTimer >= kPsiAnimation && transferTimer <= timeout + kPsiAnimation;
}

float PsiTimer::progress() const {
    if (isConnected()) return 1;
    if (transferTimer >= timeout + kPsiAnimation)
        return std::lerp(1.f, 0.f, float(transferTimer - timeout - kPsiAnimation) / kPsiAnimation);
    if (transferTimer < kPsiAnimation) return std::lerp(0.f, 1.f, float(transferTimer) / kPsiAnimation);
    return 0;
}

// ---- hose pulley -------------------------------------------------------------------------------

int hoseEndDepth(float offset) { return static_cast<int>(std::ceil(offset)); }

HoseStep stepHose(float offset, float rpm, std::function<bool(int)> const& passable) {
    HoseStep s;
    s.moving        = rpm != 0;
    float newOffset = offset + linearSpeed(rpm);
    if (newOffset < 0) {
        newOffset = 0;
        s.moving  = false;
    }
    int depth = hoseEndDepth(newOffset);
    if (depth > 0 && !passable(depth)) {
        newOffset = static_cast<float>(static_cast<int>(newOffset));
        s.moving  = false;
    }
    s.offset = newOffset;
    return s;
}

// ---- fluid bodies ------------------------------------------------------------------------------

namespace {
constexpr int kDirs[6][3]{
    {0,  -1, 0 },
    {0,  1,  0 },
    {0,  0,  -1},
    {0,  0,  1 },
    {-1, 0,  0 },
    {1,  0,  0 },
};

long long distSq(Pos const& a, Pos const& b) {
    long long dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
} // namespace

DrainPlan planDrain(Pos const& root, CellQuery const& at, int range, int threshold) {
    DrainPlan plan;
    auto      rc = at(root);
    if (rc.fluid.empty()) return plan;
    plan.fluid = rc.fluid;
    long long const rangeSq = static_cast<long long>(range) * range;

    std::set<Pos>   visited{root};
    std::deque<Pos> frontier{root};
    int             found = 0; ///< fluid blocks of the body seen so far
    while (!frontier.empty()) {
        Pos p = frontier.front();
        frontier.pop_front();
        auto c = p == root ? rc : at(p);
        if (c.fluid != plan.fluid) continue;
        if (c.source) plan.sources.push_back(p);
        if (++found > threshold) {
            plan.infinite = true;
            break;
        }
        for (int d = 0; d < 6; ++d) {
            if (d == 0) continue; // the hose drains what is at or above its end, never below it
            Pos n{p.x + kDirs[d][0], p.y + kDirs[d][1], p.z + kDirs[d][2]};
            if (distSq(n, root) > rangeSq || visited.contains(n)) continue;
            visited.insert(n);
            if (at(n).fluid == plan.fluid) frontier.push_back(n);
        }
    }
    std::stable_sort(plan.sources.begin(), plan.sources.end(), [&](Pos const& a, Pos const& b) {
        if (a.y != b.y) return a.y > b.y;
        return distSq(a, root) > distSq(b, root);
    });
    return plan;
}

FillPlan planFill(Pos const& root, std::string_view fluid, CellQuery const& at, int range, int threshold) {
    FillPlan plan;
    long long const rangeSq = static_cast<long long>(range) * range;
    auto            usable  = [&](Cell const& c) { return c.fillable || (c.fluid == fluid && c.source); };
    if (!usable(at(root))) return plan;

    std::set<Pos>   visited{root};
    std::deque<Pos> frontier{root};
    int             body = 0;
    while (!frontier.empty()) {
        Pos p = frontier.front();
        frontier.pop_front();
        auto c = at(p);
        if (c.fluid == fluid && c.source) ++body;
        else if (c.fillable) plan.targets.push_back(p);
        else continue;
        if (body > threshold) {
            plan.infinite = true;
            break;
        }
        for (int d = 0; d < 6; ++d) {
            if (d == 1) continue; // never upwards
            Pos n{p.x + kDirs[d][0], p.y + kDirs[d][1], p.z + kDirs[d][2]};
            if (distSq(n, root) > rangeSq || visited.contains(n)) continue;
            visited.insert(n);
            if (usable(at(n))) frontier.push_back(n);
        }
        if (static_cast<int>(visited.size()) > threshold * 2 + 64) break; // bounded work per search
    }
    std::stable_sort(plan.targets.begin(), plan.targets.end(), [&](Pos const& a, Pos const& b) {
        if (a.y != b.y) return a.y < b.y;
        return distSq(a, root) < distSq(b, root);
    });
    return plan;
}

} // namespace lv::core::fluids
