// Selftests of the schematics unit (testPos dx 2900..2999).

#include <cmath>
#include <filesystem>
#include <functional>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "core/schematics/Inflate.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/display/Clipboard.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "selftest/SelfTest.h"

namespace lv::feature::schematics {

namespace {

using selftest::Context;
namespace items = common::items;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

void put(Context& ctx, GlobalPos const& p, std::string const& id) {
    ctx.cleanupBlock(p);
    placeId(p, id);
}

void putBlock(Context& ctx, GlobalPos const& p, Block const& b) {
    ctx.cleanupBlock(p);
    place(p, b);
}

void clearBox(Context& ctx, GlobalPos const& o, int sx, int sy, int sz) {
    for (int x = 0; x < sx; ++x)
        for (int y = 0; y < sy; ++y)
            for (int z = 0; z < sz; ++z) {
                auto q = at(o, x, y, z);
                ctx.cleanupBlock(q);
                if (idAt(q) != "minecraft:air") clearAt(q);
            }
}

ll::coro::CoroTask<bool> waitFor(Context& ctx, std::function<bool()> cond, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        if (cond()) co_return true;
        co_await ctx.ticks(1);
    }
    co_return cond();
}

Block const* stairs(int weirdo) {
    cs::BlockState s{"minecraft:oak_stairs", {}, 0, {}};
    s.states.set("weirdo_direction", cs::nbt::Tag(std::int32_t(weirdo)));
    return resolve(s);
}

/// A small test structure (3x2x3): stone floor, planks, a stairs and a torch.
void buildSample(Context& ctx, GlobalPos const& o) {
    clearBox(ctx, o, 3, 3, 3);
    for (int x = 0; x < 3; ++x)
        for (int z = 0; z < 3; ++z) put(ctx, at(o, x, 0, z), "minecraft:stone");
    put(ctx, at(o, 0, 1, 0), "minecraft:oak_planks");
    put(ctx, at(o, 2, 1, 0), "minecraft:oak_planks");
    if (auto const* s = stairs(0)) putBlock(ctx, at(o, 1, 1, 2), *s);
    put(ctx, at(o, 2, 1, 2), "minecraft:torch");
}

std::string tmpName(Context const& ctx) { return "lv_selftest_" + ctx.name().substr(ctx.name().find('.') + 1); }

struct FileCleanup {
    std::filesystem::path path;
    ~FileCleanup() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

/// Fast cannon for tests (one shot per tick); restored afterwards.
struct FastCannon {
    int delay;
    FastCannon() : delay(config::mut<Config>().schematicannonDelay) { config::mut<Config>().schematicannonDelay = 1; }
    ~FastCannon() { config::mut<Config>().schematicannonDelay = delay; }
};

void fillChest(GlobalPos const& chest, std::vector<std::pair<std::string, int>> const& contents) {
    auto* region = regionOf(chest.dim);
    for (auto const& [id, n] : contents)
        if (auto s = makeItem(id, n)) (void)common::items::insert(*region, bp(chest), 1, *s, false);
}

} // namespace

LV_REGISTER_SELFTEST("schematics.registry", [](Context& ctx) {
    for (auto id : {kTable, kCannon, kBlueprintBlock}) ctx.require(game::blockWith(id) != nullptr, std::string(id) + " is in the behavior pack");
    for (auto id : {kEmptySchematic, kSchematic, kQuill, kBlueprintItem}) ctx.require(makeItem(id).has_value(), std::string(id) + " is an item");
    ctx.check(config().maxSchematics == 10 && config().maxTotalSchematicSize == 256 && config().schematicannonShotsPerGunpowder == 400,
              "Create's CSchematics defaults");
});

LV_REGISTER_SELFTEST("schematics.save_load", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2900);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    buildSample(ctx, o);
    auto dir  = playerDir("lv_selftest");
    auto file = saveArea(dir, tmpName(ctx), true, o.dim, cp(o), cp(at(o, 2, 1, 2)));
    ctx.require(file.has_value(), "the area is saved");
    FileCleanup cleanup{dir / upath(*file)};
    std::string err;
    auto        loaded = loadSchematic("lv_selftest", *file, &err);
    ctx.require(loaded != nullptr, "the file loads: " + err);
    auto const& s = loaded->schematic;
    ctx.check(s.size == cs::Pos{3, 2, 3}, "size 3x2x3");
    ctx.check(s.stateAt({0, 0, 0}) && s.stateAt({0, 0, 0})->name == "minecraft:stone", "stone floor");
    ctx.check(s.stateAt({0, 1, 0}) && s.stateAt({0, 1, 0})->name == "minecraft:oak_planks", "planks");
    ctx.check(s.stateAt({1, 1, 1}) && s.stateAt({1, 1, 1})->isAir(), "air is saved");
    ctx.check(loaded->unmapped.empty(), "every block resolves");
    auto const* st = s.stateAt({1, 1, 2});
    ctx.check(st && resolve(*st) == stairs(0), "stairs keep their state");
});

LV_REGISTER_SELFTEST("schematics.quill", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2910);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    buildSample(ctx, o);
    auto& player = ctx.player();
    auto  quill  = makeItem(kQuill);
    ctx.require(quill.has_value(), "quill item");
    player.getInventory().setItem(player.getSelectedItemSlot(), *quill);
    selectionOf(player) = {};
    // clicking the side of blocks selects them; clicking the top of a solid block selects the block above
    quillUse(player, std::pair{cp(o), 2});
    quillUse(player, std::pair{cp(at(o, 2, 0, 2)), 1});
    auto& sel = selectionOf(player);
    ctx.require(sel.complete(), "two corners selected");
    ctx.check(sel.size() == cs::Pos{3, 2, 3}, "the second corner is above the clicked block");
    sel.resize(5, -1, false);
    ctx.check(sel.size().x == 4, "Ctrl-scroll on the east face grows the box");
    sel.resize(5, 1, false);
    auto name = tmpName(ctx);
    auto dir  = playerDir(player.getRealName());
    auto file = quillSave(player, name, true);
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    ctx.check(!selectionOf(player).complete(), "the selection is cleared");
    auto d = schematicData(player.getSelectedItem());
    ctx.require(d.has_value(), "the quill became a written schematic (Save and Upload Immediately)");
    ctx.check(d->file == *file && d->owner == player.getRealName(), "file and owner");
    ctx.check(d->size == cs::Pos{3, 2, 3}, "size stored on the item");
    player.getInventory().setItem(player.getSelectedItemSlot(), items::empty());
});

LV_REGISTER_SELFTEST("schematics.print_compare", [](Context& ctx) -> ll::coro::CoroTask<> {
    // save an area, print it elsewhere with a schematicannon, compare
    auto o = ctx.testPos(2920);
    o.y += 1;
    auto target = at(o, 8, 0, 0);
    auto cannon = at(o, 4, 0, -4);
    auto chest  = at(cannon, 1, 0, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(target), "target area loaded");
    buildSample(ctx, o);
    clearBox(ctx, target, 3, 3, 3);
    auto& player = ctx.player();
    auto  owner  = player.getRealName();
    auto  dir    = playerDir(owner);
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(o), cp(at(o, 2, 1, 2)));
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    auto item = makeSchematic(*file, owner);
    ctx.require(item.has_value(), "written schematic");
    auto d = *schematicData(*item);
    d.anchor   = cp(target);
    d.deployed = true;
    setSchematicData(*item, d);

    put(ctx, cannon, std::string(kCannon));
    put(ctx, chest, "minecraft:chest");
    co_await ctx.ticks(2);
    fillChest(chest, {{"minecraft:stone", 16}, {"minecraft:oak_planks", 4}, {"minecraft:oak_stairs", 2}, {"minecraft:torch", 2}, {"minecraft:gunpowder", 1}});
    FastCannon fast;
    cannonSetSlot(cannon, 0, *item);
    co_await ctx.ticks(3);
    auto v = cannonView(cannon);
    ctx.check(v.status == "ready", "the cannon loads the schematic: " + v.status);
    ctx.check(v.toPlace == 13, "13 blocks to place, got " + std::to_string(v.toPlace));
    cannonAction(&player, cannon, {{"op", "play"}});
    bool done = co_await waitFor(ctx, [&] { return cannonView(cannon).status == "finished"; }, 600);
    ctx.require(done, "the cannon finishes: " + cannonView(cannon).status + " missing " + cannonView(cannon).missing);
    co_await ctx.ticks(40); // last blocks in flight
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 3; ++z) {
                auto src = blockAt(at(o, x, y, z));
                auto dst = blockAt(at(target, x, y, z));
                ctx.cleanupBlock(at(target, x, y, z));
                ctx.check(src && dst && src == dst, "block " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + " is " + (src ? std::string(src->getTypeName()) : "?") + ", printed " + (dst ? std::string(dst->getTypeName()) : "?"));
            }
    ctx.check(isItem(cannonSlot(cannon, 1), kEmptySchematic), "the used schematic comes back empty");
    ctx.check(cannonView(cannon).fuel == 400 - 13, "one gunpowder gave 400 shots");
    lv::log().info("schematics: print compare ok ({} blocks)", 13);
});

LV_REGISTER_SELFTEST("schematics.missing_items", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2940);
    o.y += 1;
    auto target = at(o, 6, 0, 0);
    auto cannon = o;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, target, 2, 2, 2);
    put(ctx, at(o, 10, 0, 0), "minecraft:gold_block");
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(at(o, 10, 0, 0)), cp(at(o, 10, 0, 0)));
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    auto item = makeSchematic(*file, player.getRealName());
    auto d    = *schematicData(*item);
    d.anchor   = cp(target);
    d.deployed = true;
    setSchematicData(*item, d);
    put(ctx, cannon, std::string(kCannon));
    FastCannon fast;
    cannonSetSlot(cannon, 0, *item);
    cannonSetSlot(cannon, 4, *makeItem("minecraft:gunpowder", 1));
    cannonAction(&player, cannon, {{"op", "skipMissing"}, {"value", false}});
    co_await ctx.ticks(3);
    cannonAction(&player, cannon, {{"op", "play"}});
    ctx.check(co_await waitFor(ctx, [&] { return cannonView(cannon).status == "missingBlock"; }, 60), "missing item pauses the cannon: " + cannonView(cannon).status);
    ctx.check(cannonView(cannon).missing == "minecraft:gold_block", "the missing item is shown");
    ctx.check(cannonView(cannon).state == "PAUSED", "paused");
    // skip missing: the cannon goes on and finishes without placing
    cannonAction(&player, cannon, {{"op", "skipMissing"}, {"value", true}});
    ctx.check(co_await waitFor(ctx, [&] { return cannonView(cannon).status == "finished"; }, 60), "skipping finishes: " + cannonView(cannon).status);
    ctx.check(idAt(target) == "minecraft:air", "nothing placed");
    // no gunpowder: out of gunpowder
    put(ctx, at(o, 0, 0, 3), std::string(kCannon));
    auto c2 = at(o, 0, 0, 3);
    cannonSetSlot(c2, 0, *item);
    co_await ctx.ticks(3);
    cannonAction(&player, c2, {{"op", "play"}});
    ctx.check(co_await waitFor(ctx, [&] { return cannonView(c2).status == "noGunpowder"; }, 40), "no gunpowder: " + cannonView(c2).status);
});

LV_REGISTER_SELFTEST("schematics.rotated_print", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2950);
    o.y += 1;
    auto target = at(o, 10, 0, 4);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    buildSample(ctx, o);
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(o), cp(at(o, 2, 1, 2)));
    ctx.require(file.has_value(), "saved");
    FileCleanup   cleanup{dir / upath(*file)};
    SchematicData d;
    d.file      = *file;
    d.owner     = player.getRealName();
    d.anchor    = cp(target);
    d.deployed  = true;
    d.transform = {1, cs::Mirror::None};
    d.size      = {3, 2, 3};
    for (int x = -3; x <= 3; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = -3; z <= 3; ++z) ctx.cleanupBlock(at(target, x, y, z));
    int n = printInstantly(&player, o.dim, d, false);
    ctx.check(n == 13, "13 blocks printed, got " + std::to_string(n));
    // clockwise 90: local (x, z) -> (-z, x)
    ctx.check(idAt(at(target, 0, 1, 0)) == "minecraft:oak_planks", "local 0,1,0 stays at the anchor");
    ctx.check(idAt(at(target, 0, 1, 2)) == "minecraft:oak_planks", "local 2,1,0 -> 0,1,2");
    auto const* st = blockAt(at(target, -2, 1, 1));
    ctx.check(st && st == stairs(2), "the stairs turned from east to south");
});

LV_REGISTER_SELFTEST("schematics.java_import", [](Context& ctx) {
    // a Create .nbt: oak stairs facing east, an unknown mod block
    using namespace cs::nbt;
    Compound root;
    List     size{Type::Int, {Tag(std::int32_t(2)), Tag(std::int32_t(1)), Tag(std::int32_t(1))}};
    root.set("size", Tag(size));
    List     palette{Type::Compound, {}};
    Compound a;
    a.set("Name", Tag(std::string("minecraft:oak_stairs")));
    Compound props;
    props.set("facing", Tag(std::string("east")));
    props.set("half", Tag(std::string("bottom")));
    a.set("Properties", Tag(props));
    palette.items.emplace_back(a);
    Compound b;
    b.set("Name", Tag(std::string("othermod:widget")));
    palette.items.emplace_back(b);
    root.set("palette", Tag(palette));
    List blocks{Type::Compound, {}};
    for (int i = 0; i < 2; ++i) {
        Compound e;
        e.set("pos", Tag(List{Type::Int, {Tag(std::int32_t(i)), Tag(std::int32_t(0)), Tag(std::int32_t(0))}}));
        e.set("state", Tag(std::int32_t(i)));
        blocks.items.emplace_back(e);
    }
    root.set("blocks", Tag(blocks));
    auto bytes = cs::gzipStored(write(Tag(root), "", Endian::Big));
    auto dir   = playerDir("lv_selftest");
    auto path  = dir / "lv_selftest_import.nbt";
    ctx.require(writeFile(path, bytes), "file written");
    FileCleanup cleanup{path};
    clearSchematicCache();
    std::string err;
    auto        loaded = loadSchematic("lv_selftest", "lv_selftest_import.nbt", &err);
    ctx.require(loaded != nullptr, "Create .nbt loads: " + err);
    ctx.check(loaded->blocks[0] == stairs(0), "oak stairs facing east -> weirdo_direction 0");
    ctx.check(loaded->unmapped.size() == 1 && loaded->unmapped[0] == "othermod:widget", "the unknown block is reported");
});

LV_REGISTER_SELFTEST("schematics.table_upload", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2960);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto& player = ctx.player();
    put(ctx, o, std::string(kTable));
    net::setTestCompanion(player, true);
    struct Unmark {
        Player& p;
        ~Unmark() { net::setTestCompanion(p, false); }
    } unmark{player};
    // a file "on the client": the sample area as .mcstructure bytes
    buildSample(ctx, at(o, 3, 0, 0));
    auto bytes = cs::writeMcstructure(capture(o.dim, cp(at(o, 3, 0, 0)), cp(at(o, 5, 1, 2))));
    auto name  = tmpName(ctx) + ".mcstructure";
    FileCleanup cleanup{playerDir(player.getRealName()) / upath(name)};
    // the empty schematic goes in through the screen
    int slot = ctx.give(player, *makeItem(kEmptySchematic));
    ctx.require(slot >= 0, "empty schematic given");
    openTable(player, o);
    net::injectFromClient(player, "screen.action", {{"type", "schematics.table"}, {"context", {{"pos", posJson(o)}}}, {"action", {{"op", "insert"}, {"slot", slot}}}});
    co_await ctx.ticks(1);
    net::injectFromClient(player, "schematics.upload.begin", {{"pos", posJson(o)}, {"name", name}, {"size", bytes.size()}});
    co_await ctx.ticks(1);
    std::size_t chunk = static_cast<std::size_t>(config().maxSchematicPacketSize);
    for (std::size_t i = 0; i < bytes.size(); i += chunk) net::injectFromClient(player, "schematics.upload.chunk", {{"name", name}, {"data", cs::base64Encode(std::string_view(bytes).substr(i, chunk))}});
    net::injectFromClient(player, "schematics.upload.end", {{"name", name}});
    co_await ctx.ticks(1);
    auto msgs = net::takeTestMessages(player);
    bool ok   = false;
    for (auto const& m : msgs)
        if (m.channel == "schematics.upload.status" && m.payload.value("finished", false)) ok = true;
    ctx.check(ok, "the upload finished");
    auto out = dataIf(o) ? items::fromJson((*dataIf(o))["inv"][1]) : items::empty();
    auto d   = schematicData(out);
    ctx.require(d.has_value(), "the table holds a written schematic");
    ctx.check(d->file == name && d->size == cs::Pos{3, 2, 3}, "written from the uploaded file");
    // too large uploads are refused (Create's maxTotalSchematicSize)
    net::injectFromClient(player, "schematics.upload.begin", {{"pos", posJson(o)}, {"name", "big.nbt"}, {"size", 999999999}});
    bool refused = false;
    for (auto const& m : net::takeTestMessages(player))
        if (m.channel == "schematics.upload.status" && !m.payload.value("ok", true)) refused = true;
    ctx.check(refused, "an oversized upload is refused");
});

LV_REGISTER_SELFTEST("schematics.checklist", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2970);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    buildSample(ctx, at(o, 4, 0, 0));
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(at(o, 4, 0, 0)), cp(at(o, 6, 1, 2)));
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    auto item = makeSchematic(*file, player.getRealName());
    auto d    = *schematicData(*item);
    d.anchor   = cp(at(o, 0, 0, 6));
    d.deployed = true;
    setSchematicData(*item, d);
    put(ctx, o, std::string(kCannon));
    cannonSetSlot(o, 0, *item);
    cannonSetSlot(o, 2, *makeItem("minecraft:book", 1));
    ctx.check(co_await waitFor(ctx, [&] { return isItem(cannonSlot(o, 3), "minecraft:written_book"); }, 60), "a material checklist is printed");
    ctx.check(items::isEmpty(cannonSlot(o, 2)), "the book was used");
});

// U44 (dx 4400..4409): a clipboard in the paper slot comes out as a read-only checklist clipboard
LV_REGISTER_SELFTEST("schematics.checklist_clipboard", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(4400);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    buildSample(ctx, at(o, 4, 0, 0));
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(at(o, 4, 0, 0)), cp(at(o, 6, 1, 2)));
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    auto item = makeSchematic(*file, player.getRealName());
    auto d    = *schematicData(*item);
    d.anchor   = cp(at(o, 0, 0, 6));
    d.deployed = true;
    setSchematicData(*item, d);
    put(ctx, o, std::string(kCannon));
    cannonSetSlot(o, 0, *item);
    cannonSetSlot(o, 2, *makeItem(kClipboard, 1));
    ctx.check(co_await waitFor(ctx, [&] { return isItem(cannonSlot(o, 3), kClipboard); }, 60), "a checklist clipboard is printed");
    auto c = display::clipboard::ofItem(cannonSlot(o, 3));
    ctx.check(c.readOnly && c.type == 2, "the checklist clipboard is written and read-only");
    std::size_t entries = 0;
    bool        icons   = true;
    for (auto const& p : c.pages) {
        ctx.check(static_cast<int>(p.size()) <= display::clipboard::kChecklistPageEntries, "at most 7 entries per page");
        for (auto const& e : p) {
            ++entries;
            icons = icons && !e.icon.is_null();
        }
    }
    // stone, planks, stairs, torch
    ctx.check(entries == 4 && icons, "one entry with its item per material, got " + std::to_string(entries));
    ctx.check(items::isEmpty(cannonSlot(o, 2)), "the clipboard was used");
});

// U44 (dx 4430..4449): schematics keep block entity data and entities (Create's StructureTemplate
// with entities); the creative print restores both, turned with the schematic
LV_REGISTER_SELFTEST("schematics.entities", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(4430);
    o.y += 1;
    auto target = at(o, 8, 0, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(target), "target area loaded");
    clearBox(ctx, o, 3, 2, 3);
    clearBox(ctx, target, 3, 2, 3);
    auto* region = regionOf(o.dim);
    ctx.require(region != nullptr, "region");
    AABB both{Vec3{float(o.x - 1), float(o.y - 1), float(o.z - 4)}, Vec3{float(target.x + 4), float(o.y + 3), float(o.z + 4)}};
    auto clearStands = [&] {
        for (auto* a : region->getEntities(both, 0.f, ActorType::ArmorStand)) a->remove();
    };
    clearStands();
    put(ctx, o, "minecraft:chest");
    co_await ctx.ticks(2);
    fillChest(o, {{"minecraft:diamond", 3}});
    auto level = ll::service::getLevel();
    auto* stand = level->getSpawner().spawnMob(*region, ActorDefinitionIdentifier("minecraft:armor_stand"), nullptr, Vec3{o.x + 1.5f, float(o.y), o.z + 0.5f}, false, false, false);
    ctx.require(stand != nullptr, "an armor stand");
    co_await ctx.ticks(2);
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, tmpName(ctx), true, o.dim, cp(o), cp(at(o, 2, 1, 2)));
    ctx.require(file.has_value(), "saved");
    FileCleanup cleanup{dir / upath(*file)};
    std::string err;
    auto        loaded = loadSchematic(player.getRealName(), *file, &err);
    ctx.require(loaded != nullptr, "loaded: " + err);
    ctx.check(loaded->schematic.blockEntities.size() == 1, "the chest's data is saved, " + std::to_string(loaded->schematic.blockEntities.size()) + " block entities");
    ctx.check(loaded->schematic.entityTags.size() == 1, "the armor stand is saved, " + std::to_string(loaded->schematic.entityTags.size()) + " entities");
    clearStands();
    // creative print, turned a quarter clockwise
    auto item = makeSchematic(*file, player.getRealName());
    auto d    = *schematicData(*item);
    d.anchor             = cp(target);
    d.transform.rotation = 1;
    d.deployed           = true;
    printInstantly(&player, o.dim, d, false);
    co_await ctx.ticks(2);
    auto chestAt = gp(o.dim, d.anchor + cs::apply(d.transform, {0, 0, 0}));
    ctx.cleanupBlock(chestAt);
    int diamonds = 0;
    if (auto h = common::items::find({*region, bp(chestAt), -1})) {
        common::Access a{*region, bp(chestAt), -1};
        for (int i = 0; i < h->slots(a); ++i)
            if (items::idOf(h->getSlot(a, i)) == "minecraft:diamond") diamonds += items::countOf(h->getSlot(a, i));
    }
    ctx.check(diamonds == 3, "the printed chest holds the saved items, got " + std::to_string(diamonds));
    auto standAt = d.anchor + cs::apply(d.transform, {1, 0, 0});
    int  stands  = 0;
    for (auto* a : region->getEntities(both, 0.f, ActorType::ArmorStand)) {
        auto const& p = a->getPosition();
        if (int(std::floor(p.x)) == standAt.x && int(std::floor(p.z)) == standAt.z) ++stands;
    }
    ctx.check(stands == 1, "the armor stand is printed where the turned schematic puts it");
    clearStands();
});

LV_REGISTER_SELFTEST("schematics.blueprint", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2980);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    // a 3x3 stone wall facing south (z+1 side free)
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y) {
            put(ctx, at(o, x, y, 0), "minecraft:stone");
            ctx.cleanupBlock(at(o, x, y, 1));
            clearAt(at(o, x, y, 1));
        }
    auto& player = ctx.player();
    ctx.require(placeBlueprint(player, at(o, 1, 1, 0), 3), "the blueprint is placed");
    int sections = 0;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            if (idAt(at(o, x, y, 1)) == kBlueprintBlock) ++sections;
    ctx.check(sections == 9, "a 3x3 blueprint fits, got " + std::to_string(sections));
    auto sec = at(o, 1, 1, 1);
    // sticks: two planks in a column
    setBlueprintSlot(sec, 1, *makeItem("minecraft:oak_planks"));
    setBlueprintSlot(sec, 4, *makeItem("minecraft:oak_planks"));
    auto doc = dataIf(sec);
    ctx.require(doc && doc->contains("blueprint"), "section data");
    auto display = items::fromJson((*doc)["blueprint"]["items"][9]);
    if (items::idOf(display) != "minecraft:stick")
        ctx.check(false, "the display item is inferred from the recipe: " + items::idOf(display) + " (" + debugRecipes("minecraft:stick") + ")");
    // U44: the display item is drawn on the canvas for the client mod (Create's BlueprintRenderer)
    auto const* shown = itemrender::find(sec, 9);
    ctx.check(shown && items::idOf(shown->stack) == "minecraft:stick" && shown->at.scale == 0.5f, "the display item is drawn on the section");
    auto& inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
    ctx.give(player, *makeItem("minecraft:oak_planks", 4));
    int crafted = blueprintCraft(player, sec, false);
    ctx.check(crafted == 4, "one craft gives 4 sticks, got " + std::to_string(crafted));
    crafted = blueprintCraft(player, sec, true);
    ctx.check(crafted == 4, "sneaking crafts until the planks run out, got " + std::to_string(crafted));
    int sticks = 0;
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (items::idOf(inv.getItem(i)) == "minecraft:stick") sticks += items::countOf(inv.getItem(i));
    ctx.check(sticks == 8, "8 sticks in the inventory");
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
    // breaking one section removes the whole blueprint
    destroyAt(at(o, 0, 0, 1), false);
    co_await ctx.ticks(3);
    int left = 0;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            if (idAt(at(o, x, y, 1)) == kBlueprintBlock) ++left;
    ctx.check(left == 0, "the whole blueprint is removed, " + std::to_string(left) + " left");
});

} // namespace lv::feature::schematics
