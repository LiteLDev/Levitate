#ifdef LV_CLIENT

// Client side of the materials unit (client mod only):
// - Block of experience: Create's ExperienceBlock.animateTick end rod particles around the local
//   player (Java's ClientLevel.animateTick sampling, feature/materials/MaterialsFx.cpp), once per
//   game tick. The server sends them to vanilla players itself.
// - Chromatic compound: Create's charge bar on the item in inventories and the hotbar (visible with
//   a charge, width round(13 * light / N), colour from 0x413c69 to white).

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

#include "nlohmann/json.hpp"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/renderers/InventoryItemRenderer.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/comprehensive/ParticleType.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"

#include "client/Client.h"
#include "feature/materials/Materials.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::materials {

namespace fm = feature::materials;

namespace {

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

std::uint64_t& lastTick() {
    static std::uint64_t t = 0;
    return t;
}

void animate() {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto tick = level->getCurrentTick().tickID;
    if (tick == lastTick()) return;
    lastTick() = tick;
    auto&    region = player->getDimensionBlockSource();
    auto     p      = player->getPosition();
    BlockPos center{int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))};
    fm::animateExperienceBlocks(region, center, rng(), [&](Vec3 const& at, Vec3 const& velocity) {
        level->addParticle(ParticleType::EndRod, at, velocity, 0, nullptr, false);
    });
}

/// Create's ChromaticCompoundItem charge (0 when none): the server mirrors it into the aux value,
/// which is all the item renderer's copy of the stack keeps (no item data).
int chargeOf(ItemStack const& stack) {
    if (stack.isNull() || !stack.mItem || stack.mItem->mFullName->getString() != "levitate:chromatic_compound") return 0;
    return std::max(0, int(stack.mAuxValue));
}

struct PendingBar {
    float x, y, size;
    int   light;
};
std::vector<PendingBar>& pendingBars() {
    static std::vector<PendingBar> v;
    return v;
}

void drawBar(MinecraftUIRenderContext& ctx, float x, float y, float size, int light) {
    int   n     = std::max(1, config::get<fm::MaterialsConfig>().lightSourceCountForRefinedRadiance);
    float frac  = std::clamp(float(light) / float(n), 0.f, 1.f);
    int   width = int(std::lround(13.f * frac));
    float px    = size / 16.f;
    // Color.mixColors(0x413c69, 0xFFFFFF, light / N)
    float r = (0x41 + (0xff - 0x41) * frac) / 255.f, g = (0x3c + (0xff - 0x3c) * frac) / 255.f, b = (0x69 + (0xff - 0x69) * frac) / 255.f;
    RectangleArea back{x + 2 * px, x + 15 * px, y + 13 * px, y + 15 * px};
    ctx.fillRectangle(back, mce::Color{0.f, 0.f, 0.f, 1.f}, 1.f);
    if (width > 0) {
        RectangleArea bar{x + 2 * px, x + (2 + width) * px, y + 13 * px, y + 14 * px};
        ctx.fillRectangle(bar, mce::Color{r, g, b, 1.f}, 1.f);
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    MaterialsClientTickHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        animate();
    } catch (...) {
    }
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsChargeBarHook,
    ll::memory::HookPriority::Normal,
    InventoryItemRenderer,
    &InventoryItemRenderer::$render,
    void,
    MinecraftUIRenderContext& ctx,
    IClientInstance&          client,
    UIControl&                owner,
    int                       pass
) {
    origin(ctx, client, owner, pass);
    try {
        int light = chargeOf(*mItemInstance);
        if (light <= 0) return;
        glm::vec2 pos  = *owner.mCachedPosition;
        glm::vec2 size = *owner.mSize;
        // items are drawn in a shared mesh batch: the bar goes on top once the batch is done
        pendingBars().push_back({pos.x, pos.y, std::min(size.x, size.y), light});
    } catch (...) {
    }
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsChargeBarBatchHook,
    ll::memory::HookPriority::Normal,
    MinecraftUIRenderContext,
    &MinecraftUIRenderContext::$endSharedMeshBatch,
    void,
    ::ComponentRenderBatch& renderBatch
) {
    origin(renderBatch);
    if (pendingBars().empty()) return;
    auto bars = std::move(pendingBars());
    pendingBars().clear();
    try {
        for (auto const& b : bars) drawBar(*this, b.x, b.y, b.size, b.light);
    } catch (...) {
    }
}

class MaterialsClientModule {
public:
    void install() {
        MaterialsClientTickHook::hook();
        MaterialsChargeBarHook::hook();
        MaterialsChargeBarBatchHook::hook();
    }
    void uninstall() {
        MaterialsChargeBarBatchHook::unhook();
        MaterialsChargeBarHook::unhook();
        MaterialsClientTickHook::unhook();
    }
};

} // namespace lv::client::materials

LV_REGISTER_MODULE(lv::client::materials::MaterialsClientModule);

#endif // LV_CLIENT
