#include "feature/common/ValueBox.h"

#include <cmath>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
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
constexpr char const* kWrenchId     = "levitate:wrench";
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

/// Block-local hit position from the event's click position (relative or absolute).
core::common::Vec localHit(BlockPos const& pos, Vec3 const& click) {
    auto inside = [](double a, double b, double c) {
        auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
        return in(a) && in(b) && in(c);
    };
    if (inside(click.x, click.y, click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

/// Deferred actions (inventory edits after the interaction event finished).
std::vector<std::function<void()>>& pending() {
    static std::vector<std::function<void()>> v;
    return v;
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
            itemrender::DisplayItem d;
            d.stack    = stack;
            d.at       = itemrender::facingFace(face);
            d.at.x     = pos.x + (*c)[0] + kNormal[face][0] * .01;
            d.at.y     = pos.y + (*c)[1] + kNormal[face][1] * .01;
            d.at.z     = pos.z + (*c)[2] + kNormal[face][2] * .01;
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
    }
    if (auto const* slot = filterslot::slotAt(*block, face, hit)) {
        auto f = filterslot::get(pos, slot->id);
        lines.push_back(trFor(player, "create.logistics.filter", "Filter"));
        lines.push_back(items::isEmpty(f) ? "§7" + trFor(player, "lv.common.filter.empty", "(empty)") : "§e" + items::displayName(f));
    }
    return lines;
}
} // namespace

class ValueBoxModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>([this](ll::event::PlayerInteractBlockEvent& ev) {
            auto block = ev.block();
            if (!block) return;
            auto& player = ev.self();
            // as Create's canInteract: sneaking players place / use normally, spectators and
            // players who may not build don't edit machines
            if (player.isSneaking() || player.isSpectator() || !player.canUseAbility(AbilitiesIndex::Build)) return;
            auto  face   = static_cast<int>(ev.face());
            auto  hit    = localHit(ev.blockPos(), ev.clickPos());
            auto  pos    = game::globalPos(player.getDimensionBlockSource(), ev.blockPos());
            auto  key    = player.getOrCreateUniqueID().rawID;

            if (auto const* slot = filterslot::slotAt(*block, face, hit)) {
                ev.cancel();
                if (debounced(key, pos)) return;
                auto slotId = slot->id;
                auto blockId = block->getTypeName();
                pending().push_back([key, pos, slotId, blockId] {
                    auto level = ll::service::getLevel();
                    if (!level) return;
                    ActorUniqueID id;
                    id.rawID  = key;
                    auto* p   = level->getPlayer(id);
                    auto  it  = filterslot::registry().find(blockId);
                    if (!p || it == filterslot::registry().end()) return;
                    // the block may have changed since the click
                    auto& region = p->getDimensionBlockSource();
                    if (static_cast<int>(p->getDimensionId()) != pos.dim || lv::game::blockIdAt(region, lv::game::blockPos(pos)) != blockId) return;
                    for (auto const& s : it->second)
                        if (s.id == slotId) filterslot::interact(*p, pos, s);
                });
                return;
            }
            auto const& held = ev.item();
            bool        tool = items::isEmpty(held) || items::idOf(held) == kWrenchId;
            if (!tool) return;
            if (auto const* box = valuebox::boxAt(*block, face, hit)) {
                ev.cancel();
                if (debounced(key, pos)) return;
                auto boxId = box->id;
                pending().push_back([key, pos, boxId] {
                    auto level = ll::service::getLevel();
                    if (!level) return;
                    ActorUniqueID id;
                    id.rawID = key;
                    if (auto* p = level->getPlayer(id)) valuebox::openEditor(*p, pos, boxId);
                });
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            ++mTick;
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
        net::off("valuebox.set");
        net::off("common.hover");
    }

private:
    /// Bedrock repeats use-on packets while the button is held: one action per click.
    bool debounced(std::int64_t player, core::GlobalPos const& pos) {
        auto& last = mLast[player];
        bool  skip = last.first == pos && mTick - last.second < 5;
        last       = {pos, mTick};
        return skip;
    }
    std::vector<ll::event::ListenerPtr>                                       mListeners;
    std::unordered_map<std::int64_t, std::pair<core::GlobalPos, std::uint64_t>> mLast;
    std::uint64_t                                                             mTick = 0;
};

} // namespace lv::feature::common

LV_REGISTER_MODULE(lv::feature::common::ValueBoxModule);
