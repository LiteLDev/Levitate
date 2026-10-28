// Display unit (U24): the vanilla display targets (Create's SignDisplayTarget and
// LecternDisplayTarget). Display boards and nixie tubes register theirs in Board.cpp / Nixie.cpp.

#include <algorithm>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/LecternBlockActor.h"
#include "mc/world/level/block/actor/SignBlockActor.h"

#include "feature/common/Items.h"
#include "feature/display/Internal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"

namespace lv::feature::display {

namespace {
namespace items = common::items;

constexpr int kSignLines   = 4;  ///< Create's sign target: 4 lines
constexpr int kSignColumns = 15; ///< ⚠ characters per sign line
constexpr int kBookPages   = 50; ///< Bedrock books hold 50 pages
constexpr int kPageColumns = 256; ///< ⚠

BlockActor* actorOf(BlockSource& region, GlobalPos const& pos, BlockActorType type) {
    if (!loadedBlock(region, game::blockPos(pos))) return nullptr;
    auto* be = region.getBlockEntity(game::blockPos(pos));
    return be && be->getType() == type ? be : nullptr;
}

std::vector<std::string> splitLines(std::string const& s) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (true) {
        auto nl = s.find('\n', start);
        out.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

/// Writes `lines` into the front text of a sign starting at `line` (Create's SignDisplayTarget).
void writeSign(BlockSource& region, GlobalPos const& pos, int line, Lines const& lines) {
    auto* be = actorOf(region, pos, BlockActorType::Sign);
    if (!be) return;
    auto& sign = static_cast<SignBlockActor&>(*be);
    auto* text = sign.mTextFront.get();
    if (!text) return;
    auto current = splitLines(text->getMessage());
    current.resize(kSignLines);
    bool changed = false;
    for (int i = 0; i < static_cast<int>(lines.size()) && line + i < kSignLines; ++i) {
        auto value = lines[static_cast<std::size_t>(i)].substr(0, 64);
        if (current[static_cast<std::size_t>(line + i)] == value) continue;
        current[static_cast<std::size_t>(line + i)] = value;
        changed                                    = true;
    }
    if (!changed) return;
    while (!current.empty() && current.back().empty()) current.pop_back();
    std::string joined;
    for (std::size_t i = 0; i < current.size(); ++i) joined += (i ? "\n" : "") + current[i];
    text->setMessage(joined);
    be->refresh(region);
}

/// The text of every page of a book item.
std::vector<std::string> bookPages(ItemStack const& book) {
    std::vector<std::string> pages;
    auto const*              tag = items::userData(book);
    if (!tag) return pages;
    auto j = Json::parse(tag->toSnbt(SnbtFormat::Jsonify, 0), nullptr, false, true);
    if (j.is_discarded() || !j.contains("pages")) return pages;
    for (auto const& p : j["pages"]) pages.push_back(p.is_object() ? p.value("text", std::string{}) : std::string{});
    return pages;
}

void writeLectern(BlockSource& region, GlobalPos const& pos, int line, Lines const& lines) {
    auto* be = actorOf(region, pos, BlockActorType::Lectern);
    if (!be) return;
    auto&     lectern = static_cast<LecternBlockActor&>(*be);
    ItemStack book    = *lectern.mBook;
    if (items::isEmpty(book)) return;
    auto id = items::idOf(book);
    if (id != "minecraft:writable_book" && id != "minecraft:written_book") return;
    auto pages = bookPages(book);
    bool changed = id == "minecraft:writable_book";
    for (int i = 0; i < static_cast<int>(lines.size()) && line + i < kBookPages; ++i) {
        if (static_cast<int>(pages.size()) <= line + i) pages.resize(static_cast<std::size_t>(line + i + 1));
        auto& page = pages[static_cast<std::size_t>(line + i)];
        if (page == lines[static_cast<std::size_t>(i)]) continue;
        page    = lines[static_cast<std::size_t>(i)];
        changed = true;
    }
    if (!changed) return;
    // Create signs a book and quill so the lectern shows a written book ⚠ (title / author)
    Json j = {{"title", "Printed Book"}, {"author", "Data Gatherer"}, {"generation", 0}, {"pages", Json::array()}};
    if (auto const* tag = items::userData(book)) {
        auto old = Json::parse(tag->toSnbt(SnbtFormat::Jsonify, 0), nullptr, false, true);
        if (!old.is_discarded() && old.is_object()) {
            if (old.contains("title")) j["title"] = old["title"];
            if (old.contains("author")) j["author"] = old["author"];
        }
    }
    for (auto const& p : pages) j["pages"].push_back({{"photoname", ""}, {"text", p}});
    auto fresh = items::make("minecraft:written_book");
    auto tag   = CompoundTag::fromSnbt(j.dump());
    if (!fresh || !tag) return;
    fresh->setUserData(std::make_unique<CompoundTag>(std::move(*tag)));
    lectern.mBook       = *fresh;
    lectern.mTotalPages = static_cast<int>(pages.size());
    be->refresh(region);
}

struct Registration {
    Registration() {
        registerTargetProvider(
            [](BlockSource& region, GlobalPos const& pos, Block const&) { return actorOf(region, pos, BlockActorType::Sign) != nullptr; },
            Target{
                .id    = "sign",
                .stats = [](BlockSource&, GlobalPos const&) { return TargetStats{kSignLines, kSignColumns, "sign"}; },
                .accept = [](BlockSource& region, GlobalPos const& t, int line, Lines const& lines, Context&) { writeSign(region, t, line, lines); },
                .acceptSections = nullptr,
                .lineName = [](int line, Player const& p) { return trFor(p, "create.display_target.line", "Line {}", line + 1); },
            }
        );
        registerTargetProvider(
            [](BlockSource& region, GlobalPos const& pos, Block const&) { return actorOf(region, pos, BlockActorType::Lectern) != nullptr; },
            Target{
                .id    = "lectern",
                .stats = [](BlockSource&, GlobalPos const&) { return TargetStats{kBookPages, kPageColumns, "lectern"}; },
                .accept = [](BlockSource& region, GlobalPos const& t, int line, Lines const& lines, Context&) {
                    // a multi-line source fills one page with its lines
                    std::string page;
                    for (std::size_t i = 0; i < lines.size(); ++i) page += (i ? "\n" : "") + lines[i];
                    writeLectern(region, t, line, {page});
                },
                .acceptSections = nullptr,
                .lineName = [](int line, Player const& p) { return trFor(p, "create.display_target.page", "Page {}", line + 1); },
            }
        );
    }
};
Registration const gRegistration;

} // namespace

/// Selftest helpers: the sign's front text / a lectern book's page.
std::string signText(BlockSource& region, GlobalPos const& pos) {
    auto* be = actorOf(region, pos, BlockActorType::Sign);
    if (!be) return {};
    auto* text = static_cast<SignBlockActor&>(*be).mTextFront.get();
    return text ? text->getMessage() : std::string{};
}

std::string lecternPage(BlockSource& region, GlobalPos const& pos, int page) {
    auto* be = actorOf(region, pos, BlockActorType::Lectern);
    if (!be) return {};
    auto pages = bookPages(*static_cast<LecternBlockActor&>(*be).mBook);
    return page < static_cast<int>(pages.size()) ? pages[static_cast<std::size_t>(page)] : std::string{};
}

} // namespace lv::feature::display
