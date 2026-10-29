// Open pipe ends (Create's OpenEndedPipe, VanillaFluidTargets, FluidReactions and the open pipe
// effect handlers): draining source blocks, placing fluid, reactions between fluids and the
// effects of fluids poured out of a pipe.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/alchemy/Potion.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Capabilities.h"
#include "feature/common/FluidRegistry.h"
#include "feature/pipes/OpenEnds.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::pipes {

namespace ft = common::fluidtypes;

namespace {

BlockPos offset(BlockPos const& p, Face f) {
    auto o = cp::offsetOf(f);
    return BlockPos{p.x + o.x, p.y + o.y, p.z + o.z};
}

int stateOf(Block const& b, char const* name, int fallback = 0) { return game::intState(b, name).value_or(fallback); }

void sound(BlockSource& region, BlockPos const& pos, char const* name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(pos, region.getDimensionId());
}

bool setId(BlockSource& region, BlockPos const& pos, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {}) {
    auto const* b = game::blockWith(id, states);
    return b && game::setBlock(region, pos, *b);
}

bool isCauldron(std::string const& id) { return id == "minecraft:cauldron" || id == "minecraft:lava_cauldron"; }
bool isHive(std::string const& id) { return id == "minecraft:beehive" || id == "minecraft:bee_nest"; }

bool ultraWarm(BlockSource& region) { return region.getDimension().mUltraWarm; }

} // namespace

// ---- world fluids -----------------------------------------------------------------------------------

std::optional<WorldFluid> worldFluidAt(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return std::nullopt;
    auto const& b  = region.getBlock(pos);
    auto const& id = b.getTypeName();
    if (id == "minecraft:water" || id == "minecraft:flowing_water")
        return WorldFluid{"minecraft:water", stateOf(b, "liquid_depth") == 0};
    if (id == "minecraft:lava" || id == "minecraft:flowing_lava")
        return WorldFluid{"minecraft:lava", stateOf(b, "liquid_depth") == 0};
    if (auto const* t = ft::byWorldBlock(id)) return WorldFluid{t->id, id == t->sourceBlock};
    return std::nullopt;
}

bool canProvideFluidWithoutCapability(Block const& block) {
    auto const& id = block.getTypeName();
    return isHive(id) || isCauldron(id);
}

namespace {

/// Create's `VanillaFluidTargets.drainBlock`: a full beehive gives 250 mB of honey, a full cauldron
/// one bucket.
cp::Fluid drainVanillaTarget(BlockSource& region, BlockPos const& pos, bool simulate) {
    auto const& b  = region.getBlock(pos);
    auto const& id = b.getTypeName();
    if (isHive(id)) {
        if (stateOf(b, "honey_level") < 5) return {};
        if (!simulate) {
            if (auto const* type = b.getBlockType().getBlockState(HashedString("honey_level"))) {
                if (auto e = b.setState(*type, 0)) game::setBlock(region, pos, *e.as_ptr());
            }
        }
        return {"levitate:honey", 250};
    }
    if (isCauldron(id)) {
        auto got = common::fluids::drain(region, pos, -1, {"", 1000}, simulate);
        return {got.fluid, got.amount};
    }
    return {};
}

bool waterSourceRefills(BlockSource& region, BlockPos const& pos, std::string const& fluid) {
    auto const& cfg = config::get<PipesConfig>();
    if (fluid == "minecraft:water" ? !cfg.waterSourceConversion : fluid == "minecraft:lava" ? !cfg.lavaSourceConversion : true)
        return false;
    // Java's FlowingFluid.getNewLiquid: two horizontal sources and a solid block or a source below
    int sources = 0;
    for (Face f : {Face::North, Face::South, Face::West, Face::East}) {
        auto w = worldFluidAt(region, offset(pos, f));
        if (w && w->source && w->id == fluid) ++sources;
    }
    if (sources < 2) return false;
    BlockPos below = offset(pos, Face::Down);
    auto     w     = worldFluidAt(region, below);
    if (w && w->source && w->id == fluid) return true;
    return region.getBlock(below).canProvideFullSupport(1);
}

/// Create's `FluidReactions.handlePipeSpillCollision`: what forms where a pipe pours into
/// another fluid.
void spillCollision(BlockSource& region, BlockPos const& pos, std::string const& pipeFluid, WorldFluid const& world) {
    std::string result;
    bool        pipeWater = pipeFluid == "minecraft:water", pipeLava = pipeFluid == "minecraft:lava";
    if (pipeWater && world.id == "minecraft:lava") result = world.source ? "minecraft:obsidian" : "minecraft:cobblestone";
    else if (pipeLava && world.id == "minecraft:water") result = world.source ? "minecraft:stone" : "minecraft:cobblestone";
    if (pipeLava) {
        if (auto const* t = ft::find(world.id); t && !t->lavaInteraction.empty()) result = t->lavaInteraction;
    }
    if (world.id == "minecraft:lava" && !world.source) {
        if (auto const* t = ft::find(pipeFluid); t && !t->lavaInteraction.empty() && !t->sourceBlock.empty())
            result = t->lavaInteraction;
    }
    if (result.empty()) return;
    if (setId(region, pos, result)) {
        sound(region, pos, "random.fizz", .5f, 2.6f);
        log().info("pipes: {} poured into {} at {} {} {} formed {}", pipeFluid, world.id, pos.x, pos.y, pos.z, result);
    }
}

} // namespace

cp::Fluid removeFluidFromSpace(BlockSource& region, BlockPos const& out, bool simulate) {
    if (!region.hasChunksAt(out, 0, false)) return {};
    auto const& block = region.getBlock(out);
    if (canProvideFluidWithoutCapability(block)) return drainVanillaTarget(region, out, simulate);
    if (!game::replaceable(region, out)) return {};
    auto w = worldFluidAt(region, out);
    if (!w || !w->source) return {};
    cp::Fluid stack{w->id, 1000};
    if (simulate) return stack;
    if (waterSourceRefills(region, out, w->id)) return stack; // the source reforms at once
    auto const* type = ft::find(w->id);
    if (w->id == "minecraft:water") setId(region, out, "minecraft:flowing_water", {{"liquid_depth", 1}});
    else if (w->id == "minecraft:lava") setId(region, out, "minecraft:flowing_lava", {{"liquid_depth", 1}});
    else if (type) game::clearBlock(region, out);
    return stack;
}

bool provideFluidToSpace(BlockSource& region, BlockPos const& out, cp::Fluid const& fluid, bool simulate) {
    if (!region.hasChunksAt(out, 0, false)) return false;
    if (!game::replaceable(region, out)) return false;
    if (fluid.empty()) return false;
    auto const* type = ft::find(fluid.id);
    if (!type || type->sourceBlock.empty()) return true; // no world block: the pipe consumes it
    auto w = worldFluidAt(region, out);
    if (w && w->id != fluid.id) {
        spillCollision(region, out, fluid.id, *w);
        return false;
    }
    if (w && w->source) return false;
    if (simulate) return true;
    if (!config::get<PipesConfig>().pipesPlaceFluidSourceBlocks) return true;
    if (fluid.id == "minecraft:water" && ultraWarm(region)) {
        sound(region, out, "random.fizz", .5f, 2.6f);
        return true;
    }
    setId(region, out, type->sourceBlock);
    return true;
}

ft::Area openEndArea(cp::BlockFace const& at) {
    auto     out = at.connected();
    ft::Area a{double(out.x), double(out.y), double(out.z), double(out.x + 1), double(out.y + 1), double(out.z + 1)};
    a.minY -= 1;
    if (at.face == Face::Down) a.minY -= 1;
    return a;
}

int openEndFill(BlockSource& region, cp::BlockFace const& at, cp::OpenEnd& st, cp::Fluid const& fluid, bool simulate, std::int64_t gameTime) {
    BlockPos out = toBlockPos(at.connected());
    if (fluid.empty() || !region.hasChunksAt(out, 0, false)) return 0;
    if (!provideFluidToSpace(region, out, fluid, true)) return 0;
    if (!st.tank.empty() && st.tank.id != fluid.id) st.tank = {};
    auto const* type     = ft::find(fluid.id);
    bool        hasBlock = type && !type->sourceBlock.empty();
    auto const* effect   = ft::effectFor(fluid.id);
    cp::Fluid   offered  = fluid;
    if (effect && !hasBlock) offered.amount = 1;
    int filled = std::min(offered.amount, 1000 - std::max(0, st.tank.amount));
    if (simulate || filled <= 0) return std::max(0, filled);
    st.pulling = false;
    st.tank    = {fluid.id, std::max(0, st.tank.amount) + filled};
    if (effect) (*effect)(region, openEndArea(at), {fluid.id, offered.amount}, gameTime);
    if (st.tank.amount >= 1000 || !hasBlock) {
        if (provideFluidToSpace(region, out, st.tank, false)) st.tank = {};
    }
    return filled;
}

cp::Fluid openEndDrain(BlockSource& region, cp::BlockFace const& at, cp::OpenEnd& st, cp::Fluid const& request, bool simulate) {
    BlockPos out = toBlockPos(at.connected());
    if (request.amount <= 0 || !region.hasChunksAt(out, 0, false)) return {};
    if (!st.tank.empty()) {
        if (!request.id.empty() && request.id != st.tank.id) return {};
        int       n = std::min(request.amount, st.tank.amount);
        cp::Fluid got{st.tank.id, n};
        if (!simulate) {
            st.tank.amount -= n;
            if (st.tank.amount <= 0) st.tank = {};
        }
        return got;
    }
    cp::Fluid peek = removeFluidFromSpace(region, out, true);
    if (peek.empty() || (!request.id.empty() && request.id != peek.id)) return {};
    cp::Fluid drained = simulate ? peek : removeFluidFromSpace(region, out, false);
    if (drained.empty()) return {};
    int       remainder = drained.amount - request.amount;
    cp::Fluid got{drained.id, std::min(request.amount, drained.amount)};
    if (!simulate) {
        st.pulling = true;
        if (remainder > 0) st.tank = {drained.id, remainder};
        log().debug("pipes: open end at {} drained {}", at.pos.key(), drained.id);
    }
    return got;
}

void flowCollision(BlockSource& region, BlockPos const& pos, cp::Fluid const& a, cp::Fluid const& b) {
    // Create's handlePipeFlowCollision: the pipe breaks; water + lava leave cobblestone, lava and a
    // fluid with a lava interaction leave that block
    std::string result;
    auto        is = [](cp::Fluid const& f, char const* id) { return f.id == id; };
    if ((is(a, "minecraft:water") && is(b, "minecraft:lava")) || (is(a, "minecraft:lava") && is(b, "minecraft:water")))
        result = "minecraft:cobblestone";
    else if (is(a, "minecraft:lava") || is(b, "minecraft:lava")) {
        auto const& other = is(a, "minecraft:lava") ? b : a;
        if (auto const* t = ft::find(other.id); t && !t->lavaInteraction.empty() && !t->sourceBlock.empty())
            result = t->lavaInteraction;
    }
    if (auto level = ll::service::getLevel()) {
        BlockChangeContext ctx{};
        level->destroyBlock(region, pos, true, ctx);
    }
    if (!result.empty()) setId(region, pos, result);
    log().info("pipes: {} and {} met in the pipe at {} {} {}", a.id, b.id, pos.x, pos.y, pos.z);
}

// ---- open pipe effects (Create's OpenPipeEffectHandler implementations) ------------------------------

namespace {

std::vector<Actor*> actorsIn(BlockSource& region, ft::Area const& a) {
    AABB box{Vec3{float(a.minX), float(a.minY), float(a.minZ)}, Vec3{float(a.maxX), float(a.maxY), float(a.maxZ)}};
    std::vector<Actor*> out;
    for (auto actor : region.fetchEntities(nullptr, box, true, false)) out.push_back(actor.get());
    return out;
}

bool living(Actor const& a) { return (a.isPlayer() || a.hasCategory(ActorCategory::Mob)) && !a.isDead(); }

/// Java's ThrownPotion.dowseFire on the blocks of the area.
void dowse(BlockSource& region, ft::Area const& a) {
    for (int x = int(std::floor(a.minX)); x < int(std::ceil(a.maxX)); ++x)
        for (int y = int(std::floor(a.minY)); y < int(std::ceil(a.maxY)); ++y)
            for (int z = int(std::floor(a.minZ)); z < int(std::ceil(a.maxZ)); ++z) {
                BlockPos p{x, y, z};
                if (!region.hasChunksAt(p, 0, false)) continue;
                auto const& b  = region.getBlock(p);
                auto const& id = b.getTypeName();
                if (id == "minecraft:fire" || id == "minecraft:soul_fire") {
                    game::clearBlock(region, p);
                    continue;
                }
                if (id == "minecraft:campfire" || id == "minecraft:soul_campfire") {
                    if (stateOf(b, "extinguished") == 0) {
                        if (auto const* s = b.getBlockType().getBlockState(HashedString("extinguished"))) {
                            if (auto e = b.setState(*s, 1)) {
                                game::setBlock(region, p, *e.as_ptr());
                                sound(region, p, "random.fizz", .5f, 2.6f);
                            }
                        }
                    }
                    continue;
                }
                if (id.find("candle") != std::string::npos && stateOf(b, "lit") != 0) {
                    if (auto const* s = b.getBlockType().getBlockState(HashedString("lit"))) {
                        if (auto e = b.setState(*s, 0)) game::setBlock(region, p, *e.as_ptr());
                    }
                }
            }
}

} // namespace

void registerOpenPipeEffects() {
    ft::registerEffect("minecraft:water", [](BlockSource& region, ft::Area const& a, common::fluids::FluidStack const&, std::int64_t t) {
        if (t % 5 != 0) return;
        for (auto* e : actorsIn(region, a))
            if (e->isOnFire()) e->stopFire();
        dowse(region, a);
    });
    ft::registerEffect("minecraft:lava", [](BlockSource& region, ft::Area const& a, common::fluids::FluidStack const&, std::int64_t t) {
        if (t % 5 != 0) return;
        for (auto* e : actorsIn(region, a))
            if (!e->isFireImmune()) e->setOnFire(3, true);
    });
    ft::registerEffect("minecraft:milk", [](BlockSource& region, ft::Area const& a, common::fluids::FluidStack const&, std::int64_t t) {
        if (t % 5 != 0) return;
        for (auto* e : actorsIn(region, a))
            if (living(*e)) e->removeAllEffects();
    });
    ft::registerEffect("levitate:tea", [](BlockSource& region, ft::Area const& a, common::fluids::FluidStack const&, std::int64_t t) {
        if (t % 5 != 0) return;
        for (auto* e : actorsIn(region, a)) {
            if (!living(*e)) continue;
            MobEffectInstance haste(3, EffectDuration{21}); // Haste I, 21 ticks, hidden
            haste.mAmplifier     = 0;
            haste.mAmbient       = false;
            haste.mEffectVisible = false;
            e->addEffect(haste);
        }
    });
    ft::registerEffect("potion:", [](BlockSource& region, ft::Area const& a, common::fluids::FluidStack const& f, std::int64_t) {
        auto potion = Potion::getPotion(ft::potionName(f.fluid));
        if (!potion) return;
        std::vector<MobEffectInstance> const& effects = potion->mEffects;
        if (effects.empty()) return;
        for (auto* e : actorsIn(region, a)) {
            if (!living(*e)) continue;
            for (auto const& effect : effects) e->addEffect(effect);
        }
    });
}

} // namespace lv::feature::pipes
