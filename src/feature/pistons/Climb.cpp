// Ropes and pulley magnets are climbable (Create tags them `minecraft:climbable`). Custom blocks
// have no JSON component for it, so their block types get the Climbable property, as the
// building unit does for ladders. The client mod marks its own block types too (a remote
// server's blocks are registered separately on the client), so its movement prediction climbs.

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/block/BlockProperty.h"
#include "mc/world/level/block/BlockType.h"

#include "feature/pistons/Pistons.h"
#include "mod/Log.h"

namespace lv::feature::pistons {

namespace {

constexpr auto kClimbable = static_cast<std::uint64_t>(BlockProperty::Climbable);

BlockType const* typeOf(std::string_view id) {
    auto type = BlockType::tryGetFromRegistry(HashedString(std::string(id)));
    return type ? &*type : nullptr;
}

} // namespace

bool ropesClimbable() {
    for (auto id : {kRope, kMagnet}) {
        auto const* type = typeOf(id);
        if (!type || (static_cast<std::uint64_t>(static_cast<BlockProperty>(type->mProperties)) & kClimbable) == 0) return false;
    }
    return true;
}

bool markRopesClimbable() {
    bool all     = true;
    int  changed = 0;
    for (auto id : {kRope, kMagnet}) {
        auto const* type = typeOf(id);
        if (!type) {
            all = false;
            continue;
        }
        BlockProperty& props = const_cast<BlockType*>(type)->mProperties;
        auto           bits  = static_cast<std::uint64_t>(props);
        if (bits & kClimbable) continue;
        props = static_cast<BlockProperty>(bits | kClimbable);
        ++changed;
    }
    if (changed > 0) log().info("pistons: ropes and pulley magnets are climbable");
    return all;
}

} // namespace lv::feature::pistons
