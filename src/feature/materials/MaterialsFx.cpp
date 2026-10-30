// Materials unit (U9): furnace experience of Create's smelting / blasting recipes and the end rod
// particles of the block of experience.
//
// - Furnace xp (Create's recipe JSONs): Bedrock credits the output item's multiplier per item
//   smelted; for the inputs Create gives its own xp (crushed ores 0.1, raw zinc 0.7, zinc ore 1.0,
//   dough / soul sand 0) the furnace's stored xp is set to Create's amount instead. Java sums the
//   recipe xp and rounds the fraction randomly when the experience is taken: the fraction is kept
//   per furnace and rolled when the game hands out the stored xp.
// - Block of experience (Create's ExperienceBlock.animateTick): the client samples blocks around
//   the player as Java's ClientLevel.animateTick (667 times within 16 and 32 blocks per tick); a
//   block of experience hit sheds an end rod particle 1 time in 5. The client mod does it itself
//   (client/materials); the server does it for vanilla players.

#include <cmath>
#include <map>
#include <random>
#include <string>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/Container.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"

#include "core/GlobalPos.h"
#include "feature/materials/Materials.h"
#include "feature/net/Net.h"
#include "mod/Log.h"

namespace lv::feature::materials {

// ---- furnace experience ------------------------------------------------------------------------

std::optional<float> createSmeltingXp(std::string const& inputId) {
    static std::unordered_map<std::string, float> const table{
        {"levitate:crushed_raw_iron", 0.1f},   {"levitate:crushed_raw_gold", 0.1f},  {"levitate:crushed_raw_copper", 0.1f},
        {"levitate:crushed_raw_zinc", 0.1f},   {"levitate:raw_zinc", 0.7f},          {"levitate:zinc_ore", 1.0f},
        {"levitate:deepslate_zinc_ore", 1.0f}, {"levitate:dough", 0.0f},             {"minecraft:soul_sand", 0.0f},
    };
    auto it = table.find(inputId);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

namespace {

/// Per furnace: Create's xp minus what the game credited for the items smelted since the last take.
std::map<core::GlobalPos, double>& adjustments() {
    static std::map<core::GlobalPos, double> f;
    return f;
}
std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

} // namespace

// Smelting is seen by comparing the furnace's slots between two looks: FurnaceBlockActor::canBurn
// (asked every tick while it works) and the moment the experience is taken. Input items gone and
// output items gained = items smelted from that input. (The furnace is read through its
// Container, the game's own virtual.)
struct FurnaceView {
    std::string input;
    int         inputCount  = 0;
    int         outputCount = 0;
};
std::map<core::GlobalPos, FurnaceView>& furnaceViews() {
    static std::map<core::GlobalPos, FurnaceView> v;
    return v;
}

void observeFurnace(FurnaceBlockActor& furnace) {
    Container* box = furnace.getContainer();
    if (!box) return;
    auto const&      p    = furnace.getPosition();
    ItemStack const& in   = box->getItem(0);
    ItemStack const& out  = box->getItem(2);
    int              inN  = in.isNull() ? 0 : in.mCount;
    int              outN = out.isNull() ? 0 : out.mCount;
    std::string      inId = in.isNull() ? std::string() : in.getTypeName();
    // the dimension is not known here: furnaces are keyed by position
    core::GlobalPos key{0, p.x, p.y, p.z};
    auto&           view = furnaceViews()[key];
    int             used = view.inputCount - inN, made = outN - view.outputCount;
    if (!view.input.empty() && used > 0 && made > 0 && (inId.empty() || inId == view.input)) {
        if (auto xp = createSmeltingXp(view.input)) {
            // Bedrock credits the output item's multiplier for these items: Create's recipe xp instead
            float vanilla = FurnaceBlockActor::_getXPRewardMultiplier(out);
            adjustments()[key] += std::min(used, made) * (double(*xp) - double(vanilla));
        }
    }
    if (!inId.empty()) view.input = inId;
    view.inputCount  = inN;
    view.outputCount = outN;
}

LL_TYPE_INSTANCE_HOOK(MaterialsFurnaceCanBurnHook, ll::memory::HookPriority::Normal, FurnaceBlockActor, &FurnaceBlockActor::canBurn, bool, ::Recipes const& recipes) {
    try {
        observeFurnace(*this);
    } catch (...) {
    }
    return origin(recipes);
}

LL_TYPE_INSTANCE_HOOK(MaterialsFurnaceWithdrawHook, ll::memory::HookPriority::Normal, FurnaceBlockActor, &FurnaceBlockActor::withdrawStoredXPReward, int) {
    try {
        observeFurnace(*this); // what was smelted since the last tick
    } catch (...) {
    }
    int         xp = origin();
    auto const& p  = getPosition();
    for (auto it = adjustments().begin(); it != adjustments().end(); ++it) {
        if (it->first.x != p.x || it->first.y != p.y || it->first.z != p.z) continue;
        // Java createExperience: the whole points, the fraction left counts as one with its probability
        double total = std::max(0.0, double(xp) + it->second);
        double whole = std::floor(total);
        xp           = int(whole) + (std::uniform_real_distribution<double>(0, 1)(rng()) < total - whole ? 1 : 0);
        adjustments().erase(it);
        break;
    }
    return xp;
}

// ---- block of experience particles -------------------------------------------------------------

void animateExperienceBlocks(BlockSource& region, BlockPos const& center, std::mt19937& random, ParticleSink const& emit) {
    auto type = BlockType::tryGetFromRegistry(HashedString("levitate:experience_block"));
    if (!type) return;
    BlockType const* exp  = type.as_ptr();
    int const        minY = region.getMinHeight(), maxY = region.getMaxHeight();
    auto             one  = [&](int range) {
        std::uniform_int_distribution<int> r(0, range - 1);
        BlockPos p{center.x + r(random) - r(random), center.y + r(random) - r(random), center.z + r(random) - r(random)};
        if (p.y < minY || p.y >= maxY) return;
        if (&region.getBlock(p).getBlockType() != exp) return;
        // ExperienceBlock.animateTick
        if (std::uniform_int_distribution<int>(0, 4)(random) != 0) return;
        std::uniform_real_distribution<float> u(-.75f, .75f);
        std::normal_distribution<float>       g(0.f, 1.f);
        Vec3 at{p.x + .5f + std::clamp(u(random), -.55f, .55f), p.y + .5f + std::clamp(u(random), -.55f, .55f), p.z + .5f + std::clamp(u(random), -.55f, .55f)};
        emit(at, Vec3{g(random) * .005f, g(random) * .005f, g(random) * .005f});
    };
    for (int i = 0; i < 667; ++i) {
        one(16);
        one(32);
    }
}

namespace {

constexpr int kEndRodParticle = 48; // ParticleType::EndRod

ll::event::ListenerPtr& tickListener() {
    static ll::event::ListenerPtr l;
    return l;
}

void particlesForVanillaPlayers() {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([](Player& player) {
        if (player.isSimulated() || net::isCompanion(player)) return true; // the client mod draws its own
        auto&    region = player.getDimensionBlockSource();
        auto     pos    = player.getPosition();
        BlockPos center{static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), static_cast<int>(std::floor(pos.z))};
        animateExperienceBlocks(region, center, rng(), [&](Vec3 const& at, Vec3 const&) {
            LevelEventPacket pkt;
            pkt.mEventId = 0x4000 | kEndRodParticle; // ParticleLegacyEvent | type
            pkt.mPos     = at;
            pkt.mData    = 0;
            pkt.sendTo(player);
        });
        return true;
    });
}

} // namespace

void installFurnaceXp() {
    MaterialsFurnaceCanBurnHook::hook();
    MaterialsFurnaceWithdrawHook::hook();
    tickListener() = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
        try {
            particlesForVanillaPlayers();
        } catch (...) {
        }
    });
}

void uninstallFurnaceXp() {
    if (tickListener()) ll::event::EventBus::getInstance().removeListener(tickListener());
    tickListener() = nullptr;
    MaterialsFurnaceWithdrawHook::unhook();
    MaterialsFurnaceCanBurnHook::unhook();
    adjustments().clear();
    furnaceViews().clear();
}

} // namespace lv::feature::materials
