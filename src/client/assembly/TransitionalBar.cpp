#ifdef LV_CLIENT

// Client mod: the progress bar of a transitional (incomplete) item of a sequenced assembly
// (Create's SequencedAssemblyItem: always visible, width round(progress * 13), colour mixed from
// 0xFFC074 to 0x46FFE0 by the progress). Bedrock draws item bars only from durability, so the bar
// is drawn over the slot after the item batch. The server mirrors the progress (in percent) into
// the stack's aux value, which is all the item renderer's copy of the stack keeps (no item data).

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/renderers/InventoryItemRenderer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"

#include "core/common/Recipe.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::assembly {

namespace {

/// The transitional items of the mod's sequenced assembly recipes, read once at install on the main
/// thread (the render thread must not touch the server's recipe cache).
std::set<std::string>& transitionalIds() {
    static std::set<std::string> ids;
    return ids;
}

void readTransitionalIds() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto const& e : fs::directory_iterator(Mod::getInstance().getSelf().getModDir() / "recipes" / "sequenced_assembly", ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream in(e.path());
        auto          doc = nlohmann::json::parse(in, nullptr, false, true);
        if (doc.is_discarded() || !doc.contains("transitional_item")) continue;
        auto const& t  = doc["transitional_item"];
        auto        id = t.value("item", t.value("id", std::string()));
        if (!id.empty()) transitionalIds().insert(core::common::mapId(id));
    }
}

/// The progress (0..1) of a transitional item, nullopt for other items.
std::optional<float> progressOf(ItemStack const& stack) {
    if (stack.isNull() || !stack.mItem) return std::nullopt;
    if (!transitionalIds().contains(stack.mItem->mFullName->getString())) return std::nullopt;
    return std::clamp(float(stack.mAuxValue) / 100.f, 0.f, 1.f);
}

struct PendingBar {
    float x, y, size, progress;
};
std::vector<PendingBar>& pendingBars() {
    static std::vector<PendingBar> v;
    return v;
}

void drawBar(MinecraftUIRenderContext& ctx, PendingBar const& b) {
    int   width = int(std::lround(13.f * b.progress));
    float px    = b.size / 16.f;
    // Color.mixColors(0xFFC074, 0x46FFE0, progress)
    float f = b.progress;
    float r = (0xff + (0x46 - 0xff) * f) / 255.f, g = (0xc0 + (0xff - 0xc0) * f) / 255.f, bl = (0x74 + (0xe0 - 0x74) * f) / 255.f;
    RectangleArea back{b.x + 2 * px, b.x + 15 * px, b.y + 13 * px, b.y + 15 * px};
    ctx.fillRectangle(back, mce::Color{0.f, 0.f, 0.f, 1.f}, 1.f);
    if (width > 0) {
        RectangleArea bar{b.x + 2 * px, b.x + (2 + width) * px, b.y + 13 * px, b.y + 14 * px};
        ctx.fillRectangle(bar, mce::Color{r, g, bl, 1.f}, 1.f);
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TransitionalBarHook,
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
        auto progress = progressOf(*mItemInstance);
        if (!progress) return;
        glm::vec2 pos  = *owner.mCachedPosition;
        glm::vec2 size = *owner.mSize;
        // items are drawn in a shared mesh batch: the bar goes on top once the batch is done
        pendingBars().push_back({pos.x, pos.y, std::min(size.x, size.y), *progress});
    } catch (...) {
    }
}

LL_TYPE_INSTANCE_HOOK(
    TransitionalBarBatchHook,
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
        for (auto const& b : bars) drawBar(*this, b);
    } catch (...) {
    }
}

class TransitionalBarModule {
public:
    void install() {
        if (transitionalIds().empty()) readTransitionalIds();
        TransitionalBarHook::hook();
        TransitionalBarBatchHook::hook();
    }
    void uninstall() {
        TransitionalBarBatchHook::unhook();
        TransitionalBarHook::unhook();
    }
};

} // namespace lv::client::assembly

LV_REGISTER_MODULE(lv::client::assembly::TransitionalBarModule);

#endif // LV_CLIENT
