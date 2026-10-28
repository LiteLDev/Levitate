// Kinetic block registry and block-state helpers (server and client builds).

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"

namespace lv::feature::kinetics {

namespace {
struct Registry {
    std::shared_mutex                                                             mutex;
    std::map<std::string, std::unique_ptr<KineticSpec>, std::less<>>             specs;
    std::unordered_map<BlockType const*, KineticSpec const*>                      byType;
};
Registry& registry() {
    static Registry r;
    return r;
}
} // namespace

void registerBlock(std::string id, KineticSpec spec) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    if (spec.stressKey.empty()) spec.stressKey = core::kinetics::defaultStressKey(id);
    auto& slot = r.specs[id];
    if (slot) *slot = std::move(spec); // keep handed-out pointers valid
    else slot = std::make_unique<KineticSpec>(std::move(spec));
    r.byType.clear();
}

KineticSpec const* specOf(std::string_view id) {
    auto&            r = registry();
    std::shared_lock lock(r.mutex);
    auto             it = r.specs.find(id);
    return it == r.specs.end() ? nullptr : it->second.get();
}

std::vector<std::string> registeredIds() {
    auto&                    r = registry();
    std::shared_lock         lock(r.mutex);
    std::vector<std::string> out;
    for (auto const& [id, spec] : r.specs) out.push_back(id);
    return out;
}

namespace detail {

KineticSpec const* specOfBlock(Block const& block) {
    auto&       r    = registry();
    auto const* type = &block.getBlockType();
    {
        std::shared_lock lock(r.mutex);
        if (auto it = r.byType.find(type); it != r.byType.end()) return it->second;
    }
    std::unique_lock lock(r.mutex);
    auto             it   = r.specs.find(block.getTypeName());
    auto const*      spec = it == r.specs.end() ? nullptr : it->second.get();
    r.byType[type]        = spec;
    return spec;
}

void clearBlockCaches() {
    auto&            r = registry();
    std::unique_lock lock(r.mutex);
    r.byType.clear();
}

BlockState snapshot(Block const& block, KineticSpec const& spec) {
    BlockState s;
    s.id = block.getTypeName();
    for (auto const& name : spec.states) s.values.emplace_back(name, game::intState(block, name).value_or(0));
    return s;
}

Block const* withState(Block const& block, std::string const& state, int value) {
    auto const* s = block.getBlockType().getBlockState(HashedString(state));
    if (!s) return nullptr;
    auto b = block.setState(*s, value);
    return b ? b.as_ptr() : nullptr;
}

} // namespace detail

} // namespace lv::feature::kinetics
