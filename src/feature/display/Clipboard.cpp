// Display unit (U24): the Clipboard (Create's ClipboardBlockItem / ClipboardBlock /
// ClipboardScreen / ClipboardValueSettingsHandler): checklist pages edited in a screen, placeable
// on surfaces (sneak), written onto display boards and nixie tubes, and copying machine settings
// (value boxes and filter slots of every unit, through U3's contracts) with a right-click and
// pasting them with a left-click.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/form/CustomForm.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/safety/RedactableString.h"

#include "core/common/ValueSettings.h"
#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::display::clipboard {

namespace blockdata = common::blockdata;
namespace items     = common::items;
namespace screens   = common::screens;
namespace valuebox  = common::valuebox;
namespace filterslot = common::filterslot;

namespace {
constexpr char kItemKey[]  = "clipboard";
constexpr char kDataKey[]  = "clipboard";
constexpr char kScreen[]   = "display.clipboard";
constexpr char kFace[]     = "levitate:face"; ///< 0 floor, 1 wall, 2 ceiling
constexpr char kFacing[]   = "levitate:facing";
constexpr char kWritten[]  = "levitate:written";
constexpr int  kMaxPage    = 50; ///< Create clamps the page index to 0..50
constexpr int  kPageEntries = 15; ///< a page holds ~15 single-line entries (Create: 185 px / 12 px) ⚠
constexpr int  kMergePage  = 12; ///< clipboard-to-clipboard copies fill pages up to 12 entries

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

std::map<GlobalPos, bool>& creativeBreaks() {
    static std::map<GlobalPos, bool> m;
    return m;
}

bool hasContent(Contents const& c) {
    for (auto const& p : c.pages)
        for (auto const& e : p)
            if (!e.text.empty()) return true;
    return false;
}

/// Create's onClose clean-up: blank entries go; empty pages go from the end (pages in the middle
/// stay so the open screen keeps its page numbers).
void tidy(Contents& c) {
    for (auto& p : c.pages) std::erase_if(p, [](Entry const& e) { return e.text.find_first_not_of(" \t\n") == std::string::npos; });
    while (!c.pages.empty() && c.pages.back().empty()) c.pages.pop_back();
    c.type = hasContent(c) ? 2 : 0;
    c.previouslyOpenedPage = std::clamp(c.previouslyOpenedPage, 0, std::max(0, static_cast<int>(c.pages.size()) - 1));
}

std::string blockName(BlockSource& region, GlobalPos const& pos) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b) return {};
    auto id   = b->getTypeName();
    auto stack = items::make(id);
    auto name  = stack ? items::displayName(*stack) : std::string{};
    return name.empty() ? id : name;
}

// ---- filter slots of a block (found by probing the faces: U3 exposes slots by hit position) ----
std::vector<filterslot::SlotSpec const*> slotsOf(Block const& block) {
    std::vector<filterslot::SlotSpec const*> out;
    if (!filterslot::hasSlots(block.getTypeName())) return out;
    for (int face = 0; face < 6; ++face) {
        for (int a = 0; a < 8; ++a) {
            for (int b = 0; b < 8; ++b) {
                double u = (a + 0.5) / 8, v = (b + 0.5) / 8;
                core::common::Vec hit;
                switch (face) {
                case 0: hit = {u, 0, v}; break;
                case 1: hit = {u, 1, v}; break;
                case 2: hit = {u, v, 0}; break;
                case 3: hit = {u, v, 1}; break;
                case 4: hit = {0, u, v}; break;
                default: hit = {1, u, v}; break;
                }
                auto const* s = filterslot::slotAt(block, face, hit);
                if (s && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
            }
        }
    }
    return out;
}

/// Takes one item like `filter` from the player's inventory (survival filter pastes).
bool takeFromInventory(Player& player, ItemStack const& filter) {
    auto& inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (items::isEmpty(s) || !items::sameItem(s, filter)) continue;
        inv.removeItem(i, 1);
        player.sendInventory(false);
        return true;
    }
    return false;
}

// ---- contexts ---------------------------------------------------------------------------------
struct Target {
    bool               hand = false;
    GlobalPos          pos;
    int                slot = 0;
};

std::optional<Target> targetOf(Player& player, Json const& ctx) {
    if (auto p = posFrom(ctx.value("pos", Json()))) {
        if (static_cast<int>(player.getDimensionId()) != p->dim) return std::nullopt;
        auto const& pp = player.getPosition();
        double dx = pp.x - (p->x + 0.5), dy = pp.y - (p->y + 0.5), dz = pp.z - (p->z + 0.5);
        if (dx * dx + dy * dy + dz * dz > 100) return std::nullopt; // Create closes the screen beyond 10 blocks
        auto* region = regionOf(p->dim);
        if (!region || idAt(*region, *p) != kClipboardId) return std::nullopt;
        return Target{false, *p, 0};
    }
    int slot = ctx.value("slot", -1);
    if (slot != player.getSelectedItemSlot()) return std::nullopt;
    if (items::idOf(player.getSelectedItem()) != kClipboardId) return std::nullopt;
    return Target{true, {}, slot};
}

Contents readTarget(Player& player, Target const& t) {
    if (t.hand) return ofItem(player.getSelectedItem());
    auto const* doc = blockdata::get(t.pos);
    return doc && doc->contains(kDataKey) ? fromJson((*doc)[kDataKey]) : Contents{};
}

void setWrittenState(BlockSource& region, GlobalPos const& pos, bool written) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b || b->getTypeName() != kClipboardId) return;
    if (intState(*b, kWritten).value_or(0) == (written ? 1 : 0)) return;
    if (auto const* next = withStates(*b, {{kWritten, written ? 1 : 0}})) game::setBlock(region, game::blockPos(pos), *next);
}

void writeTarget(Player& player, Target const& t, Contents const& c) {
    if (t.hand) {
        ItemStack copy = player.getSelectedItem();
        setOnItem(copy, c);
        player.setSelectedItem(copy);
        player.sendInventory(false);
        return;
    }
    blockdata::edit(t.pos)[kDataKey] = toJson(c);
    if (auto* region = regionOf(t.pos.dim)) setWrittenState(*region, t.pos, c.type != 0);
}

Json pagesJson(Contents const& c) {
    Json pages = Json::array();
    for (auto const& p : c.pages) {
        Json page = Json::array();
        for (auto const& e : p) {
            Json entry{{"t", e.text}, {"c", e.checked}};
            if (!e.icon.is_null()) {
                entry["i"] = e.icon;
                entry["n"] = e.amount;
            }
            page.push_back(std::move(entry));
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

std::vector<Entry> entriesFrom(Json const& j) {
    std::vector<Entry> out;
    if (!j.is_array()) return out;
    for (auto const& e : j) {
        if (!e.is_object()) continue;
        out.push_back({e.value("t", std::string{}).substr(0, 512), e.value("c", false)});
        if (static_cast<int>(out.size()) >= kPageEntries) break;
    }
    return out;
}

void applyAction(Player& player, Target const& t, Json const& action) {
    auto c    = readTarget(player, t);
    auto op   = action.value("op", std::string{});
    int  page = std::clamp(action.value("page", c.previouslyOpenedPage), 0, kMaxPage);
    auto ensurePage = [&] {
        while (static_cast<int>(c.pages.size()) <= page) c.pages.emplace_back();
    };
    if (op == "toggle") {
        ensurePage();
        auto& p = c.pages[static_cast<std::size_t>(page)];
        int   i = action.value("entry", -1);
        if (i < 0 || i >= static_cast<int>(p.size())) return;
        p[static_cast<std::size_t>(i)].checked = !p[static_cast<std::size_t>(i)].checked;
    } else if (op == "checks") {
        ensurePage();
        auto& p      = c.pages[static_cast<std::size_t>(page)];
        auto  checks = action.value("checks", Json::array());
        for (std::size_t i = 0; i < p.size() && i < checks.size(); ++i)
            if (checks[i].is_boolean()) p[i].checked = checks[i].get<bool>();
    } else if (op == "erase_checked") {
        if (c.readOnly) return;
        ensurePage();
        std::erase_if(c.pages[static_cast<std::size_t>(page)], [](Entry const& e) { return e.checked; });
    } else if (op == "page") {
        c.previouslyOpenedPage = page;
        writeTarget(player, t, c);
        return;
    } else if (op == "set") {
        if (c.readOnly) return; // Create's read-only clipboards (material checklists) can't be edited
        ensurePage();
        auto  typed = entriesFrom(action.value("entries", Json::array()));
        auto& was   = c.pages[static_cast<std::size_t>(page)];
        // entries keep their item icon while their text is unchanged
        for (std::size_t i = 0; i < typed.size() && i < was.size(); ++i) {
            if (typed[i].text != was[i].text) continue;
            typed[i].icon   = was[i].icon;
            typed[i].amount = was[i].amount;
        }
        was = std::move(typed);
        c.previouslyOpenedPage                  = page;
        log().info("clipboard page {} edited by {} ({} entries)", page + 1, player.getRealName(), c.pages[static_cast<std::size_t>(page)].size());
    } else if (op == "edit") {
        c.previouslyOpenedPage = page;
        writeTarget(player, t, c);
        Json ctx = t.hand ? Json{{"slot", t.slot}} : Json{{"pos", posJson(t.pos)}};
        screens::close(player, kScreen);
        defer([id = player.getOrCreateUniqueID().rawID, ctx] {
            if (auto* p = playerById(id)) screens::openForm(*p, kScreen, ctx);
        });
        return;
    } else {
        return;
    }
    int keep = c.previouslyOpenedPage;
    tidy(c);
    c.previouslyOpenedPage = std::clamp(keep, 0, kMaxPage);
    writeTarget(player, t, c);
}

void openForm(Player& player, Target const& t) {
    auto c    = readTarget(player, t);
    int  page = std::clamp(c.previouslyOpenedPage, 0, std::min(kMaxPage, static_cast<int>(c.pages.size())));
    ll::form::CustomForm form(items::make(kClipboardId) ? items::displayName(*items::make(kClipboardId)) : std::string("Clipboard"));
    std::vector<std::string> pageNames;
    for (int i = 0; i <= std::min(kMaxPage, static_cast<int>(c.pages.size())); ++i) {
        pageNames.push_back(trFor(player, "create.display_target.page", "Page {}", i + 1));
    }
    form.appendDropdown("page", trFor(player, "lv.display.clipboard.page", "Page"), pageNames, static_cast<std::size_t>(page));
    std::vector<Entry> entries = page < static_cast<int>(c.pages.size()) ? c.pages[static_cast<std::size_t>(page)] : std::vector<Entry>{};
    bool readOnly = c.readOnly;
    int  shown    = readOnly ? static_cast<int>(entries.size()) : std::min(kPageEntries, static_cast<int>(entries.size()) + 3);
    for (int i = 0; i < shown; ++i) {
        auto const* e = i < static_cast<int>(entries.size()) ? &entries[static_cast<std::size_t>(i)] : nullptr;
        if (readOnly) {
            // read-only entries are shown, only their check marks change
            auto text = e->text;
            if (e->amount > 0) text += " §8x" + std::to_string(e->amount);
            form.appendToggle("c" + std::to_string(i), text, e->checked);
            continue;
        }
        form.appendInput("t" + std::to_string(i), trFor(player, "lv.display.clipboard.entry", "Entry {}", i + 1), "", e ? e->text : std::string{});
        form.appendToggle("c" + std::to_string(i), "✔", e && e->checked);
    }
    if (!readOnly) form.appendToggle("erase", trFor(player, "create.gui.clipboard.erase_checked", "Erase checked items"), false);
    Json ctx = t.hand ? Json{{"slot", t.slot}} : Json{{"pos", posJson(t.pos)}};
    form.sendTo(player, [ctx, page, shown, readOnly](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto tt = targetOf(p, ctx);
        if (!tt) return;
        if (readOnly) {
            Json checks = Json::array();
            for (int i = 0; i < shown; ++i) {
                bool checked = false;
                if (auto ci = result->find("c" + std::to_string(i)); ci != result->end())
                    if (auto const* u = std::get_if<uint64>(&ci->second)) checked = *u != 0;
                checks.push_back(checked);
            }
            applyAction(p, *tt, {{"op", "checks"}, {"page", page}, {"checks", checks}});
        }
        Json entries = Json::array();
        for (int i = 0; i < shown; ++i) {
            auto ti = result->find("t" + std::to_string(i));
            auto ci = result->find("c" + std::to_string(i));
            std::string text;
            if (ti != result->end())
                if (auto const* s = std::get_if<std::string>(&ti->second)) text = *s;
            bool checked = false;
            if (ci != result->end())
                if (auto const* u = std::get_if<uint64>(&ci->second)) checked = *u != 0;
            entries.push_back({{"t", text}, {"c", checked}});
        }
        applyAction(p, *tt, {{"op", "set"}, {"page", page}, {"entries", entries}});
        if (auto it = result->find("erase"); it != result->end())
            if (auto const* u = std::get_if<uint64>(&it->second); u && *u) applyAction(p, *tt, {{"op", "erase_checked"}, {"page", page}});
        int newPage = page;
        if (auto it = result->find("page"); it != result->end())
            if (auto const* u = std::get_if<uint64>(&it->second)) newPage = static_cast<int>(*u);
        if (newPage != page) {
            applyAction(p, *tt, {{"op", "page"}, {"page", newPage}});
            defer([id = p.getOrCreateUniqueID().rawID, ctx] {
                if (auto* again = playerById(id)) screens::openForm(*again, kScreen, ctx);
            });
        }
    });
}

struct Registration {
    Registration() {
        screens::registerType({
            .id   = kScreen,
            .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
                auto t = targetOf(p, ctx);
                if (!t) return std::nullopt;
                auto c = readTarget(p, *t);
                return Json{{"pages", pagesJson(c)}, {"page", c.previouslyOpenedPage}, {"maxPage", kMaxPage}, {"maxEntries", kPageEntries}, {"readonly", c.readOnly}};
            },
            .action = [](Player& p, Json const& ctx, Json const& action) {
                if (auto t = targetOf(p, ctx)) applyAction(p, *t, action);
            },
            .form = [](Player& p, Json const& ctx) {
                if (auto t = targetOf(p, ctx)) openForm(p, *t);
            },
        });
        // Create's ClipboardValueSettingsHandler.clientTick: what the held clipboard can do with the
        // block the client mod's player looks at ("Clipboard Actions" tip and outline)
        net::on("display.clipboard_query", [](Player& p, Json const& msg) {
            auto pos = posFrom(msg.value("pos", Json()));
            Json out{{"pos", msg.value("pos", Json())}, {"copy", false}, {"paste", false}, {"other", false}};
            auto* region = pos && static_cast<int>(p.getDimensionId()) == pos->dim ? regionOf(pos->dim) : nullptr;
            if (region && items::idOf(p.getSelectedItem()) == kClipboardId && !p.isSpectator()) {
                if (idAt(*region, *pos) == kClipboardId) {
                    out["other"] = true;
                } else {
                    auto c       = ofItem(p.getSelectedItem());
                    out["copy"]  = !copySettings(*region, *pos).is_null();
                    out["paste"] = !c.copied.is_null() && pasteSettings(p, *region, *pos, c.copied, true);
                }
            }
            net::send(p, "display.clipboard_tip", out);
        });
        // a broken clipboard drops itself with its pages (Create copies the components into the drop)
        blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
            if (!doc.contains(kDataKey) || (previous != kClipboardId && !previous.empty())) return;
            auto c        = fromJson(doc[kDataKey]);
            bool creative = creativeBreaks()[pos];
            creativeBreaks().erase(pos);
            if (creative && !hasContent(c)) return;
            defer([pos, c] {
                auto* region = regionOf(pos.dim);
                auto  stack  = items::make(kClipboardId);
                if (!region || !stack) return;
                if (hasContent(c)) setOnItem(*stack, c);
                dropAt(*region, pos, *stack);
            });
        });
    }
};
Registration const gRegistration;

} // namespace

// ---- contents ---------------------------------------------------------------------------------

Contents fromJson(Json const& j) {
    Contents c;
    if (!j.is_object()) return c;
    c.type                 = j.value("type", 0);
    c.previouslyOpenedPage = j.value("page", 0);
    for (auto const& p : j.value("pages", Json::array())) {
        std::vector<Entry> page;
        for (auto const& e : p) {
            Entry entry{e.value("t", std::string{}), e.value("c", false)};
            if (auto it = e.find("i"); it != e.end() && it->is_object()) {
                entry.icon   = *it;
                entry.amount = e.value("n", 0);
            }
            page.push_back(std::move(entry));
        }
        c.pages.push_back(std::move(page));
    }
    if (auto it = j.find("copied"); it != j.end()) c.copied = *it;
    c.readOnly = j.value("readonly", false);
    return c;
}

Json toJson(Contents const& c) {
    Json j = {{"type", c.type}, {"page", c.previouslyOpenedPage}, {"pages", pagesJson(c)}};
    if (!c.copied.is_null()) j["copied"] = c.copied;
    if (c.readOnly) j["readonly"] = true;
    return j;
}

Contents ofItem(ItemStack const& stack) { return fromJson(items::getData(stack, kItemKey)); }

bool isClipboard(ItemStack const& stack) { return items::idOf(stack) == kClipboardId; }

bool addAddress(ItemStack& clipboard, std::string const& address) {
    if (!isClipboard(clipboard) || address.find_first_not_of(" \t\n") == std::string::npos) return false;
    auto c    = ofItem(clipboard);
    auto text = "#" + address;
    for (auto const& page : c.pages)
        for (auto const& e : page)
            if (e.text == text) return false;
    auto free = std::find_if(c.pages.begin(), c.pages.end(), [](auto const& p) { return p.size() <= 11; });
    if (free == c.pages.end()) {
        c.pages.emplace_back();
        free = std::prev(c.pages.end());
    }
    free->push_back({text, false});
    c.type = 2; // ClipboardOverrides.switchTo(WRITTEN)
    setOnItem(clipboard, c);
    return true;
}

std::optional<ItemStack> makeWritten(std::vector<std::vector<Entry>> pages, std::string const& name) {
    auto stack = items::make(kClipboardId);
    if (!stack) return std::nullopt;
    Contents c;
    c.type     = 2;
    c.pages    = std::move(pages);
    c.readOnly = true;
    setOnItem(*stack, c);
    if (!name.empty()) stack->setCustomName(Bedrock::Safety::RedactableString("§r" + name));
    return stack;
}

void setOnItem(ItemStack& stack, Contents const& c) {
    bool empty = c.pages.empty() && c.copied.is_null();
    items::setData(stack, kItemKey, empty ? Json(nullptr) : toJson(c));
}

std::vector<Entry> lastViewedEntries(Contents const& c) {
    if (c.pages.empty()) return {};
    int page = std::clamp(c.previouslyOpenedPage, 0, static_cast<int>(c.pages.size()) - 1);
    return c.pages[static_cast<std::size_t>(page)];
}

// ---- copy / paste of settings -----------------------------------------------------------------

Json copySettings(BlockSource& region, GlobalPos const& pos) {
    auto const* block = loadedBlock(region, game::blockPos(pos));
    if (!block) return nullptr;
    auto id  = block->getTypeName();
    Json out = Json::object();
    if (auto const* boxes = valuebox::boxesOf(id)) {
        Json values = Json::object();
        for (auto const& b : *boxes) {
            if (!b.get) continue;
            auto s       = b.get(pos);
            values[b.id] = {s.row, s.value};
        }
        if (!values.empty()) out["ScrollValue"] = values;
    }
    if (filterslot::hasSlots(id)) {
        Json filters = Json::object();
        for (auto const* s : slotsOf(*block)) filters[s->id] = items::toJson(filterslot::get(pos, s->id));
        if (!filters.empty()) out["Filtering"] = filters;
    }
    return out.empty() ? Json(nullptr) : out;
}

bool pasteSettings(Player& player, BlockSource& region, GlobalPos const& pos, Json const& copied, bool simulate) {
    if (!copied.is_object()) return false;
    auto const* block = loadedBlock(region, game::blockPos(pos));
    if (!block) return false;
    auto id      = block->getTypeName();
    bool applied = false;
    if (auto v = copied.find("ScrollValue"); v != copied.end() && v->is_object() && !v->empty()) {
        if (auto const* boxes = valuebox::boxesOf(id)) {
            for (auto const& b : *boxes) {
                auto it = v->find(b.id);
                if (it == v->end()) {
                    if (boxes->size() != 1) continue;
                    it = v->begin(); // Create's ScrollValueBehaviour: one value per block, any block
                }
                if (!it->is_array() || it->size() != 2) continue;
                applied = true;
                if (!simulate) valuebox::apply(player, pos, b.id, {(*it)[0].get<int>(), (*it)[1].get<int>()});
            }
        }
    }
    if (auto f = copied.find("Filtering"); f != copied.end() && f->is_object() && !f->empty()) {
        auto slots = slotsOf(*block);
        for (auto const* s : slots) {
            auto it = f->find(s->id);
            if (it == f->end()) {
                if (slots.size() != 1) continue;
                it = f->begin();
            }
            ItemStack wanted = items::fromJson(*it);
            applied          = true;
            if (simulate) continue;
            ItemStack previous = filterslot::get(pos, s->id);
            bool      creative = player.isCreative();
            // a filter item is taken from the inventory in survival (Create's FilteringBehaviour.readFromClipboard) ⚠
            if (!items::isEmpty(wanted) && common::filter::isFilterItem(wanted) && !creative && !takeFromInventory(player, wanted)) continue;
            filterslot::set(pos, wanted, s->id);
            if (!items::isEmpty(previous) && common::filter::isFilterItem(previous) && !creative) giveOrDrop(player, previous);
            if (s->onChanged) s->onChanged(pos);
        }
    }
    return applied;
}

// ---- interaction ------------------------------------------------------------------------------

bool useOnBlock(Player& player, BlockSource& region, GlobalPos const& pos, int face, Vec3 const& hit) {
    (void)face;
    (void)hit;
    if (player.isSneaking() || player.isSpectator()) return false;
    auto id = idAt(region, pos);
    // right-click another clipboard: merge its entries (Create's copy from clipboard)
    if (id == kClipboardId) {
        auto const* doc = blockdata::get(pos);
        auto        other = doc && doc->contains(kDataKey) ? fromJson((*doc)[kDataKey]) : Contents{};
        auto        mine  = ofItem(player.getSelectedItem());
        for (auto const& page : other.pages) {
            for (auto const& e : page) {
                bool dup = false;
                for (auto const& p : mine.pages)
                    for (auto const& m : p) dup |= m.text == e.text;
                if (dup || e.text.empty()) continue;
                auto free = std::find_if(mine.pages.begin(), mine.pages.end(), [](auto const& p) { return static_cast<int>(p.size()) < kMergePage; });
                if (free == mine.pages.end()) {
                    if (static_cast<int>(mine.pages.size()) > kMaxPage) break;
                    mine.pages.emplace_back();
                    free = std::prev(mine.pages.end());
                }
                free->push_back({e.text, false});
            }
        }
        mine.type      = mine.pages.empty() ? 0 : 2;
        ItemStack copy = player.getSelectedItem();
        setOnItem(copy, mine);
        player.setSelectedItem(copy);
        player.sendInventory(false);
        actionBar(player, "§a" + trFor(player, "create.clipboard.copied_from_clipboard", "Added entries from Clipboard"));
        log().info("clipboard entries copied from {} by {}", pos.key(), player.getRealName());
        return true;
    }
    auto copied = copySettings(region, pos);
    if (!copied.is_null()) {
        auto c   = ofItem(player.getSelectedItem());
        c.copied = copied;
        if (c.type == 0) c.type = 2; // Create sets the clipboard WRITTEN
        ItemStack copy = player.getSelectedItem();
        setOnItem(copy, c);
        player.setSelectedItem(copy);
        player.sendInventory(false);
        auto name = blockName(region, pos);
        actionBar(player, "§a" + trFor(player, "create.clipboard.copied_from", "Copying settings from {}", "§f" + name + "§a"));
        log().info("clipboard copied settings from {} at {} by {}", id, pos.key(), player.getRealName());
        return true;
    }
    return false;
}

void useInAir(Player& player) {
    if (player.isSneaking()) return;
    screens::open(player, kScreen, {{"slot", player.getSelectedItemSlot()}});
}

bool paste(Player& player, BlockSource& region, GlobalPos const& pos) {
    if (player.isSneaking() || player.isSpectator()) return false;
    auto c = ofItem(player.getSelectedItem());
    if (c.copied.is_null()) return false;
    if (!pasteSettings(player, region, pos, c.copied, true)) return false;
    pasteSettings(player, region, pos, c.copied, false);
    actionBar(player, "§a" + trFor(player, "create.clipboard.pasted_to", "Applied settings to {}", "§f" + blockName(region, pos) + "§a"));
    log().info("clipboard pasted settings to {} by {}", pos.key(), player.getRealName());
    return true;
}

bool useBlock(Player& player, BlockSource& region, GlobalPos const& pos) {
    if (idAt(region, pos) != kClipboardId) return false;
    if (player.isSneaking()) {
        // Create's breakAndCollect: the clipboard goes into the inventory with its pages
        auto doc   = blockdata::take(pos);
        auto stack = items::make(kClipboardId);
        if (stack && doc && doc->contains(kDataKey)) setOnItem(*stack, fromJson((*doc)[kDataKey]));
        game::clearBlock(region, game::blockPos(pos));
        if (stack) giveOrDrop(player, *stack);
        log().info("clipboard at {} picked up by {}", pos.key(), player.getRealName());
        return true;
    }
    screens::open(player, kScreen, {{"pos", posJson(pos)}});
    return true;
}

void placed(Player& player, BlockSource& region, GlobalPos const& pos, int clickedFace, ItemStack const& item) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b || b->getTypeName() != kClipboardId) return;
    int face = clickedFace == 1 ? 0 : clickedFace == 0 ? 2 : 1;
    int facing;
    if (face == 1) {
        static constexpr int toFacing[6]{0, 0, 2, 0, 1, 3};
        facing = toFacing[std::clamp(clickedFace, 0, 5)];
    } else {
        facing = game::opposite(game::playerFacing(player));
    }
    auto c = ofItem(item);
    if (auto const* next = withStates(*b, {{kFace, face}, {kFacing, facing}, {kWritten, c.type != 0 ? 1 : 0}})) {
        game::setBlock(region, game::blockPos(pos), *next);
    }
    blockdata::edit(pos)[kDataKey] = toJson(c);
    log().info("clipboard placed at {}", pos.key());
}

void noteBreak(Player& player, GlobalPos const& pos) { creativeBreaks()[pos] = player.isCreative(); }

} // namespace lv::feature::display::clipboard
