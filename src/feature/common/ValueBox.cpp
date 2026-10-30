#include "feature/common/ValueBox.h"

#include <cmath>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common {

using Json = nlohmann::json;

namespace {
constexpr double      kMaxEditRange = 8.0;

Json posJson(core::GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

std::optional<core::GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return core::GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

/// The block at `pos` if the player is in that dimension and within reach.
Block const* reachableBlock(Player& player, core::GlobalPos const& pos) {
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return nullptr;
    auto const& p  = player.getPosition();
    double      dx = p.x - (pos.x + 0.5), dy = p.y - (pos.y + 0.5), dz = p.z - (pos.z + 0.5);
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kMaxEditRange) return nullptr;
    auto& region = player.getDimensionBlockSource();
    if (!region.hasChunksAt(game::blockPos(pos), 0, false)) return nullptr;
    return &region.getBlock(game::blockPos(pos));
}

/// Deferred actions (inventory edits after the interaction event finished).
std::vector<std::function<void()>>& pending() {
    static std::vector<std::function<void()>> v;
    return v;
}

/// Server ticks since install (hold timeouts).
std::uint64_t& serverTick() {
    static std::uint64_t t = 0;
    return t;
}

/// A companion's right-click waiting for the client's "held / released" answer.
struct PendingHold {
    std::uint64_t        token = 0;
    core::GlobalPos      pos;
    std::uint64_t        since = 0;
    valuebox::HoldAction onShort, onHold;
};
std::unordered_map<std::int64_t, PendingHold>& holds() {
    static std::unordered_map<std::int64_t, PendingHold> m;
    return m;
}
/// Ticks a hold waits for the client's answer (the client answers after 4 ticks of holding at
/// most, plus the round trip).
constexpr std::uint64_t kHoldTimeout = 100;

Player* playerById(std::int64_t key) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = key;
    return level->getPlayer(id);
}
} // namespace

// ---- value boxes -------------------------------------------------------------------------------
namespace valuebox {

namespace {
std::unordered_map<std::string, std::vector<BoxSpec>>& registry() {
    static std::unordered_map<std::string, std::vector<BoxSpec>> m;
    return m;
}

BoxSpec const* boxById(std::string const& blockId, std::string const& boxId) {
    auto const* list = boxesOf(blockId);
    if (!list) return nullptr;
    for (auto const& b : *list)
        if (b.id == boxId) return &b;
    return nullptr;
}

std::string formatValue(BoxSpec const& box, Block const& block, ValueSettings s) {
    if (box.format) return box.format(block, s);
    return std::to_string(s.value);
}

std::string title(BoxSpec const& box, Player const* player) {
    if (box.label.empty()) return box.labelFallback;
    return player ? trFor(*player, box.label, box.labelFallback) : tr(box.label, box.labelFallback);
}

void openForm(Player& player, core::GlobalPos const& pos, BoxSpec const& box, Block const& block) {
    auto board = box.board(block);
    auto cur   = core::common::clampSettings(board, box.get(pos));
    ll::form::CustomForm form(title(box, &player));
    form.appendLabel(trFor(player, "lv.common.valuebox.current", "Current: {}", formatValue(box, block, cur)));
    if (board.rows.size() > 1) {
        form.appendDropdown("row", trFor(player, "lv.common.valuebox.row", "Mode"), board.rows, static_cast<std::size_t>(cur.row));
    }
    if (board.maxValue > 0) {
        std::vector<std::string> steps;
        if (board.maxValue <= 32) {
            for (int v = 0; v <= board.maxValue; ++v) steps.push_back(formatValue(box, block, {cur.row, v}));
            form.appendStepSlider("value", trFor(player, "lv.common.valuebox.value", "Value"), steps, static_cast<std::size_t>(cur.value));
        } else {
            form.appendSlider("value", trFor(player, "lv.common.valuebox.value", "Value"), 0, board.maxValue, 1, cur.value);
        }
    }
    std::string boxId = box.id;
    form.sendTo(player, [pos, boxId, cur](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        ValueSettings s = cur;
        if (auto it = result->find("row"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) s.row = static_cast<int>(*v);
        }
        if (auto it = result->find("value"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) s.value = static_cast<int>(*v);
            else if (auto const* d = std::get_if<double>(&it->second)) s.value = static_cast<int>(std::lround(*d));
        }
        apply(p, pos, boxId, s);
    });
}
} // namespace

void registerBox(std::string const& blockId, BoxSpec spec) {
    auto& list = registry()[blockId];
    for (auto& b : list) {
        if (b.id == spec.id) {
            b = std::move(spec);
            return;
        }
    }
    list.push_back(std::move(spec));
}

void unregisterBox(std::string const& blockId, std::string const& boxId) {
    auto it = registry().find(blockId);
    if (it == registry().end()) return;
    std::erase_if(it->second, [&](BoxSpec const& b) { return b.id == boxId; });
    if (it->second.empty()) registry().erase(it);
}

std::vector<BoxSpec> const* boxesOf(std::string const& blockId) {
    auto it = registry().find(blockId);
    return it == registry().end() ? nullptr : &it->second;
}

BoxSpec const* boxAt(Block const& block, int face, Vec const& hit) {
    auto const* list = boxesOf(block.getTypeName());
    if (!list) return nullptr;
    for (auto const& b : *list) {
        if (!b.centre) continue;
        if (auto c = b.centre(block, face); c && core::common::testBoxHit(hit, *c, b.scale)) return &b;
    }
    return nullptr;
}

void openEditor(Player& player, GlobalPos const& pos, std::string const& boxId) {
    auto const* block = reachableBlock(player, pos);
    if (!block) return;
    auto const* box = boxById(block->getTypeName(), boxId);
    if (!box || !box->board || !box->get) return;
    if (!net::isCompanion(player)) {
        openForm(player, pos, *box, *block);
        log().info("value box {} at {} opened for {} (form)", boxId, pos.key(), player.getRealName());
        return;
    }
    auto board = box->board(*block);
    auto cur   = core::common::clampSettings(board, box->get(pos));
    Json labels = Json::array();
    if (static_cast<long long>(board.rows.size()) * (board.maxValue + 1) <= 4096) {
        for (int r = 0; r < static_cast<int>(board.rows.size()); ++r) {
            Json row = Json::array();
            for (int v = 0; v <= board.maxValue; ++v) row.push_back(formatValue(*box, *block, {r, v}));
            labels.push_back(std::move(row));
        }
    }
    net::send(
        player,
        "valuebox.open",
        {
            {"pos",       posJson(pos)                },
            {"box",       boxId                       },
            {"title",     title(*box, &player)        },
            {"rows",      board.rows                  },
            {"max",       board.maxValue              },
            {"milestone", board.milestoneInterval     },
            {"row",       cur.row                     },
            {"value",     cur.value                   },
            {"labels",    labels                      },
        }
    );
    log().info("value box {} at {} opened for {} (native)", boxId, pos.key(), player.getRealName());
}

bool awaitHold(Player& player, GlobalPos const& pos, HoldAction onShort, HoldAction onHold) {
    if (!net::isCompanion(player)) return false;
    static std::uint64_t nextToken = 0;
    auto key   = player.getOrCreateUniqueID().rawID;
    auto token = ++nextToken;
    holds()[key] = PendingHold{token, pos, serverTick(), std::move(onShort), std::move(onHold)};
    net::send(player, "valuebox.hold", {{"token", token}, {"pos", posJson(pos)}});
    return true;
}

bool holdPending(Player const& player, GlobalPos const& pos) {
    auto it = holds().find(const_cast<Player&>(player).getOrCreateUniqueID().rawID);
    return it != holds().end() && it->second.pos == pos;
}

/// The client's answer to `valuebox.hold`: {token, state: "hold" | "short" | "cancel"}.
void onRelease(Player& player, Json const& msg) {
    auto it = holds().find(player.getOrCreateUniqueID().rawID);
    if (it == holds().end() || it->second.token != msg.value("token", std::uint64_t{0})) return;
    auto hold = std::move(it->second);
    holds().erase(it);
    std::string what = msg.value("state", std::string{"cancel"});
    log().info("value box hold at {} by {}: {}", hold.pos.key(), player.getRealName(), what);
    if (what == "hold" && hold.onHold) hold.onHold(player);
    else if (what == "short" && hold.onShort) hold.onShort(player);
}

bool apply(Player& player, GlobalPos const& pos, std::string const& boxId, ValueSettings settings) {
    auto const* block = reachableBlock(player, pos);
    if (!block) return false;
    auto const* box = boxById(block->getTypeName(), boxId);
    if (!box || !box->set || !box->board) return false;
    auto s = core::common::clampSettings(box->board(*block), settings);
    box->set(pos, s, player);
    log().info("value box {} at {} set to {} by {}", boxId, pos.key(), formatValue(*box, *block, s), player.getRealName());
    return true;
}

} // namespace valuebox

// ---- filter slots ------------------------------------------------------------------------------
namespace filterslot {

namespace {
std::unordered_map<std::string, std::vector<SlotSpec>>& registry() {
    static std::unordered_map<std::string, std::vector<SlotSpec>> m;
    return m;
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}
} // namespace

void registerSlot(std::string const& blockId, SlotSpec spec) {
    auto& list = registry()[blockId];
    for (auto& s : list) {
        if (s.id == spec.id) {
            s = std::move(spec);
            return;
        }
    }
    list.push_back(std::move(spec));
}

void unregisterSlot(std::string const& blockId, std::string const& slotId) {
    auto it = registry().find(blockId);
    if (it == registry().end()) return;
    std::erase_if(it->second, [&](SlotSpec const& s) { return s.id == slotId; });
    if (it->second.empty()) registry().erase(it);
}

SlotSpec const* slotAt(Block const& block, int face, Vec const& hit) {
    auto it = registry().find(block.getTypeName());
    if (it == registry().end()) return nullptr;
    for (auto const& s : it->second) {
        if (!s.centre) continue;
        if (auto c = s.centre(block, face); c && core::common::testBoxHit(hit, *c, s.scale)) return &s;
    }
    return nullptr;
}

bool hasSlots(std::string const& blockId) { return registry().contains(blockId); }

std::vector<SlotSpec> const* slotsOf(std::string const& blockId) {
    auto it = registry().find(blockId);
    return it == registry().end() ? nullptr : &it->second;
}

ItemStack get(GlobalPos const& pos, std::string const& slotId) {
    auto const* d = blockdata::get(pos);
    if (!d) return items::empty();
    auto f = d->find("filter");
    if (f == d->end() || !f->is_object()) return items::empty();
    auto s = f->find(slotId);
    return s == f->end() ? items::empty() : items::fromJson(*s);
}

void set(GlobalPos const& pos, ItemStack const& filter, std::string const& slotId) {
    auto& d = blockdata::edit(pos);
    if (!d["filter"].is_object()) d["filter"] = Json::object();
    if (items::isEmpty(filter)) d["filter"].erase(slotId);
    else d["filter"][slotId] = items::toJson(items::withCount(filter, 1));
    itemrender::refresh(pos); // the client mod draws the new filter item
}

/// Display items of a block's filter slots for the client mod (Create's FilteringRenderer): the
/// filter item on every face its slot is on, facing out, at a quarter block (half a value box).
void displayItems(GlobalPos const& pos, Block const& block, std::vector<std::pair<std::int64_t, itemrender::DisplayItem>>& out) {
    auto it = registry().find(block.getTypeName());
    if (it == registry().end()) return;
    for (auto const& spec : it->second) {
        if (!spec.centre) continue;
        ItemStack stack = get(pos, spec.id);
        if (items::isEmpty(stack)) continue;
        auto slotBase = static_cast<std::int64_t>(std::hash<std::string>{}(spec.id) & 0xffffff) * 8;
        for (int face = 0; face < 6; ++face) {
            auto c = spec.centre(block, face);
            if (!c) continue;
            static constexpr int kNormal[6][3]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            // a slot less than a pixel under the face is drawn on the face (the model covers it)
            std::array<double, 3> at = *c;
            int                   axis = face < 2 ? 1 : face < 4 ? 2 : 0;
            double                plane = kNormal[face][axis] > 0 ? 1.0 : 0.0;
            if (std::abs(at[axis] - plane) < 1 / 16.0) at[axis] = plane;
            itemrender::DisplayItem d;
            d.stack    = stack;
            d.at       = itemrender::facingFace(face);
            d.at.x     = pos.x + at[0] + kNormal[face][0] * .01;
            d.at.y     = pos.y + at[1] + kNormal[face][1] * .01;
            d.at.z     = pos.z + at[2] + kNormal[face][2] * .01;
            d.at.scale = static_cast<float>(spec.scale) * .5f;
            out.emplace_back(slotBase + face, std::move(d));
        }
    }
}

bool matches(GlobalPos const& pos, ItemStack const& stack, std::string const& slotId) {
    // predicates are cached per stored filter document (parsing it per item would be slow)
    static std::unordered_map<std::string, items::ItemPredicate> cache;
    auto const* d = blockdata::get(pos);
    if (!d) return true;
    auto f = d->find("filter");
    if (f == d->end() || !f->is_object()) return true;
    auto s = f->find(slotId);
    if (s == f->end() || s->is_null()) return true;
    auto key = s->dump();
    auto it  = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 1024) cache.clear();
        it = cache.emplace(key, filter::predicate(items::fromJson(*s))).first;
    }
    return it->second(stack);
}

void interact(Player& player, GlobalPos const& pos, SlotSpec const& slot) {
    auto const& held     = player.getSelectedItem();
    ItemStack   previous = get(pos, slot.id);
    bool        creative = player.isCreative();
    if (items::isEmpty(held)) {
        if (items::isEmpty(previous)) return;
        set(pos, items::empty(), slot.id);
        if (filter::isFilterItem(previous) && !creative) giveOrDrop(player, previous);
        log().info("filter slot at {} cleared by {}", pos.key(), player.getRealName());
    } else {
        ItemStack toApply = items::withCount(held, 1);
        set(pos, toApply, slot.id);
        if (filter::isFilterItem(toApply) && !creative) {
            player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
            player.sendInventory(false);
        }
        if (filter::isFilterItem(previous) && !creative) giveOrDrop(player, previous);
        log().info("filter slot at {} set to {} by {}", pos.key(), items::idOf(toApply), player.getRealName());
    }
    if (slot.onChanged) slot.onChanged(pos);
}

} // namespace filterslot

// ---- interaction / hover -----------------------------------------------------------------------

namespace {
/// Hover text for the client mod's overlay (box label + value, filter slot contents).
Json hoverLines(Player& player, core::GlobalPos const& pos, int face, core::common::Vec const& hit) {
    Json        lines = Json::array();
    auto const* block = reachableBlock(player, pos);
    if (!block) return lines;
    if (auto const* box = valuebox::boxAt(*block, face, hit); box && box->get && box->board) {
        auto s = core::common::clampSettings(box->board(*block), box->get(pos));
        lines.push_back(valuebox::title(*box, &player));
        lines.push_back("§e" + valuebox::formatValue(*box, *block, s));
        // ScrollValueRenderer's hover tip
        lines.push_back("§7" + trFor(player, "create.gui.value_settings.hold_to_edit", "Click and hold to edit"));
    }
    if (auto const* slot = filterslot::slotAt(*block, face, hit)) {
        auto f = filterslot::get(pos, slot->id);
        lines.push_back(trFor(player, "create.logistics.filter", "Filter"));
        lines.push_back(items::isEmpty(f) ? "§7" + trFor(player, "lv.common.filter.empty", "(empty)") : "§e" + items::displayName(f));
        if (slot->onHold) lines.push_back("§7" + trFor(player, "create.gui.value_settings.hold_to_edit", "Click and hold to edit"));
    }
    return lines;
}
} // namespace

class ValueBoxModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mHandler = interactions::onUseOn({}, [this](interactions::UseOnContext const& c) { return onClick(c); }, interactions::priority::Highest);
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            ++mTick;
            ++serverTick();
            // a client that never answers (left, lagging): the hold is dropped
            std::erase_if(holds(), [](auto const& e) { return serverTick() - e.second.since > kHoldTimeout; });
            if (pending().empty()) return;
            auto run = std::exchange(pending(), {});
            for (auto& fn : run) fn();
        }));
        static bool providerAdded = false;
        if (!providerAdded) {
            providerAdded = true;
            itemrender::registerProvider([](std::string_view id) { return filterslot::hasSlots(std::string(id)); }, filterslot::displayItems);
        }
        net::on("valuebox.set", [](Player& player, Json const& msg) {
            auto pos = posFrom(msg.value("pos", Json()));
            if (!pos) return;
            valuebox::apply(player, *pos, msg.value("box", std::string{}), {msg.value("row", 0), msg.value("value", 0)});
        });
        net::on("valuebox.release", valuebox::onRelease);
        net::on("common.hover", [](Player& player, Json const& msg) {
            auto pos = posFrom(msg.value("pos", Json()));
            auto hit = msg.value("hit", Json::array());
            if (!pos || !hit.is_array() || hit.size() != 3) return;
            core::common::Vec h{hit[0].get<double>(), hit[1].get<double>(), hit[2].get<double>()};
            net::send(player, "common.hover", {{"pos", posJson(*pos)}, {"lines", hoverLines(player, *pos, msg.value("face", 0), h)}});
        });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        interactions::remove(mHandler);
        net::off("valuebox.set");
        net::off("valuebox.release");
        net::off("common.hover");
        holds().clear();
    }

private:
    /// A right-click on a block: filter slots and value boxes (Create's canInteract: sneaking
    /// players place / use normally, spectators and players who may not build don't edit machines).
    interactions::Result onClick(interactions::UseOnContext const& c) {
        using interactions::Result;
        auto& player = c.player;
        if (c.sneaking || player.isSpectator() || !player.canUseAbility(AbilitiesIndex::Build)) return Result::Pass;
        auto key = player.getOrCreateUniqueID().rawID;
        auto pos = c.pos;
        // the button is still held on a box / slot whose hold waits for the client's answer
        if (valuebox::holdPending(player, pos)) return Result::Consume;
        if (auto const* slot = filterslot::slotAt(c.block, c.face, c.hit)) {
            if (debounced(key, pos)) return Result::Consume;
            auto slotId   = slot->id;
            auto blockId  = c.blockId;
            auto interact = [key, pos, slotId, blockId] {
                auto* p  = playerById(key);
                auto  it = filterslot::registry().find(blockId);
                if (!p || it == filterslot::registry().end()) return;
                // the block may have changed since the click
                auto& region = p->getDimensionBlockSource();
                if (static_cast<int>(p->getDimensionId()) != pos.dim || lv::game::blockIdAt(region, lv::game::blockPos(pos)) != blockId) return;
                for (auto const& s : it->second)
                    if (s.id == slotId) filterslot::interact(*p, pos, s);
            };
            if (slot->onHold) {
                auto onHold = slot->onHold;
                auto onShort = [interact](Player&) { pending().push_back(interact); };
                if (valuebox::awaitHold(player, pos, onShort, [onHold, pos](Player& p) { onHold(p, pos); })) return Result::Consume;
            }
            pending().push_back(interact);
            return Result::Consume;
        }
        bool wrench = c.itemId == interactions::kWrenchId;
        if (!wrench && !items::isEmpty(c.item)) return Result::Pass;
        auto const* box = valuebox::boxAt(c.block, c.face, c.hit);
        if (!box || (box->requiresWrench && !wrench)) return Result::Pass;
        if (debounced(key, pos)) return Result::Consume;
        auto boxId   = box->id;
        auto blockId = c.blockId;
        auto open    = [key, pos, boxId] {
            if (auto* p = playerById(key)) valuebox::openEditor(*p, pos, boxId);
        };
        // companions: Create's click and hold (a short click is the box's onShortInteract)
        auto shortClick = [pos, boxId, blockId](Player& p) {
            auto const* list = valuebox::boxesOf(blockId);
            if (!list) return;
            auto& region = p.getDimensionBlockSource();
            if (static_cast<int>(p.getDimensionId()) != pos.dim || lv::game::blockIdAt(region, lv::game::blockPos(pos)) != blockId) return;
            for (auto const& b : *list)
                if (b.id == boxId && b.onShortClick) b.onShortClick(pos, p);
        };
        if (valuebox::awaitHold(player, pos, shortClick, [open](Player&) { open(); })) return Result::Consume;
        pending().push_back(open);
        return Result::Consume;
    }

    /// Bedrock repeats use-on packets while the button is held: one action per click.
    bool debounced(std::int64_t player, core::GlobalPos const& pos) {
        auto& last = mLast[player];
        bool  skip = last.first == pos && mTick - last.second < 5;
        last       = {pos, mTick};
        return skip;
    }
    std::vector<ll::event::ListenerPtr>                                       mListeners;
    interactions::HandlerId                                                   mHandler = 0;
    std::unordered_map<std::int64_t, std::pair<core::GlobalPos, std::uint64_t>> mLast;
    std::uint64_t                                                             mTick = 0;
};

} // namespace lv::feature::common

LV_REGISTER_MODULE(lv::feature::common::ValueBoxModule);
