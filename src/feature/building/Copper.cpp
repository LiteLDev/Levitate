// Building unit: copper shingles and tiles age like vanilla copper (Java WeatheringCopper /
// ChangeOverTimeBlock), an axe scrapes one stage off or removes the wax, honeycomb waxes.
//
// Custom Bedrock blocks get no random ticks, so the unit keeps the positions of its unwaxed,
// not fully oxidized copper blocks (saved with the world) and gives each one the game's random
// tick rate: `randomTickSpeed` ticks per 16x16x16 section per game tick (SPEC §3.6.10, §8).

#include <random>
#include <set>

#include "nlohmann/json.hpp"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/storage/GameRule.h"
#include "mc/world/level/storage/GameRuleId.h"
#include "mc/world/level/storage/GameRules.h"

#include "feature/building/Building.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace {

constexpr char kDbCopper[] = "lv:building:copper";
constexpr float kWeatherChance = 0.05688889f; // Java ChangeOverTimeBlock: 64 / 1125

std::set<GlobalPos>& tracked() {
    static std::set<GlobalPos> s;
    return s;
}
bool& trackedDirty() {
    static bool d = false;
    return d;
}
std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
float nextFloat() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng()); }

/// Age of a weathering copper block (ours or vanilla), nullopt if it doesn't weather.
std::optional<int> weatheringAge(Block const& b) {
    if (auto const* i = infoOf(b)) {
        if (i->copperShape.empty() || i->waxed) return std::nullopt;
        return i->age;
    }
    std::string const& id = b.getTypeName();
    if (id.rfind("minecraft:", 0) != 0 || id.find("copper") == std::string::npos) return std::nullopt;
    if (id.find("waxed") != std::string::npos || id.find("ore") != std::string::npos || id.find("raw") != std::string::npos
        || id.find("ingot") != std::string::npos || id.find("nugget") != std::string::npos || id.find("torch") != std::string::npos) {
        return std::nullopt;
    }
    if (id.find("oxidized") != std::string::npos) return 3;
    if (id.find("weathered") != std::string::npos) return 2;
    if (id.find("exposed") != std::string::npos) return 1;
    return 0;
}

int randomTickSpeed() {
    auto level = ll::service::getLevel();
    if (!level) return 1;
    auto const& rules = level->getGameRules().mGameRules.get();
    auto        idx   = static_cast<std::size_t>(GameRules::GameRulesIndex::RandomTickSpeed);
    if (idx >= rules.size()) return 1;
    auto const& v = rules[idx].mValue.get();
    if (auto const* i = std::get_if<int>(&v)) return std::max(0, *i);
    return 1;
}

void saveTracked() {
    if (!game::db::available()) return;
    nlohmann::json j = nlohmann::json::array();
    for (auto const& p : tracked()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbCopper, j);
    trackedDirty() = false;
}

} // namespace

void trackCopper(GlobalPos const& pos) {
    if (tracked().insert(pos).second) trackedDirty() = true;
}

bool copperRandomTick(GlobalPos const& pos, std::function<float()> const& random) {
    auto const* block = blockAt(pos);
    if (!block) return false;
    auto const* info = infoOf(*block);
    if (!info || info->copperShape.empty() || info->waxed || info->age >= 3) return false;
    if (random() >= kWeatherChance) return false;
    // Java ChangeOverTimeBlock.getNextState: any less weathered block within 4 (Manhattan)
    // stops it; more weathered ones speed it up
    int age = info->age, lower = 0, higher = 0;
    for (int dx = -4; dx <= 4; ++dx) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dz = -4; dz <= 4; ++dz) {
                int dist = std::abs(dx) + std::abs(dy) + std::abs(dz);
                if (dist == 0 || dist > 4) continue;
                auto const* n = blockAt({pos.dim, pos.x + dx, pos.y + dy, pos.z + dz});
                if (!n) continue;
                auto a = weatheringAge(*n);
                if (!a) continue;
                if (*a < age) return false;
                if (*a > age) ++higher;
                else ++lower;
            }
        }
    }
    float f      = float(higher + 1) / float(higher + lower + 1);
    float chance = f * f * (age == 0 ? 0.75f : 1.f);
    if (random() >= chance) return false;
    auto const* next = convertKeepingStates(*block, copperId(info->copperShape, age + 1, false));
    if (!next) return false;
    set(pos, *next);
    return true;
}

void copperTick() {
    auto& s = tracked();
    if (s.empty()) return;
    int speed = randomTickSpeed();
    if (speed <= 0) return;
    float perBlock = float(speed) / 4096.f;
    std::vector<GlobalPos> gone;
    std::vector<GlobalPos> candidates;
    for (auto const& p : s) {
        if (nextFloat() >= perBlock) continue;
        candidates.push_back(p);
    }
    for (auto const& p : candidates) {
        auto const* b = blockAt(p);
        if (!b) continue; // not loaded: keep it
        auto const* i = infoOf(*b);
        if (!i || i->copperShape.empty() || i->waxed || i->age >= 3) {
            gone.push_back(p);
            continue;
        }
        if (copperRandomTick(p, nextFloat)) {
            log().info("building: copper at {} {} {} weathered to {}", p.x, p.y, p.z, copperId(i->copperShape, i->age + 1, false));
        }
    }
    for (auto const& p : gone) s.erase(p);
    if (!gone.empty()) trackedDirty() = true;
}

bool tryWaxOrScrape(Player& player, ItemStack const& item, GlobalPos const& pos) {
    auto const* block = blockAt(pos);
    if (!block || item.isNull()) return false;
    auto const* info = infoOf(*block);
    if (!info || info->copperShape.empty()) return false;
    std::string const& held = item.getTypeName();
    bool               axe  = held.size() > 4 && held.compare(held.size() - 4, 4, "_axe") == 0;
    if (held == "minecraft:honeycomb") {
        if (info->waxed) return false;
        auto const* next = convertKeepingStates(*block, copperId(info->copperShape, info->age, true));
        if (!next) return false;
        set(pos, *next);
        consumeHeld(player);
        playSound(pos, "copper.wax.on");
        particle(pos, "minecraft:wax_particle");
        log().info("building: {} waxed by {}", info->id, player.getRealName());
        return true;
    }
    if (!axe) return false;
    std::string target;
    if (info->waxed) target = copperId(info->copperShape, info->age, false);
    else if (info->age > 0) target = copperId(info->copperShape, info->age - 1, false);
    else return false;
    auto const* next = convertKeepingStates(*block, target);
    if (!next) return false;
    set(pos, *next);
    playSound(pos, info->waxed ? "copper.wax.off" : "scrape");
    particle(pos, info->waxed ? "minecraft:wax_particle" : "minecraft:scrape");
    if (!player.isCreative()) {
        ItemStack held2 = player.getSelectedItem();
        if (!held2.isNull()) {
            held2.hurtAndBreak(1, &player);
            player.setSelectedItem(held2);
            player.sendInventory(false);
        }
    }
    log().info("building: {} {} by {}", info->id, info->waxed ? "unwaxed" : "scraped", player.getRealName());
    return true;
}

namespace detail {
void loadCopper() {
    tracked().clear();
    if (auto j = game::db::load(kDbCopper); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) tracked().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
    trackedDirty() = false;
}
void saveCopper() {
    if (trackedDirty()) saveTracked();
}
void clearCopper() { tracked().clear(); }
std::size_t trackedCopperCount() { return tracked().size(); }
} // namespace detail

} // namespace lv::feature::building
