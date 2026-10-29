// Building unit: the catalog of block ids (mirrors tools/gen/building.ps1) and block state helpers.

#include <map>

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"

namespace lv::feature::building {

namespace {

constexpr std::array<char const*, 11> kWoods{"oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "crimson", "warped", "cherry", "bamboo"};
constexpr std::array<char const*, 16> kColours{"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                                "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
constexpr std::array<char const*, 4>  kStages{"", "exposed_", "weathered_", "oxidized_"};
constexpr std::array<char const*, 3>  kMetals{"andesite", "brass", "copper"};

struct Catalog {
    std::map<std::string, Info, std::less<>> byId;
    std::vector<Info const*>                 all;

    void add(std::string path, Kind kind, Ct ct = Ct::None) {
        Info i;
        i.id   = "levitate:" + path;
        i.kind = kind;
        i.ct   = ct;
        byId[i.id] = std::move(i);
    }

    Catalog() {
        for (auto const* c : {"andesite_casing", "brass_casing", "copper_casing", "shadow_steel_casing", "refined_radiance_casing", "railway_casing"})
            add(c, Kind::Casing, Ct::Omni);
        add("framed_glass", Kind::CtGlass, Ct::Omni);
        add("horizontal_framed_glass", Kind::CtGlass, Ct::Horizontal);
        add("vertical_framed_glass", Kind::CtGlass, Ct::Vertical);
        add("tiled_glass", Kind::TiledGlass);
        for (auto const* g : {"framed_glass", "horizontal_framed_glass", "vertical_framed_glass", "tiled_glass"})
            add(std::string(g) + "_pane", Kind::Pane);
        std::vector<std::string> windows(kWoods.begin(), kWoods.end());
        for (auto const* w : {"ornate_iron", "industrial_iron", "weathered_iron"}) windows.emplace_back(w);
        for (auto const& w : windows) {
            add(w + "_window", Kind::Window, Ct::Vertical);
            add(w + "_window_pane", Kind::Pane);
        }
        for (auto const* m : kMetals) {
            add(std::string(m) + "_bars", Kind::Bars);
            add(std::string(m) + "_scaffolding", Kind::Scaffold);
            add(std::string(m) + "_ladder", Kind::Ladder);
            add(std::string(m) + "_table_cloth", Kind::TableCloth);
        }
        for (auto const* c : kColours) add(std::string(c) + "_table_cloth", Kind::TableCloth);
        for (std::string type : {"shingles", "tiles"}) {
            std::string single = type.substr(0, type.size() - 1);
            for (bool waxed : {false, true}) {
                for (int age = 0; age < 4; ++age) {
                    std::string prefix = std::string(waxed ? "waxed_" : "") + kStages[age];
                    for (auto [shape, kind] : {std::pair{"copper_" + type, Kind::CopperBlock},
                                               std::pair{"copper_" + single + "_stairs", Kind::CopperStairs},
                                               std::pair{"copper_" + single + "_slab", Kind::CopperSlab}}) {
                        add(prefix + shape, kind);
                        auto& i       = byId["levitate:" + prefix + shape];
                        i.age         = age;
                        i.waxed       = waxed;
                        i.copperShape = shape;
                    }
                }
            }
        }
        for (auto [door, folds] : {std::pair{"andesite_door", true}, std::pair{"brass_door", false}, std::pair{"copper_door", true},
                                   std::pair{"train_door", false}, std::pair{"framed_glass_door", false}}) {
            add(door, Kind::Door);
            byId[std::string("levitate:") + door].folds = folds;
        }
        byId["levitate:framed_glass_door"].glass = true;
        add("train_trapdoor", Kind::Trapdoor);
        add("framed_glass_trapdoor", Kind::Trapdoor);
        byId["levitate:framed_glass_trapdoor"].glass = true;
        add("metal_girder", Kind::Girder);
        add("metal_girder_encased_shaft", Kind::GirderShaft);
        add("metal_bracket", Kind::Bracket);
        add("wooden_bracket", Kind::Bracket);
        add("placard", Kind::Placard);
        add("copycat_panel", Kind::CopycatPanel);
        add("copycat_step", Kind::CopycatStep);
        for (auto const& [k, v] : byId) all.push_back(&v);
    }
};

Catalog const& catalog() {
    static Catalog const c;
    return c;
}

} // namespace

Info const* infoOf(std::string_view id) {
    auto const& c = catalog();
    auto        it = c.byId.find(id);
    return it == c.byId.end() ? nullptr : &it->second;
}

Info const* infoOf(Block const& block) {
    auto const& name = block.getTypeName();
    if (name.size() < 9 || name.compare(0, 9, "levitate:") != 0) return nullptr;
    return infoOf(std::string_view(name));
}

std::vector<Info const*> const& allBlocks() { return catalog().all; }

std::string copperId(std::string const& shape, int age, bool waxed) {
    return std::string("levitate:") + (waxed ? "waxed_" : "") + kStages[std::clamp(age, 0, 3)] + shape;
}

GlobalPos offset(GlobalPos const& p, int dir, int n) {
    return {p.dim, p.x + kDx[dir] * n, p.y + kDy[dir] * n, p.z + kDz[dir] * n};
}

int stateOf(Block const& block, std::string_view name, int fallback) {
    return game::intState(block, name).value_or(fallback);
}

int hfacingOf(Block const& block) {
    int f = stateOf(block, "levitate:facing", North);
    return f < 2 || f > 5 ? North : f;
}

Block const* withStates(Block const& block, std::initializer_list<std::pair<std::string_view, int>> states) {
    Block const* b = &block;
    for (auto const& [name, value] : states) {
        auto const* s = b->getBlockType().getBlockState(HashedString(std::string(name)));
        if (!s) continue;
        if (auto next = b->setState(*s, value)) b = next.as_ptr();
    }
    return b;
}

Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& states) {
    Block const* b = &block;
    for (auto const& [name, value] : states) {
        auto const* s = b->getBlockType().getBlockState(HashedString(name));
        if (!s) continue;
        if (auto next = b->setState(*s, value)) b = next.as_ptr();
    }
    return b;
}

Block const* convertKeepingStates(Block const& from, std::string const& id) {
    auto const* to = game::blockWith(id);
    if (!to) return nullptr;
    std::vector<std::pair<std::string, int>> keep;
    for (auto const* name : {"levitate:facing", "levitate:upside", "levitate:shape", "levitate:top", "levitate:double"}) {
        if (auto v = game::intState(from, name)) keep.emplace_back(name, *v);
    }
    return withStates(*to, keep);
}

} // namespace lv::feature::building
