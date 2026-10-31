#include "feature/common/DeathMessages.h"

#include <cstdint>
#include <map>
#include <utility>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"

#include "mod/Module.h"

namespace lv::feature::common::deathmsg {

namespace {

struct Marked {
    std::string              key;
    std::vector<std::string> params;
    std::uint64_t            tick = 0;
};

std::map<std::int64_t, Marked>& marks() {
    static std::map<std::int64_t, Marked> m;
    return m;
}

std::uint64_t nowTick() {
    auto level = ll::service::getLevel();
    return level ? static_cast<std::uint64_t>(level->getCurrentTick().tickID) : 0;
}

Marked const* find(Actor const& victim) {
    auto it = marks().find(const_cast<Actor&>(victim).getOrCreateUniqueID().rawID);
    if (it == marks().end()) return nullptr;
    if (nowTick() > it->second.tick + 1) {
        marks().erase(it);
        return nullptr;
    }
    return &it->second;
}

} // namespace

void mark(Actor const& victim, std::string key, std::vector<std::string> params) {
    auto now = nowTick();
    if (marks().size() > 256) std::erase_if(marks(), [&](auto const& e) { return now > e.second.tick + 1; });
    marks()[const_cast<Actor&>(victim).getOrCreateUniqueID().rawID] = Marked{std::move(key), std::move(params), now};
}

std::pair<std::string, std::vector<std::string>> peek(Actor const& victim, std::string const& deadName) {
    auto const* m = find(victim);
    if (!m) return {};
    std::vector<std::string> p{deadName};
    p.insert(p.end(), m->params.begin(), m->params.end());
    return {m->key, std::move(p)};
}

} // namespace lv::feature::common::deathmsg

using LvDeathMessage = std::pair<std::string, std::vector<std::string>>;
LL_TYPE_INSTANCE_HOOK(
    CommonDeathMessageHook,
    ll::memory::HookPriority::Normal,
    ActorDamageSource,
    &ActorDamageSource::getDeathMessage,
    LvDeathMessage,
    std::string const& deadName,
    Actor*             dead
) {
    if (dead) {
        auto marked = lv::feature::common::deathmsg::peek(*dead, deadName);
        if (!marked.first.empty()) return marked;
    }
    return origin(deadName, dead);
}

namespace lv::feature::common::deathmsg {
namespace {
struct DeathMessageModule {
    void install() { CommonDeathMessageHook::hook(); }
    void uninstall() { CommonDeathMessageHook::unhook(); }
};
} // namespace
} // namespace lv::feature::common::deathmsg

LV_REGISTER_MODULE(lv::feature::common::deathmsg::DeathMessageModule);
