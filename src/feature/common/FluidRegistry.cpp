#include "feature/common/FluidRegistry.h"

#include <map>
#include <memory>

namespace lv::feature::common::fluidtypes {

namespace {

struct State {
    std::map<std::string, std::unique_ptr<FluidType>, std::less<>> fluids;
    std::map<std::string, EffectHandler, std::less<>>              effects;
};

FluidType builtin(std::string id, std::string key, std::string name, std::string block, std::vector<std::string> world,
                  std::uint32_t color, int slot, bool hot = false, std::string lava = {}) {
    FluidType t;
    t.id              = std::move(id);
    t.nameKey         = std::move(key);
    t.nameFallback    = std::move(name);
    t.sourceBlock     = std::move(block);
    t.worldBlocks     = std::move(world);
    t.color           = color;
    t.renderSlot      = slot;
    t.hot             = hot;
    t.lavaInteraction = std::move(lava);
    return t;
}

State& state() {
    static State s = [] {
        State st;
        auto add = [&](FluidType t) {
            auto id       = t.id;
            st.fluids[id] = std::make_unique<FluidType>(std::move(t));
        };
        // vanilla fluids (Bedrock names its source blocks water / lava)
        add(builtin("minecraft:water", "tile.water.name", "Water", "minecraft:water", {"minecraft:flowing_water"}, 0xFF3F76E4, 1));
        add(builtin("minecraft:lava", "tile.lava.name", "Lava", "minecraft:lava", {"minecraft:flowing_lava"}, 0xFFE0661A, 2, true));
        add(builtin("minecraft:milk", "lv.pipes.fluid.milk", "Milk", "", {}, 0xFFF4F4F4, 5));
        add(builtin("minecraft:powder_snow", "tile.powder_snow.name", "Powder Snow", "", {}, 0xFFF0F7FA, 5));
        // Create's fluids (U19 registers their full entries with world blocks and buckets; these
        // defaults keep pipes rendering and reacting correctly until then)
        add(builtin("levitate:honey", "fluid.create.honey", "Honey", "", {}, 0xFFF8B533, 3, false, "levitate:limestone"));
        add(builtin("levitate:chocolate", "fluid.create.chocolate", "Chocolate", "", {}, 0xFF6B3A1E, 4, false, "levitate:scoria"));
        add(builtin("levitate:tea", "fluid.create.tea", "Builder's Tea", "", {}, 0xFFA05A2C, 7));
        add(builtin("potion:", "fluid.create.potion", "Potion", "", {}, 0xFF7E3FB0, 6));
        return st;
    }();
    return s;
}

} // namespace

void registerFluid(FluidType type) {
    auto& slot = state().fluids[type.id];
    if (slot) *slot = std::move(type); // keep handed-out pointers valid
    else slot = std::make_unique<FluidType>(std::move(type));
}

FluidType const* find(std::string_view id) {
    auto& f = state().fluids;
    if (auto it = f.find(id); it != f.end()) return it->second.get();
    if (isPotion(id)) {
        if (auto it = f.find(std::string_view{"potion:"}); it != f.end()) return it->second.get();
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

bool isPotion(std::string_view id) { return id.starts_with("potion:"); }

std::string potionName(std::string_view id) { return isPotion(id) ? std::string(id.substr(7)) : std::string{}; }

void registerEffect(std::string id, EffectHandler handler) { state().effects[std::move(id)] = std::move(handler); }

EffectHandler const* effectFor(std::string_view id) {
    auto& e = state().effects;
    if (auto it = e.find(id); it != e.end()) return &it->second;
    for (auto const& [key, h] : e)
        if (!key.empty() && key.back() == ':' && id.starts_with(key)) return &h;
    return nullptr;
}

} // namespace lv::feature::common::fluidtypes
