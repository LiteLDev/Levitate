#include "feature/common/FluidRegistry.h"

#include <cctype>
#include <map>
#include <memory>

#include "core/fluids/Fluids.h"
#include "mod/I18n.h"

namespace lv::feature::common::fluidtypes {

namespace {

struct State {
    std::map<std::string, std::unique_ptr<FluidType>, std::less<>> fluids;
    std::map<std::string, EffectHandler, std::less<>>              effects;
};

std::uint32_t argb(std::string_view fluid) { return 0xFF000000u | core::fluids::fluidColor(fluid); }

FluidType vanilla(std::string id, std::string key, std::string name, std::string block, std::vector<std::string> world,
                  std::uint32_t color, int slot, std::string bucket, bool hot = false) {
    FluidType t;
    t.id           = std::move(id);
    t.nameKey      = std::move(key);
    t.nameFallback = std::move(name);
    t.sourceBlock  = std::move(block);
    t.worldBlocks  = std::move(world);
    t.color        = color;
    t.renderSlot   = slot;
    t.hot          = hot;
    t.bucketItem   = std::move(bucket);
    return t;
}

State& state() {
    static State s = [] {
        State st;
        auto add = [&](FluidType t) {
            auto id       = t.id;
            st.fluids[id] = std::make_unique<FluidType>(std::move(t));
        };
        // vanilla fluids (Bedrock names its source blocks water / lava); their buckets are in the
        // U3 capability already. Create's fluids are registered by U19 `fluids`.
        add(vanilla("minecraft:water", "fluid_type.minecraft.water", "Water", "minecraft:water", {"minecraft:flowing_water"}, argb("minecraft:water"), 1, "minecraft:water_bucket"));
        add(vanilla("minecraft:lava", "fluid_type.minecraft.lava", "Lava", "minecraft:lava", {"minecraft:flowing_lava"}, argb("minecraft:lava"), 2, "minecraft:lava_bucket", true));
        add(vanilla("minecraft:milk", "fluid_type.minecraft.milk", "Milk", "", {}, argb("minecraft:milk"), 5, "minecraft:milk_bucket"));
        add(vanilla("minecraft:powder_snow", "fluid_type.minecraft.powder_snow", "Powder Snow", "", {}, argb("minecraft:powder_snow"), 5, ""));
        return st;
    }();
    return s;
}

constexpr std::string_view kShortPotion = "potion:";          // "potion:<name>"
constexpr std::string_view kU19Potion   = "levitate:potion/"; // U19: "levitate:potion/<bottle>/<name>"

std::string readable(std::string_view text, bool capitaliseWords) {
    std::string out;
    bool        upper = capitaliseWords;
    for (char ch : text) {
        if (ch == '_') {
            out += ' ';
            upper = capitaliseWords;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
        upper = false;
    }
    return out;
}

} // namespace

void registerFluid(FluidType type) {
    if (!type.bucketItem.empty()) fluids::registerFluidItem(type.bucketItem, {type.id, kBucket}, "minecraft:bucket");
    if (!type.bottleItem.empty()) fluids::registerFluidItem(type.bottleItem, {type.id, kBottle}, "minecraft:glass_bottle");
    auto& slot = state().fluids[type.id];
    if (slot) *slot = std::move(type); // keep handed-out pointers valid
    else slot = std::make_unique<FluidType>(std::move(type));
}

FluidType const* find(std::string_view id) {
    auto& f = state().fluids;
    if (auto it = f.find(id); it != f.end()) return it->second.get();
    if (isPotion(id)) {
        if (auto it = f.find(kShortPotion); it != f.end()) return it->second.get();
    }
    return nullptr;
}

std::vector<FluidType const*> all() {
    std::vector<FluidType const*> out;
    for (auto const& [id, t] : state().fluids) out.push_back(t.get());
    return out;
}

FluidType const* byWorldBlock(std::string_view blockId) {
    if (blockId.empty()) return nullptr;
    for (auto const& [id, t] : state().fluids) {
        if (t->sourceBlock == blockId) return t.get();
        for (auto const& w : t->worldBlocks)
            if (w == blockId) return t.get();
    }
    return nullptr;
}

std::string bucketOf(std::string_view id) {
    auto const* t = find(id);
    return t ? t->bucketItem : std::string{};
}

bool hasBlockState(std::string_view id) {
    auto const* t = find(id);
    return t && !t->sourceBlock.empty();
}

std::string displayName(std::string_view id, std::string_view locale) {
    auto const* t = find(id);
    if (isPotion(id)) {
        auto potion = std::string(translate(t ? t->nameKey : "fluid.create.potion", t ? t->nameFallback : "Potion", locale));
        auto name   = potionName(id);
        return name.empty() ? potion : potion + " (" + readable(name, false) + ")";
    }
    if (t) return std::string(translate(t->nameKey, t->nameFallback, locale));
    auto colon = id.find(':');
    return readable(colon == std::string_view::npos ? id : id.substr(colon + 1), true);
}

bool isPotion(std::string_view id) { return id.starts_with(kShortPotion) || id.starts_with(kU19Potion); }

std::string potionName(std::string_view id) {
    if (id.starts_with(kShortPotion)) return std::string(id.substr(kShortPotion.size()));
    if (id.starts_with(kU19Potion)) {
        auto rest  = id.substr(kU19Potion.size());
        auto slash = rest.find('/');
        return slash == std::string_view::npos ? std::string{} : std::string(rest.substr(slash + 1));
    }
    return {};
}

void registerEffect(std::string id, EffectHandler handler) { state().effects[std::move(id)] = std::move(handler); }

EffectHandler const* effectFor(std::string_view id) {
    auto& e = state().effects;
    if (auto it = e.find(id); it != e.end()) return &it->second;
    for (auto const& [key, h] : e)
        if (!key.empty() && key.back() == ':' && id.starts_with(key)) return &h;
    // every potion id form shares the "potion:" handler
    if (isPotion(id))
        if (auto it = e.find(kShortPotion); it != e.end()) return &it->second;
    return nullptr;
}

} // namespace lv::feature::common::fluidtypes
