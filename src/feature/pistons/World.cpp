// World helpers of the pistons unit: blocks, states, BlockData, deferred edits.

#include <algorithm>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/pistons/Pistons.h"
#include "game/Blocks.h"

namespace lv::feature::pistons {

namespace {
int gEditing = 0;
std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> d;
    return d;
}
std::uint64_t gTick = 0;
} // namespace

bool isPiston(std::string_view id) { return id == kPiston || id == kStickyPiston; }
bool isPulley(std::string_view id) { return id == kRopePulley || id == kElevatorPulley; }
bool isRopeLike(std::string_view id) { return id == kRope || id == kMagnet; }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
cc::Pos   cpos(GlobalPos const& p) { return {p.x, p.y, p.z}; }
GlobalPos gpos(int dim, cc::Pos const& p) { return {dim, p.x, p.y, p.z}; }
GlobalPos rel(GlobalPos p, cc::Dir d, int n) {
    auto o = cc::offset(d) * n;
    p.x += o.x;
    p.y += o.y;
    p.z += o.z;
    return p;
}

bool loaded(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r && p.y >= r->getMinHeight() && p.y < r->getMaxHeight() && r->hasChunksAt(bp(p), 0, false);
}

Block const* blockAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    if (!r || !r->hasChunksAt(bp(p), 0, false)) return nullptr;
    if (p.y < r->getMinHeight() || p.y >= r->getMaxHeight()) return nullptr;
    return &r->getBlock(bp(p));
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? std::string(b->getTypeName()) : std::string();
}

std::optional<int> stateAt(GlobalPos const& p, char const* name) {
    auto const* b = blockAt(p);
    if (!b) return std::nullopt;
    return game::intState(*b, name);
}

int stateOr(GlobalPos const& p, char const* name, int fallback) { return stateAt(p, name).value_or(fallback); }

cc::Dir facingAt(GlobalPos const& p) { return static_cast<cc::Dir>(std::clamp(stateOr(p, kFacing, 1), 0, 5)); }

Block const* withStates(Block const& block, std::vector<std::pair<std::string_view, int>> const& states) {
    Block const* out = &block;
    for (auto const& [name, value] : states) {
        auto const* s = out->getBlockType().getBlockState(HashedString(std::string(name)));
        if (!s) return nullptr;
        auto b = out->setState(*s, value);
        if (!b) return nullptr;
        out = b.as_ptr();
    }
    return out;
}

bool placeBlock(GlobalPos const& p, Block const& block, int flags) {
    auto* r = regionOf(p.dim);
    if (!r || !r->hasChunksAt(bp(p), 0, false)) return false;
    BlockChangeContext ctx{};
    return r->setBlock(bp(p), block, flags, nullptr, ctx);
}

bool setStates(GlobalPos const& p, std::vector<std::pair<std::string_view, int>> const& states, int flags) {
    auto const* b = blockAt(p);
    if (!b) return false;
    auto const* next = withStates(*b, states);
    if (!next) return false;
    if (next == b) return true;
    return placeBlock(p, *next, flags);
}

bool place(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states, int flags) {
    auto const* b = game::blockWith(id, {});
    if (!b) return false;
    if (!states.empty()) {
        auto const* s = withStates(*b, states);
        if (s) b = s;
    }
    return placeBlock(p, *b, flags);
}

bool clearAt(GlobalPos const& p, int flags) {
    auto const* air = game::blockWith("minecraft:air");
    return air && placeBlock(p, *air, flags);
}

void destroyAt(GlobalPos const& p, bool drop) {
    auto* r     = regionOf(p.dim);
    auto  level = ll::service::getLevel();
    if (!r || !level || !r->hasChunksAt(bp(p), 0, false)) return;
    BlockChangeContext ctx{};
    level->destroyBlock(*r, bp(p), drop, ctx);
}

float destroySpeedAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? b->getBlockType().getDestroySpeed() : -1.f;
}

bool replaceableAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r && game::replaceable(*r, bp(p));
}

void sound(GlobalPos const& p, std::string const& name, float volume, float pitch) {
    contraption::ext::playSound(p.dim, cc::Vec3d::center(cpos(p)), name, volume, pitch);
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

EditGuard::EditGuard() { ++gEditing; }
EditGuard::~EditGuard() { --gEditing; }
bool editing() { return gEditing > 0; }

Json& data(GlobalPos const& p) {
    auto& doc = common::blockdata::edit(p);
    auto& d   = doc["pistons"];
    if (!d.is_object()) d = Json::object();
    return d;
}

Json const* dataIf(GlobalPos const& p) {
    auto const* doc = common::blockdata::get(p);
    if (!doc || !doc->contains("pistons")) return nullptr;
    auto const& d = (*doc)["pistons"];
    return d.is_object() ? &d : nullptr;
}

void eraseData(GlobalPos const& p) {
    auto const* doc = common::blockdata::get(p);
    if (!doc || !doc->contains("pistons")) return;
    Json copy = *doc;
    copy.erase("pistons");
    if (copy.empty()) common::blockdata::erase(p);
    else common::blockdata::set(p, std::move(copy));
}

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

std::uint64_t currentTick() { return gTick; }

namespace detail {
void runDeferred() {
    ++gTick;
    auto work = std::move(deferred());
    deferred().clear();
    for (auto& fn : work) fn();
}
void clearDeferred() { deferred().clear(); }
} // namespace detail

} // namespace lv::feature::pistons
