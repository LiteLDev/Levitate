#ifdef LV_CLIENT

// Client mod: the tooltip of a transitional (incomplete) item of a sequenced assembly in the
// game's language (Create's SequencedAssemblyRecipe.addToTooltip: "Sequenced Assembly",
// "Progress: <step>/<total>", "Next: <step>", "-> <step>" ...).
//
// The server writes the same lines into the item's lore in its own language, which is what vanilla
// clients see. With the client mod the lore lines are taken out of the hover text and the lines are
// made again here, from the recipe and the progress in the item's data, in the client's language.

#include <string>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/safety/RedactableString.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "feature/assembly/SequencedAssembly.h"
#include "feature/common/Items.h"
#include "mod/Module.h"
#include "selftest/SelfTest.h"

namespace lv::client::assembly {

namespace sa = feature::assembly::sequenced;

/// The item data tag of the sequenced assembly progress (common/Items.h stores data as "lv:<key>").
std::string const kDataTag = "lv:" + std::string(sa::kDataKey);

namespace {

/// Removes every line of `text` that ends with one of the lore `lines` (the game puts its own
/// colour codes in front of lore lines).
void removeLore(std::string& text, std::vector<std::string> const& lines) {
    for (auto const& l : lines) {
        if (l.empty()) continue;
        auto at = text.rfind(l); // the lore comes after the name
        if (at == std::string::npos) continue;
        auto start = text.rfind('\n', at);
        if (start == std::string::npos) start = at;
        text.erase(start, at + l.size() - start);
    }
}

void rewrite(std::string& text, std::vector<std::string> const& lore, std::vector<std::string> const& lines) {
    removeLore(text, lore);
    text += "\n";
    for (auto const& l : lines) text += "\n" + l;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TransitionalHovertextHook,
    ll::memory::HookPriority::Low,
    ItemStackBase,
    &ItemStackBase::getFormattedHovertext,
    Bedrock::Safety::RedactableString,
    Level& level,
    bool   showCategory
) {
    auto r = origin(level, showCategory);
    if (isNull() || !mUserData || !mUserData->contains(kDataTag)) return r;
    auto id = getTypeName();
    if (!id.starts_with("levitate:")) return r;
    // `this` may be an ItemInstance: the item data goes onto a stack of our own
    auto stack = feature::common::items::make(id, 1, getAuxValue());
    if (!stack) return r;
    stack->setUserData(mUserData->clone());
    if (!sa::isTransitional(*stack)) return r;
    auto lines = sa::tooltip(*stack, clientLocale());
    if (lines.empty()) return r;
    auto lore = getCustomLore();
    rewrite(r.mUnredactedString, lore, lines);
    if (r.mRedactedString) rewrite(*r.mRedactedString, lore, lines);
    return r;
}

// the hover text of a transitional item made by the engine (local worlds: the integrated server's
// level): Create's lines in the game's language, each once, the lore not repeated
LV_REGISTER_SELFTEST("assembly.client_tooltip", [](selftest::Context& ctx) {
    auto level = ll::service::getLevel();
    ctx.require(level.has_value(), "a level");
    auto slab = feature::common::items::make("minecraft:smooth_stone_slab");
    auto nug  = feature::common::items::make("minecraft:iron_nugget");
    ctx.require(slab && nug, "items");
    auto step = sa::findDeployingStep(*slab, *nug);
    ctx.require(step.has_value(), "the track's first step");
    auto item  = sa::advance(*step, *slab);
    auto lines = sa::tooltip(item, clientLocale());
    ctx.require(lines.size() >= 3, "tooltip lines in the game's language");
    auto text = item.getFormattedHovertext(*level, false).mUnredactedString;
    for (auto const& l : lines) {
        std::size_t n = 0;
        for (auto at = text.find(l); at != std::string::npos; at = text.find(l, at + l.size())) ++n;
        ctx.check(n == 1, "once in the hover text: " + l);
    }
    ctx.info("hover text: " + text);
});

class TransitionalTooltipModule {
public:
    void install() { TransitionalHovertextHook::hook(); }
    void uninstall() { TransitionalHovertextHook::unhook(); }
};

} // namespace lv::client::assembly

LV_REGISTER_MODULE(lv::client::assembly::TransitionalTooltipModule);

#endif // LV_CLIENT
