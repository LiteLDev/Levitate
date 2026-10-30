#include <gtest/gtest.h>

#include <set>

#include "core/schematics/Inflate.h"
#include "core/schematics/JavaMapping.h"
#include "core/schematics/Logic.h"
#include "core/schematics/Nbt.h"
#include "core/schematics/Schematic.h"

using namespace lv::core::schematics;
using nbt::Compound;
using nbt::List;
using nbt::Tag;
using nbt::Type;

namespace {

std::string textVector() {
    std::string out;
    for (int i = 0; i < 60; ++i) {
        if (i) out += '\n';
        out += "Levitate schematic line " + std::to_string(i) + " / ";
        for (int k = 0; k < i % 7; ++k) out += "abc";
    }
    return out;
}

// .NET GZipStream (optimal level: dynamic Huffman blocks) of textVector()
unsigned char const kGzipVector[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x8d, 0xd5, 0x4d, 0x0e, 0x82, 0x30, 0x10, 0xc5, 0xf1, 0xbd, 0xa7, 0xe8, 0x0d, 0x10,
    0xf0, 0xf3, 0x0e, 0x5c, 0x02, 0x09, 0x89, 0x24, 0xea, 0x46, 0xe2, 0xf9, 0xcd, 0xc4, 0x89, 0xe3, 0xd4, 0xfe, 0xdb, 0xae, 0xe7, 0xb7, 0x20, 0xef,
    0xe5, 0x95, 0x61, 0x7e, 0x2d, 0xeb, 0xb8, 0xce, 0xe1, 0x39, 0x5d, 0xe7, 0xfb, 0xb8, 0x2e, 0x53, 0xb8, 0x2d, 0x8f, 0x39, 0x6c, 0x43, 0x13, 0x36,
    0x03, 0x1c, 0xdb, 0xd0, 0x84, 0xf1, 0x32, 0xe1, 0xbd, 0xfb, 0xdc, 0x73, 0xa4, 0xff, 0x92, 0x9c, 0xda, 0xfd, 0xaa, 0x1c, 0xdc, 0x47, 0x30, 0x67,
    0x0f, 0xff, 0x36, 0xc7, 0x8f, 0xb9, 0x24, 0x4e, 0x85, 0x24, 0xce, 0xe5, 0x24, 0x5a, 0x89, 0xba, 0xfc, 0x19, 0xad, 0x86, 0x5e, 0x21, 0x2d, 0xfe,
    0x0a, 0xec, 0x8a, 0xa8, 0xf0, 0x52, 0x09, 0x5f, 0xb5, 0x07, 0x06, 0x16, 0x3e, 0x1b, 0x49, 0xbc, 0xe2, 0x4b, 0x34, 0xfb, 0x0a, 0x69, 0x2d, 0x94,
    0x71, 0xe7, 0xea, 0xa8, 0xf0, 0xd2, 0x0b, 0x5f, 0xb5, 0x0b, 0x06, 0x96, 0x3f, 0x1b, 0x37, 0x03, 0x66, 0xf1, 0x08, 0x58, 0x26, 0x26, 0xc0, 0xd8,
    0xd5, 0x51, 0xe1, 0xa5, 0x17, 0xbe, 0x6a, 0x17, 0xfc, 0x30, 0x58, 0xfe, 0x6c, 0xdc, 0x16, 0x98, 0xc5, 0x43, 0x60, 0x99, 0x58, 0x01, 0xe3, 0xf8,
    0x55, 0x2a, 0x79, 0xe9, 0x85, 0xaf, 0xda, 0x05, 0x03, 0xcb, 0x9f, 0x8d, 0xdb, 0x02, 0xb3, 0x78, 0x08, 0xfc, 0xf0, 0x26, 0x56, 0xc0, 0x38, 0x7e,
    0x9a, 0x4a, 0x5e, 0x7a, 0xe1, 0xab, 0x76, 0xc1, 0xc0, 0xf2, 0x67, 0xe3, 0xb6, 0xc0, 0x2c, 0x1e, 0x02, 0xcb, 0xc4, 0x0a, 0x18, 0xc7, 0x4f, 0x53,
    0xc9, 0x4b, 0x2f, 0xfc, 0x6f, 0xd3, 0x2e, 0x18, 0x58, 0xfe, 0x6c, 0xdc, 0x16, 0x98, 0xf5, 0xd5, 0x7f, 0xdc, 0xc4, 0x0a, 0x18, 0x27, 0xfe, 0xcf,
    0x59, 0x2f, 0xbd, 0xf0, 0x55, 0xbb, 0x60, 0x60, 0xf9, 0xb3, 0x71, 0x5b, 0x78, 0x03, 0xd9, 0x33, 0x01, 0x68, 0x07, 0x09, 0x00, 0x00,
};

Tag intList(std::initializer_list<int> v) {
    List l{Type::Int, {}};
    for (int x : v) l.items.emplace_back(std::int32_t(x));
    return Tag(std::move(l));
}

Tag javaPaletteEntry(std::string name, std::vector<std::pair<std::string, std::string>> props = {}) {
    Compound c;
    c.set("Name", Tag(std::move(name)));
    if (!props.empty()) {
        Compound p;
        for (auto& [k, v] : props) p.set(k, Tag(v));
        c.set("Properties", Tag(std::move(p)));
    }
    return Tag(std::move(c));
}

std::string stateStr(BlockState const& s, std::string_view key) {
    auto const* t = s.states.find(key);
    return t && t->asString() ? *t->asString() : std::string{};
}
std::int64_t stateInt(BlockState const& s, std::string_view key) {
    auto const* t = s.states.find(key);
    return t ? t->asInt().value_or(-99) : -99;
}

} // namespace

// ---- NBT ------------------------------------------------------------------------------------------

TEST(SchematicsNbt, RoundTripBothEndians) {
    Compound c;
    c.set("byte", Tag(std::int8_t(-5)));
    c.set("short", Tag(std::int16_t(1234)));
    c.set("int", Tag(std::int32_t(-123456)));
    c.set("long", Tag(std::int64_t(1) << 40));
    c.set("float", Tag(1.5f));
    c.set("double", Tag(-2.25));
    c.set("string", Tag(std::string("h\xc3\xa9llo")));
    c.set("bytes", Tag(nbt::ByteArray{{1, 2, 3}}));
    c.set("ints", Tag(nbt::IntArray{{7, -8}}));
    c.set("longs", Tag(nbt::LongArray{{9}}));
    c.set("list", intList({1, 2, 3}));
    Compound inner;
    inner.set("x", Tag(std::int32_t(1)));
    c.set("inner", Tag(inner));
    Tag root(c);
    for (auto e : {nbt::Endian::Big, nbt::Endian::Little}) {
        auto bytes = nbt::write(root, "root", e);
        auto back  = nbt::read(bytes, e);
        ASSERT_TRUE(back);
        EXPECT_EQ(back->name, "root");
        EXPECT_TRUE(back->root == root) << nbt::toString(back->root);
    }
    // byte order really differs
    EXPECT_NE(nbt::write(root, "", nbt::Endian::Big), nbt::write(root, "", nbt::Endian::Little));
}

TEST(SchematicsNbt, RejectsTruncatedAndBadInput) {
    auto        bytes = nbt::write(Tag(Compound{{"a"}, {Tag(std::int32_t(1))}}), "", nbt::Endian::Little);
    std::string err;
    EXPECT_FALSE(nbt::read(bytes.substr(0, bytes.size() - 2), nbt::Endian::Little, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(nbt::read("", nbt::Endian::Little));
    EXPECT_FALSE(nbt::read(std::string("\x0a\x00\x00\x63", 4), nbt::Endian::Little)); // bad tag type 0x63
    // a list claiming a huge length is refused, not allocated
    std::string huge("\x09\x00\x00\x03\xff\xff\xff\x7f", 8);
    EXPECT_FALSE(nbt::read(huge, nbt::Endian::Little));
}

// ---- GZIP -----------------------------------------------------------------------------------------

TEST(SchematicsInflate, DynamicHuffmanVector) {
    std::string gz(reinterpret_cast<char const*>(kGzipVector), sizeof(kGzipVector));
    EXPECT_TRUE(isGzip(gz));
    auto out = gunzip(gz);
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, textVector());
    // corrupting the CRC is detected
    gz[gz.size() - 6] ^= 0x55;
    EXPECT_FALSE(gunzip(gz));
}

TEST(SchematicsInflate, StoredRoundTripAndLimits) {
    std::string big(200000, 'x');
    for (std::size_t i = 0; i < big.size(); i += 7) big[i] = static_cast<char>('a' + i % 26);
    auto gz = gzipStored(big);
    auto out = gunzip(gz);
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, big);
    EXPECT_FALSE(gunzip(gz, 1000)); // output limit
    EXPECT_EQ(*gunzip(gzipStored("")), "");
    EXPECT_FALSE(gunzip("not gzip at all, really not"));
    EXPECT_EQ(crc32("123456789"), 0xCBF43926u);
}

TEST(SchematicsInflate, Base64) {
    EXPECT_EQ(base64Encode("Man"), "TWFu");
    EXPECT_EQ(base64Encode("Ma"), "TWE=");
    EXPECT_EQ(base64Encode("M"), "TQ==");
    std::string bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
    EXPECT_EQ(*base64Decode(base64Encode(bin)), bin);
    EXPECT_FALSE(base64Decode("TQ=a"));
    EXPECT_FALSE(base64Decode("abc"));
}

// ---- files ----------------------------------------------------------------------------------------

TEST(SchematicsFiles, McstructureRoundTrip) {
    Schematic s;
    s.resize({2, 3, 4});
    BlockState stone{"minecraft:stone", {}, 17959425, {}};
    BlockState stairs{"minecraft:oak_stairs", {}, 17959425, {}};
    stairs.states.set("weirdo_direction", Tag(std::int32_t(2)));
    stairs.states.set("upside_down_bit", Tag(std::int8_t(1)));
    BlockState air{"minecraft:air", {}, 0, {}};
    auto       a = s.paletteIndex(air);
    auto       b = s.paletteIndex(stone);
    auto       c = s.paletteIndex(stairs);
    EXPECT_EQ(s.paletteIndex(stone), b); // deduplicated
    for (std::size_t i = 0; i < s.blocks.size(); ++i) s.blocks[i] = a;
    s.blocks[s.index({1, 2, 3})] = c;
    s.blocks[s.index({0, 0, 0})] = b;
    s.blocks[s.index({1, 0, 0})] = kVoid;
    s.levitateData[s.index({1, 2, 3})] = R"({"kinetics":{"speed":32}})";
    auto bytes = writeMcstructure(s, {10, 20, 30});
    auto back  = readMcstructure(bytes);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->size, (Pos{2, 3, 4}));
    EXPECT_EQ(back->blocks, s.blocks);
    ASSERT_EQ(back->palette.size(), 3u);
    EXPECT_EQ(back->stateAt({1, 2, 3})->name, "minecraft:oak_stairs");
    EXPECT_EQ(stateInt(*back->stateAt({1, 2, 3}), "weirdo_direction"), 2);
    EXPECT_EQ(back->stateAt({1, 0, 0}), nullptr);
    EXPECT_EQ(back->levitateData.at(s.index({1, 2, 3})), R"({"kinetics":{"speed":32}})");
    EXPECT_EQ(back->solidCount(), 2u);
    EXPECT_EQ(back->posOf(s.index({1, 2, 3})), (Pos{1, 2, 3}));
    EXPECT_TRUE(load("house.mcstructure", bytes));
    EXPECT_FALSE(readMcstructure(bytes.substr(0, 40)));
    EXPECT_EQ(back->worldOrigin, (Pos{10, 20, 30}));
    EXPECT_EQ(back->entities, 0);
}

TEST(SchematicsFiles, McstructureKeepsBlockEntitiesAndEntities) {
    // Create's schematics keep block entity NBT and entities (StructureTemplate.fillFromWorld)
    Schematic s;
    s.resize({2, 1, 1});
    BlockState chest{"minecraft:chest", {}, 0, {}};
    auto       c = s.paletteIndex(chest);
    s.blocks[0]  = c;
    s.blocks[1]  = c;
    Compound be;
    be.set("id", Tag(std::string("Chest")));
    be.set("x", Tag(std::int32_t(5)));
    s.blockEntities[1] = Tag(be);
    Compound stand;
    stand.set("identifier", Tag(std::string("minecraft:armor_stand")));
    List pos{Type::Float, {}};
    for (float v : {5.5f, 64.f, 7.5f}) pos.items.emplace_back(v);
    stand.set("Pos", Tag(std::move(pos)));
    s.entityTags.emplace_back(stand);
    auto back = readMcstructure(writeMcstructure(s, {5, 64, 7}));
    ASSERT_TRUE(back);
    ASSERT_EQ(back->blockEntities.count(1), 1u);
    EXPECT_EQ(*back->blockEntities.at(1).asCompound()->find("id")->asString(), "Chest");
    ASSERT_EQ(back->entityTags.size(), 1u);
    EXPECT_EQ(back->entities, 1);
    EXPECT_EQ(*back->entityTags[0].asCompound()->find("identifier")->asString(), "minecraft:armor_stand");
    EXPECT_EQ(back->worldOrigin, (Pos{5, 64, 7}));
}

TEST(SchematicsFiles, JavaStructureWithMapping) {
    // a Create-saved structure: 2x1x1 with oak stairs and a create shaft, plus a structure void
    Compound root;
    root.set("DataVersion", Tag(std::int32_t(3955)));
    root.set("size", intList({3, 1, 1}));
    List palette{Type::Compound, {}};
    palette.items.push_back(javaPaletteEntry("minecraft:oak_stairs", {{"facing", "north"}, {"half", "top"}, {"shape", "straight"}, {"waterlogged", "false"}}));
    palette.items.push_back(javaPaletteEntry("create:shaft", {{"axis", "z"}, {"waterlogged", "false"}}));
    palette.items.push_back(javaPaletteEntry("minecraft:structure_void"));
    root.set("palette", Tag(std::move(palette)));
    List blocks{Type::Compound, {}};
    auto block = [](int x, int state) {
        Compound b;
        b.set("pos", intList({x, 0, 0}));
        b.set("state", Tag(std::int32_t(state)));
        return Tag(std::move(b));
    };
    blocks.items.push_back(block(0, 0));
    blocks.items.push_back(block(1, 1));
    blocks.items.push_back(block(2, 2));
    root.set("blocks", Tag(std::move(blocks)));
    root.set("entities", Tag::list(Type::Compound));
    auto raw = nbt::write(Tag(std::move(root)), "", nbt::Endian::Big);

    std::string err;
    auto        s = load("house.nbt", gzipStored(raw), &err);
    ASSERT_TRUE(s) << err;
    EXPECT_EQ(s->format, "nbt");
    auto const* st = s->stateAt({0, 0, 0});
    ASSERT_TRUE(st);
    EXPECT_EQ(st->name, "minecraft:oak_stairs");
    EXPECT_EQ(stateInt(*st, "weirdo_direction"), 3);
    EXPECT_EQ(stateInt(*st, "upside_down_bit"), 1);
    auto const* shaft = s->stateAt({1, 0, 0});
    ASSERT_TRUE(shaft);
    EXPECT_EQ(shaft->name, "levitate:shaft");
    EXPECT_EQ(stateInt(*shaft, "levitate:axis"), 2);
    EXPECT_EQ(s->at({2, 0, 0}), kVoid);
    // also accepted uncompressed
    EXPECT_TRUE(load("house.nbt", raw));
    EXPECT_FALSE(load("house.txt", raw));
}

TEST(SchematicsFiles, FileNames) {
    EXPECT_TRUE(isSchematicFileName("My Schematic.nbt"));
    EXPECT_TRUE(isSchematicFileName("house.mcstructure"));
    EXPECT_FALSE(isSchematicFileName("../evil.nbt"));
    EXPECT_FALSE(isSchematicFileName("a/b.nbt"));
    EXPECT_FALSE(isSchematicFileName("x.txt"));
    EXPECT_FALSE(isSchematicFileName(".nbt"));
}

// ---- Java mapping -----------------------------------------------------------------------------------

TEST(SchematicsMapping, VanillaFamilies) {
    auto m = [](std::string_view s) { return *mapJavaState(parseJavaState(s)); };
    EXPECT_EQ(m("minecraft:dirt_path").name, "minecraft:grass_path");
    EXPECT_EQ(m("minecraft:stone").name, "minecraft:stone");
    auto log = m("minecraft:oak_log[axis=x]");
    EXPECT_EQ(stateStr(log, "pillar_axis"), "x");
    auto slab = m("minecraft:oak_slab[type=top]");
    EXPECT_EQ(slab.name, "minecraft:oak_slab");
    EXPECT_EQ(stateStr(slab, "minecraft:vertical_half"), "top");
    auto dbl = m("minecraft:stone_slab[type=double]");
    EXPECT_EQ(dbl.name, "minecraft:normal_stone_double_slab");
    auto door = m("minecraft:oak_door[facing=east,half=upper,hinge=right,open=true]");
    EXPECT_EQ(door.name, "minecraft:wooden_door");
    EXPECT_EQ(stateInt(door, "upper_block_bit"), 1);
    EXPECT_EQ(stateInt(door, "door_hinge_bit"), 1);
    EXPECT_EQ(stateInt(door, "open_bit"), 1);
    auto sign = m("minecraft:dark_oak_sign[rotation=4]");
    EXPECT_EQ(sign.name, "minecraft:darkoak_standing_sign");
    EXPECT_EQ(stateInt(sign, "ground_sign_direction"), 4);
    auto torch = m("minecraft:wall_torch[facing=west]");
    EXPECT_EQ(torch.name, "minecraft:torch");
    EXPECT_EQ(stateStr(torch, "torch_facing_direction"), "west");
    EXPECT_EQ(m("minecraft:redstone_torch[lit=false]").name, "minecraft:unlit_redstone_torch");
    EXPECT_EQ(m("minecraft:furnace[facing=north,lit=true]").name, "minecraft:lit_furnace");
    auto wall = m("minecraft:cobblestone_wall[east=low,north=tall,south=none,west=none,up=true]");
    EXPECT_EQ(stateStr(wall, "wall_connection_type_east"), "short");
    EXPECT_EQ(stateStr(wall, "wall_connection_type_north"), "tall");
    EXPECT_EQ(stateInt(wall, "wall_post_bit"), 1);
    auto rail = m("minecraft:powered_rail[shape=ascending_north,powered=true]");
    EXPECT_EQ(rail.name, "minecraft:golden_rail");
    EXPECT_EQ(stateInt(rail, "rail_direction"), 4);
    EXPECT_EQ(m("minecraft:snow[layers=3]").name, "minecraft:snow_layer");
    EXPECT_EQ(stateInt(m("minecraft:snow[layers=3]"), "height"), 2);
    EXPECT_EQ(m("minecraft:cave_air").name, "minecraft:air");
    EXPECT_FALSE(mapJavaState(parseJavaState("minecraft:structure_void")));
    auto chest = m("minecraft:chest[facing=south,type=single]");
    EXPECT_EQ(stateStr(chest, "minecraft:cardinal_direction"), "south");
    EXPECT_EQ(stateInt(chest, "facing_direction"), 3);
    EXPECT_EQ(m("othermod:widget").name, "othermod:widget");
}

TEST(SchematicsMapping, CreateBlocks) {
    auto st = *mapJavaState(parseJavaState("create:mechanical_piston[facing=west,state=retracted]"));
    EXPECT_EQ(st.name, "levitate:mechanical_piston");
    EXPECT_EQ(stateInt(st, "levitate:facing"), 4);
    auto glass = *mapJavaState(parseJavaState("create:encased_chain_drive[axis=y,attach_face=none,powered=true]"));
    EXPECT_EQ(stateInt(glass, "levitate:axis"), 1);
    EXPECT_EQ(stateInt(glass, "levitate:powered"), 1);
    // chute states run 0..n (facing 0 down, 1..4 north..east; shape 0 normal .. 3 encased)
    auto chute = *mapJavaState(parseJavaState("create:chute[facing=west,shape=intersection]"));
    EXPECT_EQ(stateInt(chute, "levitate:facing"), 3);
    EXPECT_EQ(stateInt(chute, "levitate:shape"), 2);
    auto down = *mapJavaState(parseJavaState("create:chute[facing=down,shape=window]"));
    EXPECT_EQ(stateInt(down, "levitate:facing"), 0);
    EXPECT_EQ(stateInt(down, "levitate:shape"), 1);
}

// ---- transforms -------------------------------------------------------------------------------------

TEST(SchematicsTransform, PositionsMatchCreate) {
    Transform cw{1, Mirror::None};
    EXPECT_EQ(apply(cw, {1, 0, 0}), (Pos{0, 0, 1})); // east -> south
    EXPECT_EQ(apply(cw, {0, 0, 1}), (Pos{-1, 0, 0}));
    Transform lr{0, Mirror::LeftRight};
    EXPECT_EQ(apply(lr, {1, 2, 3}), (Pos{1, 2, -3}));
    Transform fb{0, Mirror::FrontBack};
    EXPECT_EQ(apply(fb, {1, 2, 3}), (Pos{-1, 2, 3}));
    for (int r = 0; r < 4; ++r) {
        for (int m = 0; m < 3; ++m) {
            Transform t{r, static_cast<Mirror>(m)};
            Pos       p{3, 1, -2};
            EXPECT_EQ(unapply(t, apply(t, p)), p);
        }
    }
    auto b = bounds(cw, {3, 2, 5});
    EXPECT_EQ(b[0], (Pos{-4, 0, 0}));
    EXPECT_EQ(b[1], (Pos{0, 1, 2}));
}

TEST(SchematicsTransform, RecenterKeepsFootprintCentre) {
    Pos size{4, 1, 2};
    Pos anchor{100, 64, 200};
    Pos a2 = recenterAnchor(anchor, size, {0, Mirror::None}, {2, Mirror::None});
    // 180 degrees: same footprint
    auto b0 = bounds({0, Mirror::None}, size);
    auto b2 = bounds({2, Mirror::None}, size);
    EXPECT_EQ(anchor + b0[0], a2 + b2[0]);
    EXPECT_EQ(anchor + b0[1], a2 + b2[1]);
}

TEST(SchematicsTransform, StatesFollowRotationAndMirror) {
    BlockState stairs{"minecraft:oak_stairs", {}, 0, {}};
    stairs.states.set("weirdo_direction", Tag(std::int32_t(0))); // east
    auto s1 = stairs;
    transformStates(s1, {1, Mirror::None});
    EXPECT_EQ(stateInt(s1, "weirdo_direction"), 2); // south
    auto s2 = stairs;
    transformStates(s2, {0, Mirror::FrontBack});
    EXPECT_EQ(stateInt(s2, "weirdo_direction"), 1); // west

    BlockState chest{"minecraft:chest", {}, 0, {}};
    chest.states.set("minecraft:cardinal_direction", Tag(std::string("north")));
    transformStates(chest, {1, Mirror::None});
    EXPECT_EQ(stateStr(chest, "minecraft:cardinal_direction"), "east");

    // chute facing: 0 down, 1..4 north / south / west / east
    BlockState chute{"levitate:chute", {}, 0, {}};
    chute.states.set("levitate:facing", Tag(std::int32_t(1))); // north
    auto c1 = chute;
    transformStates(c1, {1, Mirror::None});
    EXPECT_EQ(stateInt(c1, "levitate:facing"), 4); // east
    chute.states.set("levitate:facing", Tag(std::int32_t(3))); // west
    auto c2 = chute;
    transformStates(c2, {0, Mirror::FrontBack});
    EXPECT_EQ(stateInt(c2, "levitate:facing"), 4); // east
    chute.states.set("levitate:facing", Tag(std::int32_t(0))); // down
    auto c3 = chute;
    transformStates(c3, {1, Mirror::None});
    EXPECT_EQ(stateInt(c3, "levitate:facing"), 0);

    BlockState log{"minecraft:oak_log", {}, 0, {}};
    log.states.set("pillar_axis", Tag(std::string("x")));
    transformStates(log, {3, Mirror::None});
    EXPECT_EQ(stateStr(log, "pillar_axis"), "z");

    BlockState sign{"minecraft:standing_sign", {}, 0, {}};
    sign.states.set("ground_sign_direction", Tag(std::int32_t(0))); // south
    transformStates(sign, {1, Mirror::None});
    EXPECT_EQ(stateInt(sign, "ground_sign_direction"), 4); // west

    BlockState door{"minecraft:wooden_door", {}, 0, {}};
    door.states.set("door_hinge_bit", Tag(std::int8_t(0)));
    transformStates(door, {0, Mirror::LeftRight});
    EXPECT_EQ(stateInt(door, "door_hinge_bit"), 1);

    BlockState wall{"minecraft:cobblestone_wall", {}, 0, {}};
    wall.states.set("wall_connection_type_north", Tag(std::string("tall")));
    wall.states.set("wall_connection_type_south", Tag(std::string("none")));
    wall.states.set("wall_connection_type_east", Tag(std::string("short")));
    wall.states.set("wall_connection_type_west", Tag(std::string("none")));
    transformStates(wall, {1, Mirror::None});
    EXPECT_EQ(stateStr(wall, "wall_connection_type_east"), "tall");
    EXPECT_EQ(stateStr(wall, "wall_connection_type_south"), "short");

    BlockState piston{"levitate:mechanical_piston", {}, 0, {}};
    piston.states.set("levitate:facing", Tag(std::int32_t(2))); // north
    transformStates(piston, {0, Mirror::LeftRight});
    EXPECT_EQ(stateInt(piston, "levitate:facing"), 3);
}

// ---- quill ------------------------------------------------------------------------------------------

TEST(SchematicsLogic, SelectionResize) {
    Selection s;
    s.first  = Pos{0, 0, 0};
    s.second = Pos{4, 2, 3};
    EXPECT_EQ(s.size(), (Pos{5, 3, 4}));
    s.resize(5, -2, false); // east face, scroll down: grows by 2
    EXPECT_EQ(s.size(), (Pos{7, 3, 4}));
    s.resize(4, 1, false); // west face, scroll up: shrinks by 1 from the west
    EXPECT_EQ(s.box()[0], (Pos{1, 0, 0}));
    s.resize(1, 10, false); // never below one block
    EXPECT_EQ(s.size().y, 1);
    s.resize(1, -1, true); // inside: inverted
    EXPECT_EQ(s.size().y, 1);
    s.scrollRange(200);
    EXPECT_EQ(s.range, 100);
    s.scrollRange(-500);
    EXPECT_EQ(s.range, 1);
}

TEST(SchematicsLogic, Raycast) {
    // looking east along y = 0.5 from x = 0.5: the wall at x = 5 is hit on its west face
    auto h = raycast(0.5, 0.5, 0.5, 1, 0, 0, 75, [](Pos p) { return p.x == 5; });
    ASSERT_TRUE(h);
    EXPECT_EQ(h->first, (Pos{5, 0, 0}));
    EXPECT_EQ(h->second, 4);
    // looking down onto the floor at y = -3: top face
    h = raycast(0.5, 0.5, 0.5, 0, -1, 0, 75, [](Pos p) { return p.y == -3; });
    ASSERT_TRUE(h);
    EXPECT_EQ(h->second, 1);
    // out of range
    EXPECT_FALSE(raycast(0.5, 0.5, 0.5, 1, 0, 0, 3, [](Pos p) { return p.x == 5; }));
    // the start block itself never counts
    EXPECT_FALSE(raycast(0.5, 0.5, 0.5, 0, 0, 1, 2, [](Pos p) { return p == Pos{0, 0, 0}; }));
}

// ---- schematicannon ---------------------------------------------------------------------------------

TEST(SchematicsLogic, ReplaceModes) {
    PlaceQuery q;
    q.worldSolid = true;
    q.mode       = ReplaceMode::DontReplaceSolid;
    EXPECT_FALSE(shouldPlace(q));
    q.worldSolid = false;
    EXPECT_TRUE(shouldPlace(q));
    q.placingAir = true;
    EXPECT_FALSE(shouldPlace(q));
    q.mode = ReplaceMode::ReplaceWithEmpty;
    EXPECT_TRUE(shouldPlace(q));
    q            = {};
    q.mode       = ReplaceMode::ReplaceWithSolid;
    q.worldSolid = true;
    EXPECT_FALSE(shouldPlace(q));
    q.schematicSolid = true;
    EXPECT_TRUE(shouldPlace(q));
    q.mode       = ReplaceMode::ReplaceWithAny;
    q.nearCannon = true;
    EXPECT_FALSE(shouldPlace(q));
    q.nearCannon   = false;
    q.worldHasData = true;
    EXPECT_FALSE(shouldPlace(q));
    q.protectData = false;
    EXPECT_TRUE(shouldPlace(q));
}

TEST(SchematicsLogic, PrintOrderDefersBrittle) {
    PrintCursor           c({0, 0, 0}, {1, 1, 1});
    std::vector<Pos>      order;
    std::set<Pos>         brittle{{1, 0, 0}};
    while (auto p = c.next([&](Pos q) { return brittle.contains(q); })) order.push_back(*p);
    ASSERT_EQ(order.size(), 8u);
    EXPECT_EQ(order[0], (Pos{0, 0, 0}));
    EXPECT_EQ(order[1], (Pos{0, 0, 1})); // x first, then z
    EXPECT_EQ(order[2], (Pos{1, 0, 1}));
    EXPECT_EQ(order[3], (Pos{0, 1, 0})); // next layer
    EXPECT_EQ(order.back(), (Pos{1, 0, 0})); // deferred last
    EXPECT_TRUE(c.done());

    // saving and loading mid-way continues identically
    PrintCursor a({0, 0, 0}, {2, 0, 2});
    a.next([](Pos) { return false; });
    a.next([](Pos) { return false; });
    auto b = PrintCursor::load({0, 0, 0}, {2, 0, 2}, a.save());
    EXPECT_EQ(*a.next([](Pos) { return false; }), *b.next([](Pos) { return false; }));
}

TEST(SchematicsLogic, FlightAndFuel) {
    EXPECT_EQ(flightTicks({0, 0, 0}, {1, 0, 0}), 10);
    EXPECT_EQ(flightTicks({0, 0, 0}, {256, 0, 0}), 64);
    Fuel f;
    EXPECT_TRUE(f.needsRefill());
    f.refill(400);
    EXPECT_EQ(f.remaining, 400);
    f.clamp(100);
    EXPECT_EQ(f.remaining, 100);
}

TEST(SchematicsLogic, Checklist) {
    Checklist c;
    c.require("minecraft:stone", 130);
    c.require("minecraft:dirt", 2);
    c.require("minecraft:air", 5);
    c.collect("minecraft:dirt", 3);
    c.collect("minecraft:diamond", 3);
    EXPECT_EQ(c.missing("minecraft:stone"), 130);
    EXPECT_LE(c.missing("minecraft:dirt"), 0);
    EXPECT_FALSE(c.gathered.contains("minecraft:diamond"));
    auto e = c.entries([](std::string const& id) { return id == "minecraft:stone" ? std::string("Stone") : std::string("Dirt"); });
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].item, "minecraft:stone"); // missing first
    EXPECT_TRUE(e[1].complete);
    EXPECT_EQ(checklistAmount(130), " x130 | 2▤ +2");
    std::vector<Checklist::Entry> many;
    for (int i = 0; i < 7; ++i) many.push_back({"x", 1, false});
    auto pages = checklistPages(many, {}, true, "not loaded");
    EXPECT_EQ(pages.size(), 3u); // disclaimer + 5 + 2
}

// ---- blueprint ----------------------------------------------------------------------------------------

TEST(SchematicsLogic, BlueprintCells) {
    auto one = blueprintCells({0, 0, 0}, 3, 2, 1);
    ASSERT_EQ(one.size(), 1u);
    auto two = blueprintCells({0, 0, 0}, 3, 2, 2); // facing south: right is east
    ASSERT_EQ(two.size(), 4u);
    EXPECT_EQ(two[1], (Pos{1, 0, 0}));
    EXPECT_EQ(two[2], (Pos{0, 1, 0}));
    auto three = blueprintCells({5, 5, 5}, 2, 2, 3); // facing north: right is west
    EXPECT_EQ(three[0], (Pos{6, 4, 5}));
    EXPECT_EQ(three[8], (Pos{4, 6, 5}));
    auto floor = blueprintCells({0, 0, 0}, 1, 2, 2); // on the floor, player facing north
    EXPECT_EQ(floor[2], (Pos{0, 0, -1}));
}
