// Display unit (U24): the Display Link (Create's DisplayLinkBlock / DisplayLinkBlockEntity /
// DisplayLinkBlockItem): binding to a target with the item, refresh timer, redstone control,
// the configuration screen (native + form) and the data transfer.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/display/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::display {
/// `config/display.json` (Create's CLogistics display link keys).
struct DisplayConfig {
    int version = 1;
    /// Maximum distance between a display link and its target (Create's displayLinkRange).
    int displayLinkRange = 64;
};
} // namespace lv::feature::display

LV_REGISTER_CONFIG("display", lv::feature::display::DisplayConfig);

namespace lv::feature::display::link {

namespace blockdata = common::blockdata;
namespace items     = common::items;
namespace screens   = common::screens;

namespace {
constexpr char kDbLinks[]  = "lv:display:links";
constexpr char kDataKey[]  = "display_link";
constexpr char kItemKey[]  = "display_link";
constexpr char kScreen[]   = "display.link";
constexpr char kPowered[]  = "levitate:powered";
constexpr char kFaceState[] = "minecraft:block_face";

std::set<GlobalPos>&                links() {
    static std::set<GlobalPos> s;
    return s;
}
/// Create's refreshTicks plus the cached refresh interval of the selected source (0 = unknown,
/// -1 = no source / no passive refresh).
struct Timer {
    int ticks = 0;
    int every = 0;
};
std::map<GlobalPos, Timer>& refreshTicks() {
    static std::map<GlobalPos, Timer> m;
    return m;
}

void persist() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : links()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbLinks, j);
}

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

int faceIndex(std::string const& name) {
    static constexpr char const* names[6]{"down", "up", "north", "south", "west", "east"};
    for (int i = 0; i < 6; ++i)
        if (name == names[i]) return i;
    return 1;
}

std::string blockName(BlockSource& region, GlobalPos const& pos) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b || b->isAir()) return {};
    auto      instance = b->asItemInstance(region, game::blockPos(pos), false);
    auto      tag      = instance.save(*SaveContextFactory::createCloneSaveContext());
    ItemStack stack    = tag ? ItemStack::fromTag(*tag) : ItemStack::EMPTY_ITEM();
    auto      name     = stack.isNull() ? std::string{} : stack.getName();
    return name.empty() ? b->getTypeName() : name;
}

Source const* findSource(std::vector<Source const*> const& list, std::string const& id) {
    for (auto const* s : list)
        if (s->id == id) return s;
    return nullptr;
}

/// The value of an option (default when unset).
Json optionValue(Json const& config, Option const& o) {
    auto it = config.find(o.key);
    return it == config.end() ? o.defaultValue : *it;
}

std::string localized(Player const& p, std::string const& key, std::string const& fallback) {
    return key.empty() ? fallback : trFor(p, key, fallback);
}

bool withinReach(Player& player, GlobalPos const& pos, double reach) {
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return false;
    auto const& p  = player.getPosition();
    double      dx = p.x - (pos.x + 0.5), dy = p.y - (pos.y + 0.5), dz = p.z - (pos.z + 0.5);
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] {
            links().clear();
            refreshTicks().clear();
            if (auto j = game::db::load(kDbLinks); j && j->is_array()) {
                for (auto const& e : *j)
                    if (auto p = posFrom(e)) links().insert(*p);
            }
        });
        game::db::onWorldClosed([] {
            links().clear();
            refreshTicks().clear();
        });
        blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const&) {
            if (!doc.contains(kDataKey)) return;
            links().erase(pos);
            refreshTicks().erase(pos);
            persist();
        });
    }
};
Lifecycle const gLifecycle;

} // namespace

// ---- data -------------------------------------------------------------------------------------

std::optional<LinkData> read(GlobalPos const& pos) {
    auto const* doc = blockdata::get(pos);
    if (!doc || !doc->contains(kDataKey)) return std::nullopt;
    auto const& j = (*doc)[kDataKey];
    LinkData    d;
    if (auto t = j.find("target"); t != j.end() && t->is_array() && t->size() == 3) {
        d.targetOffset = {(*t)[0].get<int>(), (*t)[1].get<int>(), (*t)[2].get<int>()};
    }
    d.line     = j.value("line", 0);
    d.sourceId = j.value("source", std::string{});
    if (auto c = j.find("config"); c != j.end() && c->is_object()) d.config = *c;
    return d;
}

void write(GlobalPos const& pos, LinkData const& d) {
    auto& doc     = blockdata::edit(pos);
    doc[kDataKey] = {
        {"target", Json::array({d.targetOffset[0], d.targetOffset[1], d.targetOffset[2]})},
        {"line",   d.line                                                                 },
        {"source", d.sourceId                                                             },
        {"config", d.config                                                               },
    };
}

std::optional<GlobalPos> sourcePos(BlockSource& region, GlobalPos const& link) {
    auto const* b = loadedBlock(region, game::blockPos(link));
    if (!b || b->getTypeName() != kLinkId) return std::nullopt;
    int face = faceIndex(strState(*b, kFaceState).value_or("up"));
    // the link faces away from the block it is attached to
    return offsetPos(link, face ^ 1);
}

GlobalPos targetPos(GlobalPos const& link, LinkData const& d) {
    return {link.dim, link.x + d.targetOffset[0], link.y + d.targetOffset[1], link.z + d.targetOffset[2]};
}

void track(GlobalPos const& pos) {
    if (links().insert(pos).second) persist();
}

std::set<GlobalPos> const& tracked() { return links(); }

// ---- transfer ---------------------------------------------------------------------------------

namespace {
/// Runs a source with its context; `fn(source, ctx, data)` and stores config changes.
template <class Fn>
bool withSource(GlobalPos const& link, Fn&& fn) {
    auto* region = regionOf(link.dim);
    if (!region) return false;
    auto data = read(link);
    if (!data) return false;
    auto src = sourcePos(*region, link);
    if (!src) return false;
    auto tgt = targetPos(link, *data);
    if (!loadedBlock(*region, game::blockPos(*src)) || !loadedBlock(*region, game::blockPos(tgt))) return false;
    auto sources = sourcesAt(*region, *src);
    auto const* active = findSource(sources, data->sourceId);
    bool changed = false;
    if (!active && !data->sourceId.empty()) {
        // Create: the selected source is no longer offered: deselect, reset the config
        data->sourceId.clear();
        data->config = Json::object();
        changed      = true;
    }
    Context ctx{*region, link, *src, tgt, data->config, gameTime()};
    bool    ok = fn(active, ctx, *data);
    if (ok || changed) write(link, *data);
    return ok;
}
} // namespace

bool update(GlobalPos const& link) {
    return withSource(link, [](Source const* source, Context& ctx, LinkData& data) {
        auto const* target = targetAt(ctx.region, ctx.target);
        if (!source || !target || !source->text) return false;
        TargetStats stats = target->stats ? target->stats(ctx.region, ctx.target) : TargetStats{};
        if (target->acceptSections && source->boardText) {
            if (auto sections = source->boardText(ctx, stats)) {
                target->acceptSections(ctx.region, ctx.target, data.line, *sections, ctx);
                return true;
            }
        }
        Lines lines = source->text(ctx, stats);
        if (lines.empty()) lines.emplace_back();
        // a list that got shorter clears the lines it wrote before
        if (source->multiLine)
            while (static_cast<int>(lines.size()) < stats.maxRows - data.line) lines.emplace_back();
        if (target->accept) target->accept(ctx.region, ctx.target, data.line, lines, ctx);
        return true;
    });
}

void selectSource(GlobalPos const& link, std::string const& sourceId) {
    refreshTicks()[link] = {};
    withSource(link, [&](Source const*, Context& ctx, LinkData& data) {
        if (data.sourceId == sourceId) return true;
        auto        sources = sourcesAt(ctx.region, ctx.source);
        auto const* s       = findSource(sources, sourceId);
        data.sourceId       = s ? sourceId : std::string{};
        data.config         = Json::object();
        if (s && s->onSelected) {
            Context fresh{ctx.region, ctx.link, ctx.source, ctx.target, data.config, ctx.gameTime};
            s->onSelected(fresh);
        }
        return true;
    });
}

void notify(GlobalPos const& source) {
    std::vector<GlobalPos> hit;
    for (auto const& l : links()) {
        if (l.dim != source.dim) continue;
        auto* region = regionOf(l.dim);
        if (!region) continue;
        if (auto s = sourcePos(*region, l); s && *s == source) hit.push_back(l);
    }
    for (auto const& l : hit) {
        auto* region = regionOf(l.dim);
        auto const* b = region ? loadedBlock(*region, game::blockPos(l)) : nullptr;
        if (b && intState(*b, kPowered).value_or(0)) continue; // powered links don't send
        refreshTicks()[l].ticks = 0;
        update(l);
    }
}

void itemsPassed(GlobalPos const& source, int count) {
    for (auto const& l : links()) {
        if (l.dim != source.dim) continue;
        auto* region = regionOf(l.dim);
        if (!region) continue;
        auto s = sourcePos(*region, l);
        if (!s || *s != source) continue;
        auto const* lb = loadedBlock(*region, game::blockPos(l));
        if (!lb || intState(*lb, kPowered).value_or(0)) continue; // a powered link ignores them
        auto data = read(l);
        if (!data || (data->sourceId != "item_throughput" && data->sourceId != "accumulate_items")) continue;
        data->config["lv:pending"] = data->config.value("lv:pending", 0) + count;
        write(l, *data);
        if (data->sourceId == "accumulate_items") {
            refreshTicks()[l].ticks = 0;
            update(l);
        }
    }
}

// ---- ticking ----------------------------------------------------------------------------------

namespace {
void onNoLongerPowered(GlobalPos const& link) {
    refreshTicks()[link] = {};
    withSource(link, [](Source const* source, Context& ctx, LinkData&) {
        if (source && source->onSignalReset) source->onSignalReset(ctx);
        return true;
    });
    update(link);
}
} // namespace

void tickAll(std::uint64_t tick) {
    std::vector<GlobalPos> gone, toUpdate, unpowered;
    for (auto const& l : links()) {
        auto* region = regionOf(l.dim);
        if (!region) continue;
        auto const* b = loadedBlock(*region, game::blockPos(l));
        if (!b) continue; // unloaded: frozen (Create's block entity doesn't tick)
        if (b->getTypeName() != kLinkId) {
            gone.push_back(l);
            continue;
        }
        bool powered = intState(*b, kPowered).value_or(0) != 0;
        // custom blocks get no neighbour updates: poll the redstone input (Create's neighborChanged)
        if ((tick + static_cast<std::uint64_t>(l.x + l.z)) % 2 == 0) {
            bool should = signalAt(*region, game::blockPos(l)) > 0;
            if (should != powered) {
                if (auto const* next = withStates(*b, {{kPowered, should ? 1 : 0}})) game::setBlock(*region, game::blockPos(l), *next);
                powered = should;
                if (!should) unpowered.push_back(l);
            }
        }
        // Create's DisplayLinkBlockEntity.tick
        auto& timer = refreshTicks()[l];
        if (timer.every == 0) {
            timer.every = -1;
            auto data   = read(l);
            auto src    = sourcePos(*region, l);
            if (data && src && !data->sourceId.empty()) {
                auto        sources = sourcesAt(*region, *src);
                auto const* active  = findSource(sources, data->sourceId);
                if (active && active->passiveRefresh) timer.every = std::max(1, active->refreshTicks);
            }
        }
        if (timer.every < 0) continue;
        if (++timer.ticks < timer.every) continue;
        timer.ticks = 0;
        timer.every = 0; // re-read: the source block may have changed
        if (powered) continue; // tickSource: a powered link resets its timer and sends nothing
        toUpdate.push_back(l);
    }
    for (auto const& l : gone) {
        links().erase(l);
        refreshTicks().erase(l);
    }
    if (!gone.empty()) persist();
    for (auto const& l : unpowered) onNoLongerPowered(l);
    for (auto const& l : toUpdate) update(l);
}

// ---- item -------------------------------------------------------------------------------------

namespace {
struct PendingPlacement {
    GlobalPos placed;
    GlobalPos target;
};
std::map<std::int64_t, PendingPlacement>& pending() {
    static std::map<std::int64_t, PendingPlacement> m;
    return m;
}
} // namespace

bool useItem(Player& player, BlockSource& region, BlockPos const& clicked, int face) {
    auto const& held = player.getSelectedItem();
    auto        sel  = items::getData(held, kItemKey);
    auto        selected = sel.is_object() ? posFrom(sel.value("pos", Json())) : std::nullopt;
    if (player.isSneaking() && selected) {
        ItemStack copy = held;
        items::setData(copy, kItemKey, nullptr);
        player.setSelectedItem(copy);
        player.sendInventory(false);
        actionBar(player, trFor(player, "create.display_link.clear", "Cleared position selection"));
        return true;
    }
    if (!selected) {
        ItemStack copy = held;
        items::setData(copy, kItemKey, {{"pos", posJson(game::globalPos(region, clicked))}});
        player.setSelectedItem(copy);
        player.sendInventory(false);
        actionBar(player, trFor(player, "create.display_link.set", "Targeted position selected"));
        log().info("display link target selected at {} by {}", game::globalPos(region, clicked).key(), player.getRealName());
        return true;
    }
    BlockPos placedPos = clicked;
    if (!game::replaceable(region, clicked) && face >= 0 && face < 6) {
        placedPos = BlockPos{clicked.x + kDx[face], clicked.y + kDy[face], clicked.z + kDz[face]};
    }
    auto   placed = game::globalPos(region, placedPos);
    int    range  = config::get<DisplayConfig>().displayLinkRange;
    double dx = selected->x - placed.x, dy = selected->y - placed.y, dz = selected->z - placed.z;
    if (selected->dim != placed.dim || dx * dx + dy * dy + dz * dz >= double(range) * range) {
        actionBar(player, "§c" + trFor(player, "create.display_link.too_far", "Targeted position is too far from here"));
        player.sendInventory(false);
        return true;
    }
    pending()[player.getOrCreateUniqueID().rawID] = {placed, *selected};
    return false;
}

void placed(Player& player, GlobalPos const& linkPos) {
    auto it = pending().find(player.getOrCreateUniqueID().rawID);
    if (it == pending().end() || it->second.placed != linkPos) return;
    auto target = it->second.target;
    pending().erase(it);
    LinkData d;
    d.targetOffset = {target.x - linkPos.x, target.y - linkPos.y, target.z - linkPos.z};
    write(linkPos, d);
    track(linkPos);
    // the rest of the stack forgets the selection (Create removes it from the item in hand)
    auto const& held = player.getSelectedItem();
    if (!held.isNull() && held.getTypeName() == kLinkId) {
        ItemStack copy = held;
        items::setData(copy, kItemKey, nullptr);
        player.setSelectedItem(copy);
    }
    player.sendInventory(false);
    actionBar(player, "§a" + trFor(player, "create.display_link.success", "Successfully bound to targeted position"));
    log().info("display link placed at {} targeting {}", linkPos.key(), target.key());
}

// ---- screen -----------------------------------------------------------------------------------

namespace {
Json screenData(Player& player, GlobalPos const& link) {
    auto* region = regionOf(link.dim);
    auto  data   = read(link);
    if (!region || !data) return nullptr;
    auto src = sourcePos(*region, link);
    if (!src) return nullptr;
    auto tgt     = targetPos(link, *data);
    auto sources = sourcesAt(*region, *src);
    auto const* target = targetAt(*region, tgt);
    Json        list   = Json::array();
    for (auto const* s : sources) {
        Json opts = Json::array();
        for (auto const& o : s->options) {
            Json choices = Json::array();
            for (std::size_t i = 0; i < o.choices.size(); ++i) {
                choices.push_back(localized(player, o.choices[i], i < o.choiceFallbacks.size() ? o.choiceFallbacks[i] : o.choices[i]));
            }
            opts.push_back({
                {"key",     o.key                                       },
                {"label",   localized(player, o.label, o.labelFallback)},
                {"choices", choices                                     },
                {"text",    o.choices.empty()                           },
                {"value",   optionValue(data->config, o)                },
            });
        }
        list.push_back({
            {"id",      s->id                                         },
            {"name",    localized(player, s->name, s->nameFallback)},
            {"multi",   s->multiLine                                  },
            {"options", opts                                          },
        });
    }
    Json lineNames = Json::array();
    if (target) {
        auto stats = target->stats ? target->stats(*region, tgt) : TargetStats{};
        int  rows  = std::max(1, stats.maxRows);
        for (int i = 0; i < rows; ++i) {
            if (target->lineName) lineNames.push_back(target->lineName(i, player));
            else if (rows == 1) lineNames.push_back(trFor(player, "create.display_target.single_line", "Single Line"));
            else lineNames.push_back(trFor(player, "create.display_target.line", "Line {}", i + 1));
        }
    }
    return {
        {"pos",        posJson(link)                },
        {"sourceName", blockName(*region, *src)     },
        {"targetName", blockName(*region, tgt)      },
        {"sources",    list                         },
        {"selected",   data->sourceId               },
        {"line",       data->line                   },
        {"lines",      lineNames                    },
        {"noSource",   sources.empty()              },
        {"noTarget",   target == nullptr            },
    };
}

void applyAction(Player& player, GlobalPos const& link, Json const& action) {
    auto op = action.value("op", std::string{});
    if (op == "source") {
        selectSource(link, action.value("id", std::string{}));
        log().info("display link at {} source set to '{}' by {}", link.key(), action.value("id", std::string{}), player.getRealName());
    } else if (op == "line") {
        auto d = read(link);
        if (!d) return;
        d->line = std::max(0, action.value("value", 0));
        write(link, *d);
        log().info("display link at {} line set to {} by {}", link.key(), d->line, player.getRealName());
    } else if (op == "form") {
        // text options are edited in the form (the native screen has no text field)
        screens::close(player, kScreen);
        defer([id = player.getOrCreateUniqueID().rawID, link] {
            if (auto* p = playerById(id)) screens::openForm(*p, kScreen, {{"pos", posJson(link)}});
        });
        return;
    } else if (op == "option") {
        auto d = read(link);
        if (!d) return;
        auto key = action.value("key", std::string{});
        if (key.empty() || key.starts_with("lv:")) return;
        d->config[key] = action.value("value", Json());
        write(link, *d);
        log().info("display link at {} option {} set by {}", link.key(), key, player.getRealName());
    } else {
        return;
    }
    refreshTicks()[link] = {};
    // Create sends the new configuration and updates the target at once
    auto* region = regionOf(link.dim);
    auto const* b = region ? loadedBlock(*region, game::blockPos(link)) : nullptr;
    if (b && !intState(*b, kPowered).value_or(0)) update(link);
}

void openForm(Player& player, GlobalPos const& link) {
    auto data = screenData(player, link);
    if (data.is_null()) return;
    ll::form::CustomForm form(trFor(player, "create.display_link.title", "Display Link"));
    form.appendLabel(trFor(player, "create.display_link.reading_from", "Read from:") + " " + data.value("sourceName", std::string{}));
    form.appendLabel(trFor(player, "create.display_link.writing_to", "Send to:") + " " + data.value("targetName", std::string{}));
    auto const& sources  = data["sources"];
    auto        selected = data.value("selected", std::string{});
    std::vector<std::string> names{"-"};
    std::size_t              current = 0;
    Json const*              active  = nullptr;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        names.push_back(sources[i].value("name", std::string{}));
        if (sources[i].value("id", std::string{}) == selected) {
            current = i + 1;
            active  = &sources[i];
        }
    }
    if (sources.empty()) form.appendLabel("§c" + trFor(player, "create.display_link.no_source", "Not a Display Source"));
    else form.appendDropdown("source", trFor(player, "create.display_link.information_type", "Type of Information"), names, current);
    auto lines = data["lines"].get<std::vector<std::string>>();
    if (lines.empty()) form.appendLabel("§c" + trFor(player, "create.display_link.no_target", "Not a Display Target"));
    else {
        bool multi = active && active->value("multi", false);
        auto label = multi ? trFor(player, "create.display_link.display_on_multiline", "Start writing at:")
                           : trFor(player, "create.display_link.display_on", "Write data to:");
        form.appendDropdown("line", label, lines, static_cast<std::size_t>(std::clamp(data.value("line", 0), 0, int(lines.size()) - 1)));
    }
    if (active) {
        for (auto const& o : (*active)["options"]) {
            auto key = "opt:" + o.value("key", std::string{});
            if (o.value("text", false)) {
                form.appendInput(key, o.value("label", std::string{}), "", o["value"].is_string() ? o["value"].get<std::string>() : std::string{});
            } else {
                auto choices = o["choices"].get<std::vector<std::string>>();
                if (choices.empty()) continue;
                int v = o["value"].is_number_integer() ? o["value"].get<int>() : 0;
                form.appendDropdown(key, o.value("label", std::string{}), choices, static_cast<std::size_t>(std::clamp(v, 0, int(choices.size()) - 1)));
            }
        }
    }
    auto sourceIds = Json::array();
    for (auto const& s : sources) sourceIds.push_back(s.value("id", std::string{}));
    form.sendTo(player, [link, sourceIds, selected](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result || !withinReach(p, link, 16)) return;
        std::string newSource = selected;
        if (auto it = result->find("source"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) newSource = (*v == 0 || *v > sourceIds.size()) ? std::string{} : sourceIds[static_cast<std::size_t>(*v - 1)].get<std::string>();
        }
        if (newSource != selected) {
            applyAction(p, link, {{"op", "source"}, {"id", newSource}});
        } else {
            for (auto const& [key, value] : *result) {
                if (!key.starts_with("opt:")) continue;
                Json v;
                if (auto const* u = std::get_if<uint64>(&value)) v = static_cast<int>(*u);
                else if (auto const* s = std::get_if<std::string>(&value)) v = *s;
                else continue;
                applyAction(p, link, {{"op", "option"}, {"key", key.substr(4)}, {"value", v}});
            }
        }
        if (auto it = result->find("line"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) applyAction(p, link, {{"op", "line"}, {"value", static_cast<int>(*v)}});
        }
        // the options of a newly chosen source are set in a second form (Create shows them at once)
        if (newSource != selected && !newSource.empty()) {
            auto* region = regionOf(link.dim);
            auto  src    = region ? sourcePos(*region, link) : std::nullopt;
            if (src) {
                auto        list = sourcesAt(*region, *src);
                auto const* s    = findSource(list, newSource);
                if (s && !s->options.empty()) defer([id = p.getOrCreateUniqueID().rawID, link] {
                        if (auto* again = playerById(id)) screens::openForm(*again, kScreen, {{"pos", posJson(link)}});
                    });
            }
        }
    });
}

struct ScreenRegistration {
    ScreenRegistration() {
        screens::registerType({
            .id   = kScreen,
            .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
                auto pos = posFrom(ctx.value("pos", Json()));
                if (!pos || !withinReach(p, *pos, 16)) return std::nullopt;
                auto d = screenData(p, *pos);
                if (d.is_null()) return std::nullopt;
                return d;
            },
            .action = [](Player& p, Json const& ctx, Json const& action) {
                if (auto pos = posFrom(ctx.value("pos", Json()))) applyAction(p, *pos, action);
            },
            .form = [](Player& p, Json const& ctx) {
                if (auto pos = posFrom(ctx.value("pos", Json()))) openForm(p, *pos);
            },
        });
    }
};
ScreenRegistration const gScreen;
} // namespace

void openScreen(Player& player, GlobalPos const& link) {
    auto data = read(link);
    auto* region = regionOf(link.dim);
    bool  valid  = data && region && (data->targetOffset != std::array<int, 3>{0, 0, 0});
    if (valid) {
        auto tgt = targetPos(link, *data);
        auto const* b = loadedBlock(*region, game::blockPos(tgt));
        valid = b != nullptr;
    }
    if (!valid) {
        actionBar(player, "§c" + trFor(player, "create.display_link.invalid", "Link has no valid target, try placing it again"));
        return;
    }
    track(link);
    screens::open(player, kScreen, {{"pos", posJson(link)}});
}

} // namespace lv::feature::display::link
