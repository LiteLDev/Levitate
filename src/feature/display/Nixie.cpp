// Display unit (U24): Nixie Tubes (Create's NixieTubeBlock / NixieTubeBlockEntity /
// NixieTubeDisplayTarget / NixieTubeDisplaySource): two characters per tube, rows of tubes share
// one text, redstone strength when no text is set, dyed variants (16 colours, the orange one is
// `nixie_tube`), text from clipboards, name tags and display links.

#include <algorithm>
#include <cmath>
#include <map>

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/display/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::display::nixie {

namespace blockdata = common::blockdata;
namespace items     = common::items;

namespace {
constexpr char kDbNixies[] = "lv:display:nixies";
constexpr char kDataKey[]  = "nixie";
constexpr char kFacing[]   = "levitate:facing";
constexpr char kFace[]     = "levitate:face"; ///< 0 floor, 1 ceiling, 2 wall, 3 wall_reversed
constexpr int  kMaxRow     = 64;

std::set<GlobalPos>& tubes() {
    static std::set<GlobalPos> s;
    return s;
}
std::map<GlobalPos, Json>& lastSent() {
    static std::map<GlobalPos, Json> m;
    return m;
}

void persist() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : tubes()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbNixies, j);
}

struct TubeBlock {
    int facing = 0, face = 0, colour = kOrange;
};

std::optional<TubeBlock> tubeAt(BlockSource& region, GlobalPos const& p) {
    auto const* b = loadedBlock(region, game::blockPos(p));
    if (!b || !isNixie(b->getTypeName())) return std::nullopt;
    return TubeBlock{intState(*b, kFacing).value_or(0) & 3, std::clamp(intState(*b, kFace).value_or(0), 0, 3), colourOf(b->getTypeName())};
}

/// Create's getLeftNixieDirection (a FacingID).
int leftOf(TubeBlock const& t) {
    if (t.face == 2) return 1; // WALL: up
    if (t.face == 3) return 0; // WALL_REVERSED: down
    return faceOfHorizontal(game::opposite(t.facing));
}

struct TubeData {
    std::optional<std::string> text;
    int                        index  = 0;
    int                        signal = 0;
};

TubeData readTube(GlobalPos const& p) {
    TubeData    d;
    auto const* doc = blockdata::get(p);
    if (!doc || !doc->contains(kDataKey)) return d;
    auto const& j = (*doc)[kDataKey];
    if (auto t = j.find("text"); t != j.end() && t->is_string()) d.text = t->get<std::string>();
    d.index  = j.value("index", 0);
    d.signal = j.value("signal", 0);
    return d;
}

void writeTube(GlobalPos const& p, TubeData const& d) {
    Json j = {{"index", d.index}, {"signal", d.signal}};
    if (d.text) j["text"] = *d.text;
    blockdata::edit(p)[kDataKey] = std::move(j);
}

std::string charOrSpace(std::vector<std::string> const& g, int i) {
    return i >= 0 && i < static_cast<int>(g.size()) ? g[static_cast<std::size_t>(i)] : std::string(" ");
}

std::string shown(TubeData const& d) {
    if (d.text) {
        auto g = utf8Glyphs(*d.text);
        return charOrSpace(g, d.index * 2) + charOrSpace(g, d.index * 2 + 1);
    }
    // Create: "0"/"1" for the tens, the ones digit
    return std::string(d.signal < 10 ? "0" : "1") + std::to_string(d.signal % 10);
}

std::array<double, 3> textPos(GlobalPos const& p, TubeBlock const& t) {
    double y = p.y + (t.face == 1 ? 0.3 : t.face >= 2 ? 0.35 : 0.65);
    double x = p.x + 0.5, z = p.z + 0.5;
    if (t.face >= 2) {
        int f = faceOfHorizontal(t.facing);
        x += kDx[f] * 0.3;
        z += kDz[f] * 0.3;
    }
    return {x, y, z};
}

void sync(BlockSource& region, GlobalPos const& p) {
    auto t = tubeAt(region, p);
    if (!t) return;
    auto chars = shown(readTube(p));
    Json state = {
        {"op",     "set"                                },
        {"dim",    p.dim                                },
        {"pos",    Json::array({p.x, p.y, p.z})         },
        {"facing", t->facing                            },
        {"face",   t->face                              },
        {"colour", dyeRgb(t->colour)                    },
        {"chars",  chars                                },
    };
    auto& last = lastSent()[p];
    if (last == state) return;
    last = state;
    broadcast(p.dim, "display.nixie", state);
    bool blank = chars.find_first_not_of(' ') == std::string::npos;
    if (blank) removeFloatingText("nixie:" + p.key());
    else setFloatingText("nixie:" + p.key(), p.dim, textPos(p, *t), dyeFormatCode(t->colour) + "§l" + chars);
}

void forget(GlobalPos const& p) {
    tubes().erase(p);
    if (lastSent().erase(p)) broadcast(p.dim, "display.nixie", {{"op", "remove"}, {"dim", p.dim}, {"pos", Json::array({p.x, p.y, p.z})}});
    removeFloatingText("nixie:" + p.key());
    persist();
}

struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] {
            tubes().clear();
            lastSent().clear();
            if (auto j = game::db::load(kDbNixies); j && j->is_array()) {
                for (auto const& e : *j)
                    if (e.is_array() && e.size() == 4) tubes().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
            }
        });
        game::db::onWorldClosed([] {
            tubes().clear();
            lastSent().clear();
        });
    }
};
Lifecycle const gLifecycle;

} // namespace

bool isNixie(std::string_view id) {
    if (id == kNixieId) return true;
    if (!id.starts_with("levitate:") || !id.ends_with("_nixie_tube")) return false;
    auto colour = id.substr(9, id.size() - 9 - 11);
    for (int i = 0; i < 16; ++i)
        if (i != kOrange && colour == kDyeNames[static_cast<std::size_t>(i)]) return true;
    return false;
}

int colourOf(std::string_view id) {
    if (id == kNixieId) return kOrange;
    for (int i = 0; i < 16; ++i)
        if (id == idFor(i)) return i;
    return kOrange;
}

std::string idFor(int dye) {
    if (dye == kOrange) return kNixieId;
    return std::string("levitate:") + kDyeNames[static_cast<std::size_t>(std::clamp(dye, 0, 15))] + "_nixie_tube";
}

std::vector<GlobalPos> row(BlockSource& region, GlobalPos const& pos) {
    std::vector<GlobalPos> out;
    auto                   t = tubeAt(region, pos);
    if (!t) return out;
    auto same = [&](GlobalPos const& q) {
        auto o = tubeAt(region, q);
        return o && o->facing == t->facing && o->face == t->face; // Create: equal after normalising the colour
    };
    int       left  = leftOf(*t);
    GlobalPos start = pos;
    for (int i = 0; i < kMaxRow && same(offsetPos(start, left)); ++i) start = offsetPos(start, left);
    for (int i = 0; i < kMaxRow * 2; ++i) {
        auto q = offsetPos(start, left ^ 1, i);
        if (!same(q)) break;
        out.push_back(q);
    }
    return out;
}

std::string displayed(BlockSource& region, GlobalPos const& pos) {
    (void)region;
    return shown(readTube(pos));
}

std::string fullText(BlockSource& region, GlobalPos const& pos) {
    (void)region;
    auto d = readTube(pos);
    return d.text ? *d.text : std::to_string(d.signal);
}

void track(GlobalPos const& pos) {
    if (tubes().insert(pos).second) persist();
}

void displayCustomText(BlockSource& region, GlobalPos const& pos, std::string const& text) {
    auto r = row(region, pos);
    for (int i = 0; i < static_cast<int>(r.size()); ++i) {
        auto const& p = r[static_cast<std::size_t>(i)];
        auto        d = readTube(p);
        if (d.text && *d.text == text && d.index == i) continue;
        d.text  = text;
        d.index = i;
        writeTube(p, d);
        track(p);
        sync(region, p);
        notifyGatherers(p);
    }
}

void clearCustomText(BlockSource& region, GlobalPos const& pos) {
    auto d = readTube(pos);
    d.text.reset();
    d.index = 0;
    writeTube(pos, d);
    updateRedstone(region, pos, true);
}

void updateRedstone(BlockSource& region, GlobalPos const& pos, bool force) {
    auto t = tubeAt(region, pos);
    if (!t) return;
    auto d = readTube(pos);
    if (d.text && !force) return; // reactsToRedstone: only without custom text
    int power = signalAt(region, game::blockPos(pos));
    if (power == d.signal && !force) return;
    d.signal = power;
    d.text.reset();
    writeTube(pos, d);
    sync(region, pos);
    notifyGatherers(pos);
}

bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held) {
    if (player.isSneaking()) return false;
    auto t = tubeAt(region, pos);
    if (!t) return false;
    auto id = items::idOf(held);
    if (id.empty()) {
        // Create's NixieTubeBlock.useItemOn: an empty hand clears the clicked tube (not one that
        // shows redstone)
        if (!readTube(pos).text) return false;
        clearCustomText(region, pos);
        log().info("nixie tube at {} cleared by {}", pos.key(), player.getRealName());
        return true;
    }
    if (id == "minecraft:name_tag") {
        auto name = held.getCustomName();
        if (name.empty()) return false;
        displayCustomText(region, pos, name);
        log().info("nixie tube at {} shows '{}' (name tag)", pos.key(), name);
        return true;
    }
    if (id == kClipboardId) {
        auto entries = clipboard::lastViewedEntries(clipboard::ofItem(held));
        if (entries.empty()) return false;
        displayCustomText(region, pos, entries.front().text);
        log().info("nixie tube at {} shows '{}' (clipboard)", pos.key(), entries.front().text);
        return true;
    }
    if (auto dye = dyeOf(id)) {
        // Create's walkNixies: the whole row changes colour
        for (auto const& p : row(region, pos)) {
            auto const& block = region.getBlock(game::blockPos(p));
            auto const* fresh = game::blockWith(idFor(*dye), {{kFacing, t->facing}, {kFace, t->face}});
            if (!fresh || fresh == &block) continue;
            auto doc = blockdata::take(p); // keep the text across the block change
            game::setBlock(region, game::blockPos(p), *fresh);
            if (doc) blockdata::put(p, *doc);
            lastSent().erase(p);
            sync(region, p);
        }
        playSound(region, pos, "dye.use");
        log().info("nixie tubes at {} dyed {}", pos.key(), kDyeNames[static_cast<std::size_t>(*dye)]);
        return true;
    }
    return false;
}

void placed(Player& player, BlockSource& region, GlobalPos const& pos, int clickedFace) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b || !isNixie(b->getTypeName())) return;
    int face = 0, facing = 0;
    if (clickedFace == 1) {
        face = 0; // floor
    } else if (clickedFace == 0) {
        face = 1; // ceiling
    } else {
        // Create's DoubleFaceAttachedBlock: on a wall, reversed when the player looks against the
        // clockwise side of the direction it looks at the wall
        static constexpr double cwX[6]{0, 0, 1, -1, 0, 0}, cwZ[6]{0, 0, 0, 0, -1, 1}; // FacingID -> clockwise normal
        int  looking = clickedFace ^ 1;                                                  // opposite of the clicked face
        auto look    = player.getViewVector();
        face         = look.x * cwX[std::clamp(looking, 0, 5)] + look.z * cwZ[std::clamp(looking, 0, 5)] < 0 ? 3 : 2;
    }
    if (face < 2) {
        facing = clockwise(game::playerFacing(player)); // Create rotates FACING clockwise for floor / ceiling
    } else {
        static constexpr int toFacing[6]{0, 0, 2, 0, 1, 3}; // FacingID -> Java 2D facing
        facing = toFacing[std::clamp(clickedFace, 0, 5)];
    }
    if (auto const* next = withStates(*b, {{kFacing, facing}, {kFace, face}})) game::setBlock(region, game::blockPos(pos), *next);
    track(pos);
    // Create's onPlace: a new tube in a row with text shows its part of it
    auto r = row(region, pos);
    std::optional<std::string> text;
    for (auto const& p : r)
        if (p != pos)
            if (auto d = readTube(p); d.text) text = d.text;
    if (text) displayCustomText(region, pos, *text);
    else updateRedstone(region, pos, true);
    log().info("nixie tube placed at {}", pos.key());
}

void removed(GlobalPos const& pos) { forget(pos); }

void syncAll(Player& player) {
    int dim = static_cast<int>(player.getDimensionId());
    for (auto const& [p, st] : lastSent())
        if (p.dim == dim) net::send(player, "display.nixie", st);
}

void tickAll(std::uint64_t tick) {
    if (tick % 2 != 0) return; // Create schedules the redstone update one tick after a neighbour change
    std::vector<GlobalPos> gone;
    auto                   snapshot = tubes();
    for (auto const& p : snapshot) {
        auto* region = regionOf(p.dim);
        if (!region || !loadedBlock(*region, game::blockPos(p))) continue;
        if (!tubeAt(*region, p)) {
            gone.push_back(p);
            continue;
        }
        updateRedstone(*region, p);
        if (tick % 20 == 0 || !lastSent().contains(p)) sync(*region, p);
    }
    for (auto const& p : gone) forget(p);
}

// ---- display target / source ------------------------------------------------------------------

namespace {
struct Registration {
    Registration() {
        auto target = Target{
            .id    = "nixie_tube",
            .stats = [](BlockSource& region, GlobalPos const& t) {
                return TargetStats{1, static_cast<int>(row(region, t).size()) * 2, "nixie_tube"};
            },
            .accept = [](BlockSource& region, GlobalPos const& t, int, Lines const& lines, Context&) {
                displayCustomText(region, t, lines.empty() ? std::string{} : lines.front());
            },
            .acceptSections = nullptr,
            .lineName = [](int, Player const& p) { return trFor(p, "create.display_target.single_line", "Single Line"); },
        };
        auto source = Source{
            .id           = "nixie_tube",
            .name         = "create.display_source.nixie_tube",
            .nameFallback = "Copy Nixie Tubes",
            .text = [](Context& ctx, TargetStats const&) -> Lines { return {fullText(ctx.region, ctx.source)}; },
        };
        for (int i = 0; i < 16; ++i) {
            registerTarget(idFor(i), target);
            registerSource(idFor(i), source);
        }
    }
};
Registration const gRegistration;
} // namespace

} // namespace lv::feature::display::nixie
