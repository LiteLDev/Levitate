// Display unit (U24): the source / target registries of Display.h.

#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/display/Internal.h"
#include "game/Blocks.h"

namespace lv::feature::display {

namespace {
using Applies = std::function<bool(BlockSource&, GlobalPos const&, Block const&)>;

struct Registries {
    std::unordered_map<std::string, std::vector<Source>> sources;
    std::vector<std::pair<Applies, Source>>              sourceProviders;
    std::unordered_map<std::string, Target>              targets;
    std::vector<std::pair<Applies, Target>>              targetProviders;
};
Registries& reg() {
    static Registries r;
    return r;
}
} // namespace

void registerSource(std::string const& blockId, Source source) {
    auto& list = reg().sources[blockId];
    for (auto& s : list) {
        if (s.id == source.id) {
            s = std::move(source);
            return;
        }
    }
    list.push_back(std::move(source));
}

void registerSourceProvider(Applies applies, Source source) {
    reg().sourceProviders.emplace_back(std::move(applies), std::move(source));
}

void registerTarget(std::string const& blockId, Target target) { reg().targets[blockId] = std::move(target); }

void registerTargetProvider(Applies applies, Target target) {
    reg().targetProviders.emplace_back(std::move(applies), std::move(target));
}

std::vector<Source const*> sourcesAt(BlockSource& region, GlobalPos const& pos) {
    std::vector<Source const*> out;
    auto const*                block = loadedBlock(region, game::blockPos(pos));
    if (!block || block->isAir()) return out;
    if (auto it = reg().sources.find(block->getTypeName()); it != reg().sources.end()) {
        for (auto const& s : it->second) out.push_back(&s);
    }
    for (auto const& [applies, s] : reg().sourceProviders) {
        if (!applies(region, pos, *block)) continue;
        bool dup = false;
        for (auto const* o : out) dup |= o->id == s.id;
        if (!dup) out.push_back(&s);
    }
    return out;
}

Target const* targetAt(BlockSource& region, GlobalPos const& pos) {
    auto const* block = loadedBlock(region, game::blockPos(pos));
    if (!block || block->isAir()) return nullptr;
    if (auto it = reg().targets.find(block->getTypeName()); it != reg().targets.end()) return &it->second;
    for (auto const& [applies, t] : reg().targetProviders)
        if (applies(region, pos, *block)) return &t;
    return nullptr;
}

void notifyGatherers(GlobalPos const& source) { link::notify(source); }
void refreshLink(GlobalPos const& l) { link::notifyLink(l); }
void itemsPassed(GlobalPos const& source, int count) { link::itemsPassed(source, count); }

std::string formatValue(long long value, bool shortened) {
    // Create's ValueListDisplaySource: shortened values use "k" / "m" with one decimal
    if (!shortened || std::llabs(value) < 1000) return std::to_string(value);
    double      v      = static_cast<double>(value);
    char const* suffix = "k";
    if (std::llabs(value) >= 1000000) {
        v /= 1000000.0;
        suffix = "m";
    } else {
        v /= 1000.0;
    }
    char buf[32];
    if (std::fabs(v) >= 100) std::snprintf(buf, sizeof buf, "%.0f%s", std::floor(v), suffix);
    else std::snprintf(buf, sizeof buf, "%.1f%s", std::floor(v * 10) / 10, suffix);
    std::string s = buf;
    if (auto dot = s.find(".0"); dot != std::string::npos && dot + 2 == s.size() - 1) s.erase(dot, 2);
    return s;
}

std::string progressBar(double level, int width) {
    width      = std::max(1, width);
    level      = std::clamp(level, 0.0, 1.0);
    int filled = static_cast<int>(std::round(level * width));
    std::string s;
    for (int i = 0; i < width; ++i) s += i < filled ? "\xE2\x96\x88" : "\xE2\x96\x92"; // █ ▒
    return s;
}

} // namespace lv::feature::display
