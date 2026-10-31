// Create's general commands (AllCommands, SPEC §6):
//   /create clone <begin> <end> <destination> [skipBlocks]   (op) CloneCommand: blocks, their block
//       entities and Levitate's per-block data, plus the super glue of the area
//   /create debuginfo                                         DebugInfoCommand: version report
//   /create util replaceInCommandBlocks <begin> <end> <toReplace> <replaceWith>   (op)
//   /create u ...                                             Create's redirect of /create util
// (`/c` is an alias of `/create`, registered with the root in mod/Commands.cpp.)
// Selftests use testPos dx 5850..5899.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "ll/api/Versions.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/mod/Manifest.h"
#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/utils/SystemUtils.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/actor/BaseCommandBlock.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/CommandBlockActor.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/BlockData.h"
#include "feature/contraption/Contraption.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "selftest/SelfTest.h"

#ifdef LV_CLIENT
#include "mc/client/player/LocalPlayer.h"

#include "client/Client.h"
#include "client/integration/Clipboard.h"
#endif

namespace lv::feature::integration {

// Command parameter types need external linkage.
struct CloneParams {
    CommandPosition begin{};
    CommandPosition end{};
    CommandPosition destination{};
};
struct ReplaceInCommandBlocksParams {
    CommandPosition begin{};
    CommandPosition end{};
    std::string     toReplace;
    std::string     replaceWith;
};

namespace {

namespace bd = common::blockdata;
namespace ct = contraption;
namespace cc = contraption::cc;
using GP     = core::GlobalPos;

constexpr int kLatestCommandVersion = 0x7FFFFFFF; ///< newest rules for relative positions
/// Java's game rule commandModificationBlockLimit (default), which Create's clone checks.
constexpr long long kModificationBlockLimit = 32768;
constexpr int       kNoUpdates              = 2; ///< clients only (Create's Block.UPDATE_CLIENTS)

/// Translated in the sender's language (the default one for the console / command blocks).
template <class... Args>
std::string text(CommandOrigin const& origin, std::string_view key, std::string_view fallback, Args const&... args) {
    if (auto* p = commands::playerOf(origin)) return trFor(*p, key, fallback, args...);
    return tr(key, fallback, args...);
}

BlockPos posOf(CommandPosition const& p, CommandOrigin const& origin) {
    return game::blockPosOf(p.getPosition(kLatestCommandVersion, origin, Vec3{0, 0, 0}));
}

struct Area {
    BlockPos min, max;
    [[nodiscard]] long long volume() const {
        return (long long)(max.x - min.x + 1) * (max.y - min.y + 1) * (max.z - min.z + 1);
    }
};
Area areaOf(BlockPos const& a, BlockPos const& b) {
    return {
        BlockPos{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
        BlockPos{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}
    };
}

bool loaded(BlockSource& region, Area const& a) {
    if (a.min.y < region.getMinHeight() || a.max.y >= region.getMaxHeight()) return false;
    AABB box{
        Vec3{float(a.min.x), float(a.min.y), float(a.min.z)},
        Vec3{float(a.max.x + 1), float(a.max.y + 1), float(a.max.z + 1)}
    };
    return region.hasChunksAt(box, false);
}

// ---- clone (Create's CloneCommand) -------------------------------------------------------------

struct CloneResult {
    int blocks = 0;
    int glue   = 0;
};

/// One block of the source area, taken before anything is written (the areas may overlap).
struct Snapshot {
    BlockPos                     target;
    Block const*                 block = nullptr;
    std::unique_ptr<CompoundTag> blockEntity;
    std::optional<bd::Json>      data; ///< Levitate's BlockData document (our blocks' "block entity")
};

/// Puts the copied items into the placed container when the new block entity didn't take them
/// (as the contraption engine does when it places a carried container).
void restoreContainer(BlockSource& region, BlockPos const& bp, CompoundTag const& be) {
    if (!be.contains("Items") || !be.at("Items").is_array()) return;
    auto container = region.tryGetContainer(bp);
    if (!container) return;
    for (int i = 0; i < container->getContainerSize(); ++i)
        if (!container->getItem(i).isNull()) return; // already loaded from the NBT
    for (auto const& tag : be.at("Items").get<ListTag>()) {
        if (!tag || !tag.hold(Tag::Type::Compound)) continue;
        auto const& item = static_cast<CompoundTag const&>(*tag);
        int         slot = item.contains("Slot") ? int(item.at("Slot").get<ByteTag>().data) : -1;
        if (slot < 0 || slot >= container->getContainerSize()) continue;
        container->setItem(slot, ItemStack::fromTag(item));
    }
}

/// Create's cloneBlocks: every block of the area (block entity NBT and BlockData included) to the
/// same place relative to `diff`. Returns how many blocks were set.
int cloneBlocks(BlockSource& region, int dim, Area const& src, BlockPos const& diff) {
    bd::flush(); // owners with cached data (kinetics) write it back first
    auto                  saveCtx = SaveContextFactory::createCloneSaveContext();
    std::vector<Snapshot> blocks, beBlocks;
    for (int z = src.min.z; z <= src.max.z; ++z) {
        for (int y = src.min.y; y <= src.max.y; ++y) {
            for (int x = src.min.x; x <= src.max.x; ++x) {
                BlockPos p{x, y, z};
                Snapshot s;
                s.target = BlockPos{x + diff.x, y + diff.y, z + diff.z};
                s.block  = &region.getBlock(p);
                if (auto* be = region.getBlockEntity(p)) {
                    auto tag = std::make_unique<CompoundTag>();
                    if (be->save(*tag, *saveCtx)) s.blockEntity = std::move(tag);
                }
                if (auto const* doc = bd::get(GP{dim, x, y, z})) s.data = *doc;
                (s.blockEntity ? beBlocks : blocks).push_back(std::move(s));
            }
        }
    }
    std::vector<Snapshot*> all;
    all.reserve(blocks.size() + beBlocks.size());
    for (auto& s : blocks) all.push_back(&s);
    for (auto& s : beBlocks) all.push_back(&s);

    // Create clears the targets' inventories first (Clearable.tryClear), so nothing drops; our
    // blocks' documents are taken away without their erase listeners for the same reason.
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        auto const& t = (*it)->target;
        if (auto container = region.tryGetContainer(t)) container->removeAllItems();
        GP gp{dim, t.x, t.y, t.z};
        if (bd::has(gp)) (void)bd::take(gp);
    }

    int pastes = 0;
    for (auto* s : all) {
        std::shared_ptr<BlockActor> be;
        if (s->blockEntity) {
            auto& tag = *s->blockEntity;
            tag["x"]  = s->target.x;
            tag["y"]  = s->target.y;
            tag["z"]  = s->target.z;
            if (tag.contains("pairx") && tag.contains("pairz")) { // a double chest's other half moves too
                tag["pairx"] = tag.at("pairx").get<IntTag>().data + diff.x;
                tag["pairz"] = tag.at("pairz").get<IntTag>().data + diff.z;
            }
            be = BlockActor::create(tag, s->target);
        }
        BlockChangeContext ctx{};
        bool               set = be ? region.setBlock(s->target, *s->block, kNoUpdates, be, nullptr, ctx)
                                    : region.setBlock(s->target, *s->block, kNoUpdates, nullptr, ctx);
        if (set) ++pastes;
        if (s->blockEntity) {
            restoreContainer(region, s->target, *s->blockEntity);
            if (auto* placed = region.getBlockEntity(s->target)) placed->refresh(region);
        }
        if (s->data) bd::put(GP{dim, s->target.x, s->target.y, s->target.z}, *s->data);
    }
    for (auto it = all.rbegin(); it != all.rend(); ++it) region.updateNeighborsAt((*it)->target);
    return pastes;
}

/// Create's cloneGlue (SuperGlueEntity.collectCropped): every glue region overlapping the area,
/// cropped to it, is added again moved by `diff`; crops of a single block are skipped.
int cloneGlue(int dim, Area const& src, BlockPos const& diff) {
    int pastes = 0;
    for (auto const& g : ct::glueRegions(dim)) { // a copy: the new regions are not visited
        cc::GlueBox in{
            {std::max(g.min.x, src.min.x), std::max(g.min.y, src.min.y), std::max(g.min.z, src.min.z)},
            {std::min(g.max.x, src.max.x), std::min(g.max.y, src.max.y), std::min(g.max.z, src.max.z)}
        };
        if (in.max.x < in.min.x || in.max.y < in.min.y || in.max.z < in.min.z) continue;
        auto size = in.size();
        if (size.x == 1 && size.y == 1 && size.z == 1) continue;
        ct::addGlue(dim, in.moved(cc::Pos{diff.x, diff.y, diff.z}));
        ++pastes;
    }
    return pastes;
}

CloneResult cloneArea(BlockSource& region, int dim, Area const& src, BlockPos const& destination, bool withBlocks) {
    BlockPos    diff{destination.x - src.min.x, destination.y - src.min.y, destination.z - src.min.z};
    CloneResult r;
    if (withBlocks) r.blocks = cloneBlocks(region, dim, src, diff);
    r.glue = cloneGlue(dim, src, diff);
    return r;
}

void runClone(CommandOrigin const& origin, CommandOutput& output, CloneParams const& params, bool withBlocks) {
    if (!commands::isAdmin(origin))
        return output.error(text(origin, "lv.integration.need_op", "You need operator permission."));
    auto* dimension = origin.getDimension();
    if (!dimension) return output.error(text(origin, "lv.integration.not_loaded", "That position is not loaded"));
    auto& region = dimension->getBlockSourceFromMainChunkSource();
    int   dim    = static_cast<int>(dimension->getDimensionId());
    auto  src    = areaOf(posOf(params.begin, origin), posOf(params.end, origin));
    auto  dest   = posOf(params.destination, origin);
    Area  target{dest, BlockPos{dest.x + (src.max.x - src.min.x), dest.y + (src.max.y - src.min.y), dest.z + (src.max.z - src.min.z)}};
    if (src.volume() > kModificationBlockLimit) {
        return output.error(text(
            origin,
            "lv.integration.clone.too_big",
            "Too many blocks in the specified area (maximum {0}, specified {1})",
            kModificationBlockLimit,
            src.volume()
        ));
    }
    if (!loaded(region, src) || !loaded(region, target))
        return output.error(text(origin, "lv.integration.not_loaded", "That position is not loaded"));
    auto r = cloneArea(region, dim, src, dest, withBlocks);
    log().info(
        "clone: {} {} {}..{} {} {} to {} {} {}: {} blocks, glue applied {} times",
        src.min.x, src.min.y, src.min.z, src.max.x, src.max.y, src.max.z, dest.x, dest.y, dest.z, r.blocks, r.glue
    );
    std::string msg;
    if (withBlocks) msg = text(origin, "lv.integration.clone.blocks", "Successfully cloned {0} Blocks", r.blocks) + "\n";
    msg += text(origin, "lv.integration.clone.glue", "Successfully applied glue {0} times", r.glue);
    output.success(msg);
}

// ---- replaceInCommandBlocks (Create's ReplaceInCommandBlocksCommand) ---------------------------

/// Replaces `pattern` (a regular expression, as Java's String.replaceAll) with `replaceWith` in the
/// commands of the command blocks of the area. Returns how many contained `toReplace` literally
/// (Create counts with indexOf). Throws std::regex_error for an invalid pattern.
int replaceInCommandBlocks(
    BlockSource&       region,
    Area const&        area,
    std::string const& toReplace,
    std::string const& replaceWith
) {
    std::regex pattern(toReplace);
    int        count = 0;
    for (int x = area.min.x; x <= area.max.x; ++x) {
        for (int y = area.min.y; y <= area.max.y; ++y) {
            for (int z = area.min.z; z <= area.max.z; ++z) {
                BlockPos p{x, y, z};
                auto*    be = region.getBlockEntity(p);
                if (!be || be->getType() != BlockActorType::CommandBlock) continue;
                auto&       base    = *static_cast<CommandBlockActor&>(*be).mBaseCB;
                std::string command = *base.mCommand;
                if (command.find(toReplace) != std::string::npos) ++count;
                auto replaced = std::regex_replace(command, pattern, replaceWith);
                if (replaced == command) continue;
                base.setCommand(region, p, replaced);
                be->refresh(region);
            }
        }
    }
    return count;
}

void runReplace(CommandOrigin const& origin, CommandOutput& output, ReplaceInCommandBlocksParams const& params) {
    if (!commands::isAdmin(origin))
        return output.error(text(origin, "lv.integration.need_op", "You need operator permission."));
    auto* dimension = origin.getDimension();
    if (!dimension) return output.error(text(origin, "lv.integration.not_loaded", "That position is not loaded"));
    auto& region = dimension->getBlockSourceFromMainChunkSource();
    auto  begin  = posOf(params.begin, origin);
    auto  end    = posOf(params.end, origin);
    if (!loaded(region, areaOf(begin, begin)) || !loaded(region, areaOf(end, end)))
        return output.error(text(origin, "lv.integration.not_loaded", "That position is not loaded"));
    int count = 0;
    try {
        count = replaceInCommandBlocks(region, areaOf(begin, end), params.toReplace, params.replaceWith);
    } catch (std::regex_error const& e) {
        return output.error(text(origin, "lv.integration.replace.bad_pattern", "Invalid pattern: {0}", e.what()));
    }
    log().info("replaceInCommandBlocks \"{}\" -> \"{}\": {} blocks", params.toReplace, params.replaceWith, count);
    if (count == 0) {
        return output.success(
            text(origin, "lv.integration.replace.none", "Couldn't find \"{0}\" anywhere.", params.toReplace)
        );
    }
    output.success(text(origin, "lv.integration.replace.done", "Replaced occurrences in {0} blocks.", count));
}

// ---- debuginfo (Create's DebugInfoCommand / DebugInformation) ---------------------------------

std::string versionOf(std::optional<ll::data::Version> const& v) { return v ? v->to_string() : "None"; }

/// Create's printInfo: a collapsible GitHub block with tab-indented "key: value" sections.
std::string debugReport(std::string_view side) {
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;

    auto const& self = Mod::getInstance().getSelf();
    sections.push_back(
        {"Levitate",
         {
             {"Mod Version", versionOf(self.getManifest().version)},
             {"LeviLamina Version", ll::getLoaderVersion().to_string()},
             {"Minecraft Version", ll::getGameVersion().to_string()},
             {"Network Protocol", std::to_string(ll::getNetworkProtocolVersion())},
             {"Platform", ll::isClient() ? "Client (local world)" : "Dedicated Server"},
         }}
    );
    std::string os = ll::sys_utils::getSystemName() + " " + ll::sys_utils::getSystemVersion().to_string();
    if (ll::sys_utils::isWine()) os += " (Wine)";
    sections.push_back({"System Information", {{"Operating System", os}}});
    std::vector<std::pair<std::string, std::string>> mods;
    for (auto& m : ll::mod::ModManagerRegistry::getInstance().mods()) {
        if (m.getName() == self.getName()) continue;
        mods.emplace_back(m.getName(), versionOf(m.getManifest().version));
    }
    if (mods.empty()) mods.emplace_back("Other mods", "none");
    sections.push_back({"Other Mods", std::move(mods)});

    std::string out = "<details>\n<summary>" + std::string(side) + " Info</summary>\n\n```\n";
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i != 0) out += '\n';
        out += sections[i].first + ":\n";
        for (auto const& [key, value] : sections[i].second) out += "\t" + key + ": " + value + "\n";
    }
    out += "```\n\n</details>\n";
    return out;
}

void runDebugInfo(CommandOrigin const& origin, CommandOutput& output) {
    auto* player = commands::playerOf(origin);
    auto  sending = text(origin, "create.command.debuginfo.sending", "Collecting debug information...");
    if (player) player->sendMessage(sending);
    auto report = debugReport(ll::isClient() ? "Client" : "Server");
    log().info("debuginfo:\n{}", report);
    std::string msg = report;
#ifdef LV_CLIENT
    // Create's client copies the report to the clipboard: here only for the host of the local world
    if (auto* local = client::currentPlayer(); player && local
        && local->getOrCreateUniqueID().rawID == player->getOrCreateUniqueID().rawID
        && client::integration::copyToClipboard(report)) {
        msg += "§a" + text(origin, "create.command.debuginfo.saved_to_clipboard", "Debug information has been copied to your clipboard.");
    }
#endif
    output.success(msg);
}

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    // /create clone: usage line without arguments (Create's executes on the literal)
    cmd.overload().text("clone").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin))
            return output.error(text(origin, "lv.integration.need_op", "You need operator permission."));
        output.success(text(
            origin,
            "lv.integration.clone.usage",
            "Clones all blocks as well as super glue from the specified area to the target destination"
        ));
    });
    cmd.overload<CloneParams>().text("clone").required("begin").required("end").required("destination").execute(
        [](CommandOrigin const& origin, CommandOutput& output, CloneParams const& params) {
            runClone(origin, output, params, true);
        }
    );
    cmd.overload<CloneParams>()
        .text("clone")
        .required("begin")
        .required("end")
        .required("destination")
        .text("skipBlocks")
        .execute([](CommandOrigin const& origin, CommandOutput& output, CloneParams const& params) {
            runClone(origin, output, params, false);
        });

    cmd.overload().text("debuginfo").execute([](CommandOrigin const& origin, CommandOutput& output) {
        runDebugInfo(origin, output);
    });

    // /create util ... and its redirect /create u ...
    for (char const* util : {"util", "u"}) {
        cmd.overload<ReplaceInCommandBlocksParams>()
            .text(util)
            .text("replaceInCommandBlocks")
            .required("begin")
            .required("end")
            .required("toReplace")
            .required("replaceWith")
            .execute([](CommandOrigin const& origin, CommandOutput& output, ReplaceInCommandBlocksParams const& params) {
                runReplace(origin, output, params);
            });
    }
});

// ---- selftests (testPos dx 5850..5899) --------------------------------------------------------

namespace {

using lv::selftest::Context;

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack makeItem(std::string const& id, int count) {
    CompoundTag tag;
    tag["Name"]   = id;
    tag["Count"]  = static_cast<uchar>(count);
    tag["Damage"] = static_cast<short>(0);
    return ItemStack::fromTag(tag);
}

} // namespace

LV_REGISTER_SELFTEST("integration.clone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5850);
    p.y += 1;
    auto q = at(p, 6, 0, 0); // the destination
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(q), "destination loaded");
    auto& region = regionOf(ctx);
    ctx.require(ctx.setBlock(p, "minecraft:stone"), "stone placed");
    ctx.require(ctx.setBlock(at(p, 1, 0, 0), "minecraft:chest"), "chest placed");
    ctx.setBlock(at(p, 1, 1, 0), "minecraft:oak_planks");
    for (int dx = 0; dx <= 2; ++dx)
        for (int dy = 0; dy <= 1; ++dy) ctx.cleanupBlock(at(q, dx, dy, 0));
    co_await ctx.ticks(2);
    auto chestPos = game::blockPos(at(p, 1, 0, 0));
    auto chest    = region.tryGetContainer(chestPos);
    ctx.require(bool(chest), "the chest has a container");
    chest->setItem(3, makeItem("minecraft:diamond", 5));
    ct::addGlue(p.dim, cc::GlueBox::of({p.x, p.y, p.z}, {p.x + 1, p.y + 1, p.z}));
    ct::addGlue(p.dim, cc::GlueBox::of({p.x + 2, p.y, p.z}, {p.x + 4, p.y, p.z})); // crops to one block: skipped

    Area src{game::blockPos(p), game::blockPos(at(p, 2, 1, 0))};
    auto r = cloneArea(region, p.dim, src, game::blockPos(q), true);
    ctx.check(r.blocks >= 3, "the 3 solid blocks set, got " + std::to_string(r.blocks));
    ctx.check(r.glue == 1, "glue applied once, got " + std::to_string(r.glue));
    ctx.check(game::blockIdAt(region, game::blockPos(q)) == "minecraft:stone", "stone cloned");
    ctx.check(game::blockIdAt(region, game::blockPos(at(q, 1, 1, 0))) == "minecraft:oak_planks", "planks cloned");
    ctx.check(game::blockIdAt(region, game::blockPos(at(q, 1, 0, 0))) == "minecraft:chest", "chest cloned");
    auto copy = region.tryGetContainer(game::blockPos(at(q, 1, 0, 0)));
    ctx.check(copy && copy->getItem(3).mCount == 5, "the chest's items were cloned");
    ctx.check(chest->getItem(3).mCount == 5, "the source chest keeps its items");
    bool glued = false;
    for (auto const& g : ct::glueRegions(p.dim))
        if (g == cc::GlueBox::of({q.x, q.y, q.z}, {q.x + 1, q.y + 1, q.z})) glued = true;
    ctx.check(glued, "the glue region was cloned to the destination");

    // skipBlocks: only the glue
    auto q2 = at(q, 0, 3, 0);
    ctx.cleanupBlock(q2);
    auto r2 = cloneArea(region, p.dim, src, game::blockPos(q2), false);
    ctx.check(r2.blocks == 0 && r2.glue == 1, "skipBlocks clones the glue only");
    ctx.check(game::blockIdAt(region, game::blockPos(q2)) == "minecraft:air", "skipBlocks leaves the blocks");

    chest->removeAllItems();
    if (copy) copy->removeAllItems();
    ct::clearGlue(p.dim, {p.x + 3, p.y, p.z}, 6);
});

LV_REGISTER_SELFTEST("integration.replace_in_command_blocks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5860);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    ctx.require(ctx.setBlock(p, "minecraft:command_block"), "command block placed");
    ctx.require(ctx.setBlock(at(p, 2, 0, 0), "minecraft:command_block"), "second command block placed");
    co_await ctx.ticks(2);
    auto* be  = region.getBlockEntity(game::blockPos(p));
    auto* be2 = region.getBlockEntity(game::blockPos(at(p, 2, 0, 0)));
    ctx.require(be && be->getType() == BlockActorType::CommandBlock, "the command block has its block entity");
    ctx.require(be2 && be2->getType() == BlockActorType::CommandBlock, "the second one too");
    auto& cb  = *static_cast<CommandBlockActor&>(*be).mBaseCB;
    auto& cb2 = *static_cast<CommandBlockActor&>(*be2).mBaseCB;
    cb.setCommand(region, game::blockPos(p), "say hello hello");
    cb2.setCommand(region, game::blockPos(at(p, 2, 0, 0)), "say bye");

    Area area{game::blockPos(p), game::blockPos(at(p, 2, 0, 0))};
    int  n = replaceInCommandBlocks(region, area, "hello", "world");
    ctx.check(n == 1, "one command block contained the text, got " + std::to_string(n));
    ctx.check(*cb.mCommand == "say world world", "every occurrence replaced: " + std::string(*cb.mCommand));
    ctx.check(*cb2.mCommand == "say bye", "the other command stays: " + std::string(*cb2.mCommand));
    ctx.check(replaceInCommandBlocks(region, area, "nothing", "x") == 0, "nothing found");
});

// CKinetics.minecartContraptionInContainers (MinecartContraptionItem.canFitInsideContainerItems)
LV_REGISTER_SELFTEST("integration.minecart_contraption_in_containers", [](Context& ctx) {
    auto& cfg  = lv::config::mut<bearings::Config>();
    bool  prev = cfg.minecartContraptionInContainers;
    cfg.minecartContraptionInContainers = false;
    ctx.check(!bearings::fitsInsideContainerItems(bearings::kMinecartContraption), "a minecart contraption doesn't fit by default");
    ctx.check(!bearings::fitsInsideContainerItems(bearings::kChestMinecartContraption), "nor a chest minecart contraption");
    ctx.check(bearings::fitsInsideContainerItems("minecraft:stone"), "other items fit");
    cfg.minecartContraptionInContainers = true;
    ctx.check(bearings::fitsInsideContainerItems(bearings::kMinecartContraption), "it fits when the config allows it");
    cfg.minecartContraptionInContainers = prev;
});

} // namespace lv::feature::integration
