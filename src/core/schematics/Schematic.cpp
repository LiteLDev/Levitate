#include "core/schematics/Schematic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <sstream>

#include "core/schematics/Inflate.h"
#include "core/schematics/JavaMapping.h"

namespace lv::core::schematics {

using nbt::Compound;
using nbt::List;
using nbt::Tag;
using nbt::Type;

std::string BlockState::describe() const {
    std::ostringstream os;
    os << name;
    if (!states.empty()) {
        os << '[';
        for (std::size_t i = 0; i < states.keys.size(); ++i) {
            if (i) os << ',';
            os << states.keys[i] << '=';
            auto const& v = states.values[i];
            if (auto const* s = v.asString()) os << *s;
            else if (auto n = v.asInt()) os << *n;
            else os << nbt::toString(v);
        }
        os << ']';
    }
    return os.str();
}

bool operator==(BlockState const& a, BlockState const& b) {
    return a.name == b.name && Tag(a.states) == Tag(b.states) && a.alternatives == b.alternatives;
}

Pos Schematic::posOf(std::size_t i) const {
    int z = static_cast<int>(i % static_cast<std::size_t>(size.z));
    i /= static_cast<std::size_t>(size.z);
    int y = static_cast<int>(i % static_cast<std::size_t>(size.y));
    int x = static_cast<int>(i / static_cast<std::size_t>(size.y));
    return {x, y, z};
}

BlockState const* Schematic::stateAt(Pos p) const {
    auto i = at(p);
    return i < 0 || static_cast<std::size_t>(i) >= palette.size() ? nullptr : &palette[static_cast<std::size_t>(i)];
}

std::int32_t Schematic::paletteIndex(BlockState const& state) {
    for (std::size_t i = 0; i < palette.size(); ++i)
        if (palette[i] == state) return static_cast<std::int32_t>(i);
    palette.push_back(state);
    return static_cast<std::int32_t>(palette.size() - 1);
}

void Schematic::resize(Pos s) {
    size = s;
    blocks.assign(volume(), kVoid);
    liquids.clear();
}

std::size_t Schematic::solidCount() const {
    std::size_t n = 0;
    for (auto i : blocks)
        if (i >= 0 && static_cast<std::size_t>(i) < palette.size() && !palette[static_cast<std::size_t>(i)].isAir()) ++n;
    return n;
}

bool isSchematicFileName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    if (name.find_first_of("/\\:*?\"<>|") != std::string_view::npos) return false;
    if (name.starts_with('.')) return false;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 32) return false;
    return name.ends_with(".nbt") || name.ends_with(".mcstructure");
}

namespace {

bool fail(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

std::optional<Pos> readSize(Tag const* t) {
    if (!t) return std::nullopt;
    std::vector<std::int64_t> v;
    if (auto const* l = t->asList()) {
        for (auto const& item : l->items)
            if (auto n = item.asInt()) v.push_back(*n);
    } else if (t->type() == Type::IntArray) {
        for (auto n : std::get<nbt::IntArray>(t->v).v) v.push_back(n);
    }
    if (v.size() != 3) return std::nullopt;
    return Pos{static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2])};
}

bool validSize(Pos s) {
    if (s.x < 0 || s.y < 0 || s.z < 0) return false;
    if (s.x > kMaxDimension || s.y > kMaxDimension || s.z > kMaxDimension) return false;
    return static_cast<std::size_t>(s.x) * s.y * s.z <= kMaxVolume;
}

Tag intList(std::initializer_list<int> values) {
    List l{Type::Int, {}};
    for (int v : values) l.items.emplace_back(static_cast<std::int32_t>(v));
    return Tag(std::move(l));
}

} // namespace

// ---- .mcstructure ---------------------------------------------------------------------------------

std::optional<Schematic> readMcstructure(std::string_view bytes, std::string* error) {
    std::string err;
    auto        res = nbt::read(bytes, nbt::Endian::Little, &err);
    if (!res) return fail(error, "not a structure file: " + err), std::nullopt;
    auto const* root = res->root.asCompound();
    if (!root) return fail(error, "root is not a compound"), std::nullopt;
    auto size = readSize(root->find("size"));
    if (!size || !validSize(*size)) return fail(error, "bad or too large size"), std::nullopt;
    auto const* structure = root->find("structure") ? root->find("structure")->asCompound() : nullptr;
    if (!structure) return fail(error, "missing structure"), std::nullopt;

    Schematic s;
    s.format = "mcstructure";
    s.resize(*size);
    auto const* paletteRoot = structure->find("palette") ? structure->find("palette")->asCompound() : nullptr;
    Compound const* def = nullptr;
    if (paletteRoot) {
        if (auto const* d = paletteRoot->find("default")) def = d->asCompound();
        if (!def && !paletteRoot->values.empty()) def = paletteRoot->values.front().asCompound();
    }
    if (def) {
        if (auto const* bp = def->find("block_palette"); bp && bp->asList()) {
            for (auto const& entry : bp->asList()->items) {
                BlockState st;
                if (auto const* c = entry.asCompound()) {
                    if (auto const* n = c->find("name"); n && n->asString()) st.name = *n->asString();
                    if (auto const* states = c->find("states"); states && states->asCompound()) st.states = *states->asCompound();
                    if (auto const* v = c->find("version")) st.version = static_cast<std::int32_t>(v->asInt().value_or(0));
                }
                if (st.name.empty()) st.name = "minecraft:air";
                s.palette.push_back(std::move(st));
            }
        }
    }
    auto const* indices = structure->find("block_indices") ? structure->find("block_indices")->asList() : nullptr;
    if (!indices || indices->items.empty()) return fail(error, "missing block_indices"), std::nullopt;
    auto readLayer = [&](Tag const& t, std::vector<std::int32_t>& out) -> bool {
        auto const* l = t.asList();
        if (!l) return false;
        if (l->items.size() != s.volume()) return false;
        out.resize(l->items.size());
        for (std::size_t i = 0; i < l->items.size(); ++i) {
            auto v = l->items[i].asInt().value_or(-1);
            out[i] = (v < 0 || static_cast<std::size_t>(v) >= s.palette.size()) ? kVoid : static_cast<std::int32_t>(v);
        }
        return true;
    };
    if (s.volume() > 0 && !readLayer(indices->items[0], s.blocks)) return fail(error, "bad block layer"), std::nullopt;
    if (indices->items.size() > 1 && s.volume() > 0) {
        std::vector<std::int32_t> liquids;
        if (readLayer(indices->items[1], liquids) && std::any_of(liquids.begin(), liquids.end(), [](auto v) { return v >= 0; }))
            s.liquids = std::move(liquids);
    }
    if (def) {
        if (auto const* pd = def->find("block_position_data"); pd && pd->asCompound()) {
            auto const& c = *pd->asCompound();
            for (std::size_t i = 0; i < c.keys.size(); ++i) {
                std::size_t idx  = 0;
                auto const& key  = c.keys[i];
                auto [end, code] = std::from_chars(key.data(), key.data() + key.size(), idx);
                if (code != std::errc{} || end != key.data() + key.size()) continue;
                if (idx >= s.volume()) continue;
                auto const* entry = c.values[i].asCompound();
                if (!entry) continue;
                if (auto const* be = entry->find("block_entity_data")) s.blockEntities[idx] = *be;
                if (auto const* lv = entry->find("levitate_data"); lv && lv->asString()) s.levitateData[idx] = *lv->asString();
            }
        }
    }
    if (auto const* ents = structure->find("entities"); ents && ents->asList()) s.entities = static_cast<int>(ents->asList()->items.size());
    return s;
}

std::string writeMcstructure(Schematic const& s, Pos origin) {
    Compound root;
    root.set("format_version", Tag(std::int32_t{1}));
    root.set("size", intList({s.size.x, s.size.y, s.size.z}));

    Compound structure;
    List     layers{Type::List, {}};
    List     primary{Type::Int, {}};
    primary.items.reserve(s.blocks.size());
    for (auto v : s.blocks) primary.items.emplace_back(static_cast<std::int32_t>(v));
    List secondary{Type::Int, {}};
    secondary.items.reserve(s.blocks.size());
    for (std::size_t i = 0; i < s.blocks.size(); ++i)
        secondary.items.emplace_back(static_cast<std::int32_t>(i < s.liquids.size() ? s.liquids[i] : kVoid));
    layers.items.emplace_back(std::move(primary));
    layers.items.emplace_back(std::move(secondary));
    structure.set("block_indices", Tag(std::move(layers)));
    structure.set("entities", Tag::list(Type::Compound));

    List paletteList{Type::Compound, {}};
    for (auto const& st : s.palette) {
        Compound e;
        e.set("name", Tag(st.name));
        e.set("states", Tag(st.states));
        e.set("version", Tag(st.version));
        paletteList.items.emplace_back(std::move(e));
    }
    Compound positionData;
    std::set<std::size_t> keys;
    for (auto const& [i, _] : s.levitateData) keys.insert(i);
    for (auto const& [i, _] : s.blockEntities) keys.insert(i);
    for (auto i : keys) {
        Compound e;
        if (auto it = s.blockEntities.find(i); it != s.blockEntities.end()) e.set("block_entity_data", it->second);
        if (auto it = s.levitateData.find(i); it != s.levitateData.end()) e.set("levitate_data", Tag(it->second));
        positionData.set(std::to_string(i), Tag(std::move(e)));
    }
    Compound def;
    def.set("block_palette", Tag(std::move(paletteList)));
    def.set("block_position_data", Tag(std::move(positionData)));
    Compound palette;
    palette.set("default", Tag(std::move(def)));
    structure.set("palette", Tag(std::move(palette)));
    root.set("structure", Tag(std::move(structure)));
    root.set("structure_world_origin", intList({origin.x, origin.y, origin.z}));
    return nbt::write(Tag(std::move(root)), "", nbt::Endian::Little);
}

// ---- Java .nbt ------------------------------------------------------------------------------------

std::optional<Schematic> readJavaStructure(std::string_view bytes, std::string* error) {
    std::string err;
    auto        res = nbt::read(bytes, nbt::Endian::Big, &err);
    if (!res) return fail(error, "not an NBT structure: " + err), std::nullopt;
    auto const* root = res->root.asCompound();
    if (!root) return fail(error, "root is not a compound"), std::nullopt;
    auto size = readSize(root->find("size"));
    if (!size || !validSize(*size)) return fail(error, "bad or too large size"), std::nullopt;

    List const* paletteList = nullptr;
    if (auto const* p = root->find("palette")) paletteList = p->asList();
    if (!paletteList) {
        if (auto const* ps = root->find("palettes"); ps && ps->asList() && !ps->asList()->items.empty())
            paletteList = ps->asList()->items.front().asList();
    }
    if (!paletteList) return fail(error, "missing palette"), std::nullopt;

    Schematic s;
    s.format = "nbt";
    s.resize(*size);
    // Java palette index -> our palette index (-1: structure void)
    std::vector<std::int32_t> remap;
    for (auto const& entry : paletteList->items) {
        JavaState js;
        if (auto const* c = entry.asCompound()) {
            if (auto const* n = c->find("Name"); n && n->asString()) js.name = *n->asString();
            if (auto const* props = c->find("Properties"); props && props->asCompound()) {
                auto const& pc = *props->asCompound();
                for (std::size_t i = 0; i < pc.keys.size(); ++i) {
                    std::string value;
                    if (auto const* sv = pc.values[i].asString()) value = *sv;
                    else if (auto iv = pc.values[i].asInt()) value = std::to_string(*iv);
                    js.props.emplace_back(pc.keys[i], value);
                }
            }
        }
        auto mapped = mapJavaState(js);
        remap.push_back(mapped ? s.paletteIndex(*mapped) : kVoid);
    }
    auto const* blocks = root->find("blocks") ? root->find("blocks")->asList() : nullptr;
    if (blocks) {
        for (auto const& b : blocks->items) {
            auto const* c = b.asCompound();
            if (!c) continue;
            auto pos   = readSize(c->find("pos"));
            auto state = c->find("state") ? c->find("state")->asInt() : std::nullopt;
            if (!pos || !state || !s.contains(*pos)) continue;
            if (*state < 0 || static_cast<std::size_t>(*state) >= remap.size()) continue;
            auto idx       = s.index(*pos);
            s.blocks[idx]  = remap[static_cast<std::size_t>(*state)];
            if (auto const* n = c->find("nbt"); n && s.blocks[idx] >= 0) s.blockEntities[idx] = *n;
        }
    }
    if (auto const* ents = root->find("entities"); ents && ents->asList()) s.entities = static_cast<int>(ents->asList()->items.size());
    return s;
}

std::optional<Schematic> load(std::string_view fileName, std::string_view bytes, std::string* error) {
    if (fileName.ends_with(".mcstructure")) return readMcstructure(bytes, error);
    if (fileName.ends_with(".nbt")) {
        if (isGzip(bytes)) {
            auto raw = gunzip(bytes);
            if (!raw) return fail(error, "corrupt GZIP data"), std::nullopt;
            return readJavaStructure(*raw, error);
        }
        return readJavaStructure(bytes, error);
    }
    return fail(error, "unsupported file type"), std::nullopt;
}

// ---- transforms -----------------------------------------------------------------------------------

namespace {

Pos rotateCw(Pos p, int q) {
    switch (((q % 4) + 4) % 4) {
    case 1:
        return {-p.z, p.y, p.x};
    case 2:
        return {-p.x, p.y, -p.z};
    case 3:
        return {p.z, p.y, -p.x};
    default:
        return p;
    }
}

Pos mirrorPos(Pos p, Mirror m) {
    if (m == Mirror::LeftRight) p.z = -p.z;
    else if (m == Mirror::FrontBack) p.x = -p.x;
    return p;
}

} // namespace

Pos apply(Transform const& t, Pos local) { return rotateCw(mirrorPos(local, t.mirror), t.rotation); }

Pos unapply(Transform const& t, Pos offset) { return mirrorPos(rotateCw(offset, -t.rotation), t.mirror); }

std::array<Pos, 2> bounds(Transform const& t, Pos size) {
    Pos a = apply(t, {0, 0, 0});
    Pos b = apply(t, {std::max(0, size.x - 1), std::max(0, size.y - 1), std::max(0, size.z - 1)});
    return {Pos{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}, Pos{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}};
}

Pos recenterAnchor(Pos anchor, Pos size, Transform const& from, Transform const& to) {
    auto [fmin, fmax] = bounds(from, size);
    auto [tmin, tmax] = bounds(to, size);
    // keep the footprint's centre in place (Create rotates and mirrors about the centre)
    auto half = [](int twice) { return twice >= 0 ? twice / 2 : -((-twice + 1) / 2); };
    Pos  f2   = fmin + fmax;
    Pos  t2   = tmin + tmax;
    return {anchor.x + half(f2.x - t2.x), anchor.y + half(f2.y - t2.y), anchor.z + half(f2.z - t2.z)};
}

namespace {

// horizontal directions as (dx, dz)
enum class H { S, W, N, E };

Pos hvec(H h) {
    switch (h) {
    case H::S:
        return {0, 0, 1};
    case H::W:
        return {-1, 0, 0};
    case H::N:
        return {0, 0, -1};
    default:
        return {1, 0, 0};
    }
}

std::optional<H> hOf(Pos v) {
    if (v == Pos{0, 0, 1}) return H::S;
    if (v == Pos{-1, 0, 0}) return H::W;
    if (v == Pos{0, 0, -1}) return H::N;
    if (v == Pos{1, 0, 0}) return H::E;
    return std::nullopt;
}

// 3D directions in Bedrock facing order: down, up, north, south, west, east
constexpr std::array<Pos, 6> kDir{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
constexpr std::array<char const*, 6> kDirName{"down", "up", "north", "south", "west", "east"};

int dirIndex(Pos v) {
    for (int i = 0; i < 6; ++i)
        if (kDir[static_cast<std::size_t>(i)] == v) return i;
    return -1;
}

int dirFromName(std::string_view n) {
    for (int i = 0; i < 6; ++i)
        if (n == kDirName[static_cast<std::size_t>(i)]) return i;
    return -1;
}

Pos tv(Transform const& t, Pos v) { return apply(t, v); }

bool setInt(Tag& tag, std::int64_t v) {
    auto old = tag.asInt();
    if (old && *old == v) return false;
    switch (tag.type()) {
    case Type::Byte:
        tag.v = static_cast<std::int8_t>(v);
        break;
    case Type::Short:
        tag.v = static_cast<std::int16_t>(v);
        break;
    case Type::Long:
        tag.v = static_cast<std::int64_t>(v);
        break;
    default:
        tag.v = static_cast<std::int32_t>(v);
        break;
    }
    return true;
}

bool setStr(Tag& tag, std::string v) {
    auto const* old = tag.asString();
    if (old && *old == v) return false;
    tag.v = std::move(v);
    return true;
}

/// A horizontal index state with the given order of directions.
bool mapHorizontalIndex(Tag& tag, std::array<H, 4> const& order, Transform const& t) {
    auto v = tag.asInt();
    if (!v || *v < 0 || *v > 3) return false;
    auto r = hOf(tv(t, hvec(order[static_cast<std::size_t>(*v)])));
    if (!r) return false;
    for (int i = 0; i < 4; ++i)
        if (order[static_cast<std::size_t>(i)] == *r) return setInt(tag, i);
    return false;
}

} // namespace

bool transformStates(BlockState& st, Transform const& t) {
    if (t.rotation % 4 == 0 && t.mirror == Mirror::None) return false;
    bool  changed = false;
    auto& s       = st.states;
    auto  name    = std::string_view(st.name);
    bool  odd     = (((t.rotation % 4) + 4) % 4) % 2 == 1;

    for (auto const* key : {"facing_direction", "levitate:facing"}) {
        if (auto* tag = s.find(key)) {
            auto v = tag->asInt();
            if (v && *v >= 0 && *v <= 5) changed |= setInt(*tag, dirIndex(tv(t, kDir[static_cast<std::size_t>(*v)])));
        }
    }
    for (auto const* key : {"minecraft:facing_direction", "minecraft:block_face", "minecraft:cardinal_direction"}) {
        if (auto* tag = s.find(key); tag && tag->asString()) {
            int d = dirFromName(*tag->asString());
            if (d >= 0) changed |= setStr(*tag, kDirName[static_cast<std::size_t>(dirIndex(tv(t, kDir[static_cast<std::size_t>(d)])))]);
        }
    }
    if (auto* tag = s.find("torch_facing_direction"); tag && tag->asString()) {
        auto const& v = *tag->asString();
        int         d = v == "top" ? -1 : dirFromName(v);
        if (d >= 2) changed |= setStr(*tag, kDirName[static_cast<std::size_t>(dirIndex(tv(t, kDir[static_cast<std::size_t>(d)])))]);
    }
    for (auto const* key : {"pillar_axis"}) {
        if (auto* tag = s.find(key); tag && tag->asString() && odd) {
            auto const& v = *tag->asString();
            if (v == "x") changed |= setStr(*tag, "z");
            else if (v == "z") changed |= setStr(*tag, "x");
        }
    }
    if (auto* tag = s.find("levitate:axis"); tag && odd) {
        auto v = tag->asInt();
        if (v == 0) changed |= setInt(*tag, 2);
        else if (v == 2) changed |= setInt(*tag, 0);
    }
    if (auto* tag = s.find("weirdo_direction")) changed |= mapHorizontalIndex(*tag, {H::E, H::W, H::S, H::N}, t);
    if (auto* tag = s.find("direction")) {
        bool trapdoor = name.find("trapdoor") != std::string_view::npos;
        bool door     = !trapdoor && name.find("door") != std::string_view::npos;
        if (trapdoor) changed |= mapHorizontalIndex(*tag, {H::E, H::W, H::S, H::N}, t);
        else if (door) changed |= mapHorizontalIndex(*tag, {H::E, H::S, H::W, H::N}, t);
        else changed |= mapHorizontalIndex(*tag, {H::S, H::W, H::N, H::E}, t);
    }
    if (t.mirror != Mirror::None) {
        if (auto* tag = s.find("door_hinge_bit"); tag && name.find("trapdoor") == std::string_view::npos) {
            if (auto v = tag->asInt()) changed |= setInt(*tag, *v ? 0 : 1);
        }
    }
    if (auto* tag = s.find("ground_sign_direction")) {
        if (auto v = tag->asInt(); v && *v >= 0 && *v <= 15) {
            std::int64_t r = *v;
            if (t.mirror == Mirror::LeftRight) r = (8 - r + 16) % 16;
            else if (t.mirror == Mirror::FrontBack) r = (16 - r) % 16;
            r = (r + 4 * (((t.rotation % 4) + 4) % 4)) % 16;
            changed |= setInt(*tag, r);
        }
    }
    if (auto* tag = s.find("rail_direction")) {
        if (auto v = tag->asInt(); v && *v >= 0 && *v <= 9) {
            static constexpr std::array<H, 4> kAsc{H::E, H::W, H::N, H::S};
            static constexpr std::array<std::array<H, 2>, 4> kCurve{{{H::S, H::E}, {H::S, H::W}, {H::N, H::W}, {H::N, H::E}}};
            std::int64_t r = *v;
            if (r <= 1) {
                if (odd) r = 1 - r;
            } else if (r <= 5) {
                auto d = hOf(tv(t, hvec(kAsc[static_cast<std::size_t>(r - 2)])));
                for (int i = 0; d && i < 4; ++i)
                    if (kAsc[static_cast<std::size_t>(i)] == *d) r = 2 + i;
            } else {
                auto a = hOf(tv(t, hvec(kCurve[static_cast<std::size_t>(r - 6)][0])));
                auto b = hOf(tv(t, hvec(kCurve[static_cast<std::size_t>(r - 6)][1])));
                for (int i = 0; a && b && i < 4; ++i) {
                    auto const& c = kCurve[static_cast<std::size_t>(i)];
                    if ((c[0] == *a && c[1] == *b) || (c[0] == *b && c[1] == *a)) r = 6 + i;
                }
            }
            changed |= setInt(*tag, r);
        }
    }
    if (auto* tag = s.find("vine_direction_bits")) {
        if (auto v = tag->asInt()) {
            // bits: 1 south, 2 west, 4 north, 8 east
            static constexpr std::array<H, 4> kBits{H::S, H::W, H::N, H::E};
            std::int64_t r = 0;
            for (int i = 0; i < 4; ++i) {
                if (!(*v & (1 << i))) continue;
                auto d = hOf(tv(t, hvec(kBits[static_cast<std::size_t>(i)])));
                for (int j = 0; d && j < 4; ++j)
                    if (kBits[static_cast<std::size_t>(j)] == *d) r |= 1 << j;
            }
            changed |= setInt(*tag, r);
        }
    }
    // walls: wall_connection_type_<side>
    {
        static constexpr std::array<std::pair<char const*, H>, 4> kSides{
            {{"wall_connection_type_south", H::S}, {"wall_connection_type_west", H::W}, {"wall_connection_type_north", H::N}, {"wall_connection_type_east", H::E}}
        };
        std::array<Tag, 4> before{};
        bool               all = true;
        for (std::size_t i = 0; i < 4; ++i) {
            auto* tag = s.find(kSides[i].first);
            if (!tag) all = false;
            else before[i] = *tag;
        }
        if (all) {
            for (std::size_t i = 0; i < 4; ++i) {
                auto d = hOf(tv(t, hvec(kSides[i].second)));
                for (std::size_t j = 0; d && j < 4; ++j) {
                    if (kSides[j].second != *d) continue;
                    auto* tag = s.find(kSides[j].first);
                    if (!(*tag == before[i])) {
                        *tag    = before[i];
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

} // namespace lv::core::schematics
