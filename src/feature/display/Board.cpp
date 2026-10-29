// Display unit (U24): the Display Board (Create's FlapDisplayBlock / FlapDisplayBlockEntity /
// DisplayBoardTarget): a kinetic multiblock of split-flap lines (two per block row), dyeable and
// glowing per line, text from clipboards, name tags and display links. Players with the client
// mod see the flaps animate (src/client/display/); vanilla players see the text as a name tag.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/display/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::display::board {

namespace blockdata = common::blockdata;
namespace items     = common::items;
namespace k         = lv::kinetics;

namespace {
constexpr char kDbBoards[] = "lv:display:boards";
constexpr char kDataKey[]  = "display_board";
constexpr char kFacing[]   = "levitate:facing";
constexpr char kUp[]       = "levitate:up";
constexpr char kDown[]     = "levitate:down";
constexpr int  kMaxSize    = 32;
constexpr unsigned kDefaultColour = 0xD3C6BA; // Create's getLineColor default

std::set<GlobalPos>& controllers() {
    static std::set<GlobalPos> s;
    return s;
}
std::map<GlobalPos, Json>& lastSent() {
    static std::map<GlobalPos, Json> m;
    return m;
}
std::map<std::int64_t, int>& playerDims() {
    static std::map<std::int64_t, int> m;
    return m;
}

void persist() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& p : controllers()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbBoards, j);
}

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }

struct BoardBlock {
    int  facing = 0;
    bool up = false, down = false;
};

std::optional<BoardBlock> boardAt(BlockSource& region, GlobalPos const& p) {
    auto const* b = loadedBlock(region, game::blockPos(p));
    if (!b || b->getTypeName() != kBoardId) return std::nullopt;
    return BoardBlock{intState(*b, kFacing).value_or(0) & 3, intState(*b, kUp).value_or(0) != 0, intState(*b, kDown).value_or(0) != 0};
}

GlobalPos step(GlobalPos p, int facing2d, int n = 1) { return offsetPos(p, faceOfHorizontal(facing2d), n); }

/// The row of boards with the same facing through `p` (left to right as seen from the front).
std::vector<GlobalPos> rowOf(BlockSource& region, GlobalPos const& p, int facing) {
    std::vector<GlobalPos> out;
    auto                   b = boardAt(region, p);
    if (!b || b->facing != facing) return out;
    GlobalPos left = p;
    for (int i = 0; i < kMaxSize; ++i) {
        auto n  = step(left, clockwise(facing));
        auto nb = boardAt(region, n);
        if (!nb || nb->facing != facing) break;
        left = n;
    }
    for (int i = 0; i < kMaxSize * 2; ++i) {
        auto n  = step(left, counterClockwise(facing), i);
        auto nb = boardAt(region, n);
        if (!nb || nb->facing != facing) break;
        out.push_back(n);
    }
    return out;
}

bool rowMatches(BlockSource& region, std::vector<GlobalPos> const& row, int dy, int facing) {
    if (row.empty()) return false;
    GlobalPos first = row.front();
    first.y += dy;
    auto other = rowOf(region, first, facing);
    if (other.size() != row.size()) return false;
    for (std::size_t i = 0; i < row.size(); ++i)
        if (other[i].x != row[i].x || other[i].z != row[i].z) return false;
    return true;
}

void fixRow(BlockSource& region, GlobalPos const& p) {
    auto b = boardAt(region, p);
    if (!b) return;
    auto row  = rowOf(region, p, b->facing);
    bool up   = rowMatches(region, row, 1, b->facing);
    bool down = rowMatches(region, row, -1, b->facing);
    for (auto const& q : row) {
        auto qb = boardAt(region, q);
        if (!qb || (qb->up == up && qb->down == down)) continue;
        auto const& block = region.getBlock(game::blockPos(q));
        if (auto const* next = withStates(block, {{kUp, up ? 1 : 0}, {kDown, down ? 1 : 0}})) game::setBlock(region, game::blockPos(q), *next);
    }
}

// ---- stored lines -----------------------------------------------------------------------------

struct Line {
    std::string text;
    bool        manual = false;
    int         dye    = -1; ///< -1 = default colour (white dye resets)
    bool        glow   = false;
    std::string owner; ///< the link that reserved the line
};

struct Lines_ {
    int               w = 1, h = 1;
    std::vector<Line> lines;
};

Lines_ readLines(GlobalPos const& c) {
    Lines_      out;
    auto const* doc = blockdata::get(c);
    if (!doc || !doc->contains(kDataKey)) return out;
    auto const& j = (*doc)[kDataKey];
    out.w         = j.value("w", 1);
    out.h         = j.value("h", 1);
    for (auto const& l : j.value("lines", Json::array())) {
        out.lines.push_back({l.value("t", std::string{}), l.value("m", false), l.value("c", -1), l.value("g", false), l.value("o", std::string{})});
    }
    return out;
}

void writeLines(GlobalPos const& c, Lines_ const& d) {
    Json lines = Json::array();
    for (auto const& l : d.lines) {
        Json e = {{"t", l.text}};
        if (l.manual) e["m"] = true;
        if (l.dye >= 0) e["c"] = l.dye;
        if (l.glow) e["g"] = true;
        if (!l.owner.empty()) e["o"] = l.owner;
        lines.push_back(std::move(e));
    }
    blockdata::edit(c)[kDataKey] = {{"w", d.w}, {"h", d.h}, {"lines", lines}};
}

/// Lines of the board at controller `c`, resized to its structure (Create's updateControllerStatus:
/// a size change keeps the colours and glow, resets the text).
Lines_ linesFor(Structure const& s) {
    auto d = readLines(s.controller);
    int  n = s.height * 2;
    if (d.w != s.width || d.h != s.height || static_cast<int>(d.lines.size()) != n) {
        d.lines.resize(static_cast<std::size_t>(n));
        for (auto& l : d.lines) {
            l.text.clear();
            l.manual = false;
            l.owner.clear();
        }
        d.w = s.width;
        d.h = s.height;
    }
    return d;
}

std::string composeSections(Lines const& sections, int width) {
    if (sections.empty()) return {};
    if (sections.size() == 1) return sections[0];
    // value lists: the value right-aligned in a column before the name (Create's flap layout)
    std::string out;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i) out += " ";
        out += sections[i];
    }
    return truncateGlyphs(out, width);
}

// ---- sync -------------------------------------------------------------------------------------

std::array<double, 3> frontCentre(Structure const& s) {
    auto c = s.controller;
    int  f = faceOfHorizontal(s.facing), r = faceOfHorizontal(counterClockwise(s.facing));
    double x = c.x + 0.5 + kDx[f] * 0.6 + kDx[r] * (s.width - 1) / 2.0;
    double y = c.y + 0.5 - (s.height - 1) / 2.0 - 0.3;
    double z = c.z + 0.5 + kDz[f] * 0.6 + kDz[r] * (s.width - 1) / 2.0;
    return {x, y, z};
}

Json renderState(BlockSource& region, Structure const& s) {
    auto  d     = linesFor(s);
    float speed = k::speedAt(s.controller);
    Json  lines = Json::array();
    for (auto const& l : d.lines) {
        lines.push_back({{"t", l.text}, {"c", l.dye < 0 ? kDefaultColour : dyeRgb(l.dye)}, {"g", l.glow}});
    }
    (void)region;
    return {
        {"op",      "set"                                          },
        {"dim",     s.controller.dim                               },
        {"pos",     posJson(s.controller)                          },
        {"facing",  s.facing                                       },
        {"w",       s.width                                        },
        {"h",       s.height                                       },
        {"running", running(s.controller)                          },
        {"instant", std::fabs(speed) > 128                         },
        {"lines",   lines                                          },
    };
}

std::string vanillaText(Json const& state) {
    std::string out;
    bool        any = false;
    for (auto const& l : state["lines"]) {
        if (!out.empty() || any) out += "\n";
        auto t = l.value("t", std::string{});
        any |= !t.empty();
        unsigned c   = l.value("c", kDefaultColour);
        int      dye = -1;
        for (int i = 0; i < 16; ++i)
            if (dyeRgb(i) == c) dye = i;
        out += (dye < 0 ? std::string("§f") : dyeFormatCode(dye)) + t;
    }
    return any ? out : std::string{};
}

void sendState(GlobalPos const& c, Json const& state) {
    auto& last = lastSent()[c];
    if (last == state) return;
    last = state;
    broadcast(c.dim, "display.board", state);
    // vanilla fallback: the text of a turning board as a name tag (the flaps keep their text while stopped)
    if (state.value("running", false)) {
        Structure s{c, state.value("facing", 0), state.value("w", 1), state.value("h", 1)};
        setFloatingText("board:" + c.key(), c.dim, frontCentre(s), vanillaText(state));
    }
}

void forget(GlobalPos const& c) {
    controllers().erase(c);
    if (lastSent().erase(c)) broadcast(c.dim, "display.board", {{"op", "remove"}, {"dim", c.dim}, {"pos", posJson(c)}});
    removeFloatingText("board:" + c.key());
    persist();
}

void refreshController(BlockSource& region, GlobalPos const& c) {
    // rows placed or removed without a player (commands, structures, pistons) settle here
    fixRow(region, c);
    auto s = structureOf(region, c);
    if (s && s->controller == c) {
        // a block of the rectangle went without a player breaking it: settle the rows again
        bool whole = true;
        for (int x = 0; x < s->width && whole; ++x)
            for (int y = 0; y < s->height && whole; ++y) {
                auto q = step(c, counterClockwise(s->facing), x);
                q.y -= y;
                auto b = boardAt(region, q);
                whole  = b && b->facing == s->facing;
            }
        if (!whole) {
            for (int x = 0; x < s->width; ++x)
                for (int y = 0; y < s->height; ++y) {
                    auto q = step(c, counterClockwise(s->facing), x);
                    q.y -= y;
                    fixRow(region, q);
                }
            s = structureOf(region, c);
        }
    }
    if (!s || s->controller != c) {
        forget(c);
        if (s && controllers().insert(s->controller).second) persist();
        return;
    }
    auto d = linesFor(*s);
    auto stored = readLines(c);
    if (stored.w != d.w || stored.h != d.h || stored.lines.size() != d.lines.size()) writeLines(c, d);
    sendState(c, renderState(region, *s));
}

struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] {
            controllers().clear();
            lastSent().clear();
            playerDims().clear();
            if (auto j = game::db::load(kDbBoards); j && j->is_array()) {
                for (auto const& e : *j)
                    if (e.is_array() && e.size() == 4) controllers().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
            }
        });
        game::db::onWorldClosed([] {
            controllers().clear();
            lastSent().clear();
            playerDims().clear();
        });
        net::onCompanionJoined([](Player& p) { syncAll(p); });
    }
};
Lifecycle const gLifecycle;

// ---- kinetics: Create's FlapDisplayBlock is a horizontal kinetic block and a small cog (ICogWheel)
// turning about its facing axis; it has no stress impact (CStress.setNoImpact).
k::KineticSpec boardSpec() {
    k::KineticSpec spec;
    spec.states       = {kFacing};
    spec.rotationAxis = [](k::BlockState const& s) -> std::optional<k::Axis> {
        int f = s.get(kFacing, 0) & 3;
        return (f == 0 || f == 2) ? k::Axis::Z : k::Axis::X;
    };
    spec.hasShaftTowards = [axis = spec.rotationAxis](k::BlockState const& s, k::Face f) {
        auto a = axis(s);
        return a && *a == core::kinetics::axisOf(f);
    };
    spec.cog       = [](k::BlockState const&) { return k::CogType::Small; };
    spec.stressKey = "display_board";
    spec.impact    = 0;
    return spec;
}
LV_REGISTER_KINETIC_BLOCK(kBoardId, boardSpec());

} // namespace

// ---- structure --------------------------------------------------------------------------------

bool isBoard(std::string_view id) { return id == kBoardId; }

std::optional<Structure> structureOf(BlockSource& region, GlobalPos const& pos) {
    auto b = boardAt(region, pos);
    if (!b) return std::nullopt;
    int       facing = b->facing;
    GlobalPos p      = pos;
    // up to the top row (UP flags), then to the left end (identical states)
    for (int i = 0; i < kMaxSize; ++i) {
        auto cur = boardAt(region, p);
        if (!cur || !cur->up) break;
        GlobalPos above = p;
        above.y += 1;
        auto ab = boardAt(region, above);
        if (!ab || ab->facing != facing) break;
        p = above;
    }
    for (int i = 0; i < kMaxSize; ++i) {
        auto cur = boardAt(region, p);
        auto n   = step(p, clockwise(facing));
        auto nb  = boardAt(region, n);
        if (!nb || !cur || nb->facing != facing || nb->up != cur->up || nb->down != cur->down) break;
        p = n;
    }
    Structure s{p, facing, 1, 1};
    auto      top = boardAt(region, p);
    for (int i = 1; i < kMaxSize; ++i) {
        auto nb = boardAt(region, step(p, counterClockwise(facing), i));
        if (!nb || nb->facing != facing || nb->up != top->up || nb->down != top->down) break;
        s.width = i + 1;
    }
    GlobalPos q = p;
    for (int i = 0; i < kMaxSize - 1; ++i) {
        auto cur = boardAt(region, q);
        if (!cur || !cur->down) break;
        q.y -= 1;
        auto nb = boardAt(region, q);
        if (!nb || nb->facing != facing) break;
        s.height = i + 2;
    }
    return s;
}

void updateColumns(BlockSource& region, GlobalPos const& pos) {
    std::vector<GlobalPos> seeds;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int side = 0; side < 4; ++side) {
            GlobalPos p = offsetPos(pos, faceOfHorizontal(side));
            p.y += dy;
            seeds.push_back(p);
        }
        GlobalPos p = pos;
        p.y += dy;
        seeds.push_back(p);
    }
    for (auto const& p : seeds) fixRow(region, p);
    // a second pass settles rows whose neighbours changed in the first one
    for (auto const& p : seeds) fixRow(region, p);
    for (auto const& p : seeds) {
        if (auto s = structureOf(region, p); s && controllers().insert(s->controller).second) persist();
    }
}

int maxChars(int width) { return static_cast<int>((width * 16 - 2) / 3.5); }

bool running(GlobalPos const& controller) {
    // Create: getMinimumRequiredSpeedLevel() = MEDIUM
    return k::speedLevelOf(std::fabs(k::speedAt(controller))) >= k::SpeedLevel::Medium;
}

std::string lineText(GlobalPos const& controller, int line) {
    auto d = readLines(controller);
    if (line < 0 || line >= static_cast<int>(d.lines.size())) return {};
    return d.lines[static_cast<std::size_t>(line)].text;
}

void setLine(BlockSource& region, GlobalPos const& controller, int line, std::string const& text, bool manual) {
    auto s = structureOf(region, controller);
    if (!s) return;
    auto d = linesFor(*s);
    if (line < 0 || line >= static_cast<int>(d.lines.size())) return;
    auto& l  = d.lines[static_cast<std::size_t>(line)];
    l.text   = truncateGlyphs(text, maxChars(s->width));
    l.manual = manual && !text.empty();
    if (manual) l.owner.clear();
    writeLines(s->controller, d);
    if (controllers().insert(s->controller).second) persist();
    sendState(s->controller, renderState(region, *s));
}

void setLineSections(BlockSource& region, GlobalPos const& controller, int line, Lines const& sections) {
    auto s = structureOf(region, controller);
    if (!s) return;
    setLine(region, controller, line, composeSections(sections, maxChars(s->width)), false);
}

Json state(GlobalPos const& controller) {
    auto d = readLines(controller);
    Json j = Json::array();
    for (auto const& l : d.lines) j.push_back({{"t", l.text}, {"m", l.manual}, {"c", l.dye}, {"g", l.glow}});
    return j;
}

void syncAll(Player& player) {
    int dim = static_cast<int>(player.getDimensionId());
    for (auto const& [c, st] : lastSent())
        if (c.dim == dim) net::send(player, "display.board", st);
}

// ---- interaction ------------------------------------------------------------------------------

namespace {
double localY(GlobalPos const& pos, Vec3 const& hit) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(hit.x) && in(hit.y) && in(hit.z)) return hit.y;
    return hit.y - pos.y;
}
} // namespace

bool use(Player& player, BlockSource& region, GlobalPos const& pos, int face, Vec3 const& hit, ItemStack const& held) {
    if (player.isSneaking()) return false;
    auto s = structureOf(region, pos);
    if (!s) return false;
    double y = pos.y + localY(pos, hit);
    if (face == 1) y -= 0.125; // Create shifts the hit .125 into the block
    if (face == 0) y += 0.125;
    int  lines = s->height * 2;
    int  line  = std::clamp(static_cast<int>(std::floor(2 * (s->controller.y + 1 - y))), 0, lines - 1);
    auto id    = items::idOf(held);
    bool run   = running(s->controller);
    auto d     = linesFor(*s);
    auto& l    = d.lines[static_cast<std::size_t>(line)];

    if (id.empty()) {
        if (!run) return false;
        l.text.clear();
        l.manual = false;
        writeLines(s->controller, d);
        sendState(s->controller, renderState(region, *s));
        log().info("display board at {} line {} cleared by {}", s->controller.key(), line, player.getRealName());
        return true;
    }
    if (id == "minecraft:glow_ink_sac") {
        l.glow = true;
        writeLines(s->controller, d);
        playSound(region, pos, "sign.ink_sac.use");
        sendState(s->controller, renderState(region, *s));
        log().info("display board at {} line {} glowing", s->controller.key(), line);
        return true;
    }
    if (id == "minecraft:name_tag") {
        auto name = held.getCustomName();
        if (name.empty()) return false;
        setLine(region, s->controller, line, name, true);
        log().info("display board at {} line {} set from a name tag by {}", s->controller.key(), line, player.getRealName());
        return true;
    }
    if (id == kClipboardId) {
        auto entries = clipboard::lastViewedEntries(clipboard::ofItem(held));
        int  at      = line;
        for (auto const& e : entries) {
            std::size_t start = 0;
            while (at < lines) {
                auto nl = e.text.find('\n', start);
                setLine(region, s->controller, at++, e.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start), true);
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
        log().info("display board at {} written from a clipboard by {}", s->controller.key(), player.getRealName());
        return true;
    }
    if (auto dye = dyeOf(id)) {
        if (!run) return false;
        l.dye = *dye == 0 ? -1 : *dye; // white resets (Create stores null for WHITE)
        writeLines(s->controller, d);
        playSound(region, pos, "dye.use");
        sendState(s->controller, renderState(region, *s));
        log().info("display board at {} line {} dyed {}", s->controller.key(), line, kDyeNames[static_cast<std::size_t>(*dye)]);
        return true;
    }
    return false;
}

void placed(Player& player, BlockSource& region, GlobalPos const& pos, std::optional<int> clickedFacing) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    if (!b || b->getTypeName() != kBoardId) return;
    // Create: copies the facing of a board it is placed against (unless sneaking), else faces the player
    int facing = (clickedFacing && !player.isSneaking()) ? *clickedFacing : game::opposite(game::playerFacing(player));
    if (auto const* next = withStates(*b, {{kFacing, facing}, {kUp, 0}, {kDown, 0}})) game::setBlock(region, game::blockPos(pos), *next);
    updateColumns(region, pos);
    log().info("display board placed at {}", pos.key());
}

void removed(BlockSource& region, GlobalPos const& pos) { updateColumns(region, pos); }

std::optional<BlockPos> placementHelper(Player& player, BlockSource& region, GlobalPos const& pos, int, Vec3 const& hit) {
    if (player.isSneaking()) return std::nullopt;
    auto b = boardAt(region, pos);
    if (!b) return std::nullopt;
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    double hx = hit.x, hy = hit.y, hz = hit.z;
    if (!(in(hx) && in(hy) && in(hz))) hx -= pos.x, hy -= pos.y, hz -= pos.z;
    // Create's orderedByDistanceExceptAxis: neighbours off the facing axis, nearest to the hit first
    int  facingFace = faceOfHorizontal(b->facing);
    std::vector<std::pair<double, int>> dirs;
    for (int f = 0; f < 6; ++f) {
        if ((f >> 1) == (facingFace >> 1)) continue;
        double cx = 0.5 + kDx[f], cy = 0.5 + kDy[f], cz = 0.5 + kDz[f];
        dirs.emplace_back((cx - hx) * (cx - hx) + (cy - hy) * (cy - hy) + (cz - hz) * (cz - hz), f);
    }
    std::sort(dirs.begin(), dirs.end());
    for (auto const& [dist, f] : dirs) {
        BlockPos target{pos.x + kDx[f], pos.y + kDy[f], pos.z + kDz[f]};
        if (loadedBlock(region, target) && game::replaceable(region, target)) return target;
    }
    return std::nullopt;
}

// ---- ticking ----------------------------------------------------------------------------------

void trackPlayerDims(Player& p);

void tickAll(std::uint64_t tick) {
    if (tick % 20 == 5) {
        // companions that changed dimension get the boards and tubes of their new dimension
        if (auto level = ll::service::getLevel()) {
            level->forEachPlayer(std::function<bool(Player&)>([](Player& p) {
                trackPlayerDims(p);
                return true;
            }));
        }
    }
    if (tick % 10 != 0) return; // Create's lazy tick rate
    auto snapshot = controllers();
    for (auto const& c : snapshot) {
        auto* region = regionOf(c.dim);
        if (!region || !loadedBlock(*region, game::blockPos(c))) continue;
        if (!boardAt(*region, c)) {
            forget(c);
            continue;
        }
        refreshController(*region, c);
    }
}

void trackPlayerDims(Player& p) {
    auto id  = p.getOrCreateUniqueID().rawID;
    int  dim = static_cast<int>(p.getDimensionId());
    auto it  = playerDims().find(id);
    if (it != playerDims().end() && it->second == dim) return;
    playerDims()[id] = dim;
    if (net::isCompanion(p)) {
        syncAll(p);
        nixie::syncAll(p);
    }
}

// ---- display target ---------------------------------------------------------------------------

namespace {
std::string linkKey(GlobalPos const& p) { return p.key(); }

/// Create's DisplayTarget.reserve / isReserved: a line written by one link is skipped by others
/// while that link still targets this board.
bool mayWrite(BlockSource& region, Structure const& s, Line const& l, GlobalPos const& linkPos) {
    if (l.owner.empty() || l.owner == linkKey(linkPos)) return true;
    for (auto const& other : link::tracked()) {
        if (linkKey(other) != l.owner) continue;
        if (idAt(region, other) != kLinkId) return true;
        auto d = link::read(other);
        if (!d) return true;
        auto t = link::targetPos(other, *d);
        auto ts = structureOf(region, t);
        return !(ts && ts->controller == s.controller);
    }
    return true;
}

void acceptLines(BlockSource& region, GlobalPos const& target, int line, Lines const& text, Context& ctx) {
    auto s = structureOf(region, target);
    if (!s) return;
    auto d     = linesFor(*s);
    int  n     = static_cast<int>(d.lines.size());
    int  width = maxChars(s->width);
    for (int i = 0; i < static_cast<int>(text.size()) && line + i < n; ++i) {
        auto& l = d.lines[static_cast<std::size_t>(line + i)];
        if (!mayWrite(region, *s, l, ctx.link)) continue;
        l.owner  = linkKey(ctx.link);
        l.text   = truncateGlyphs(text[static_cast<std::size_t>(i)], width);
        l.manual = false;
    }
    writeLines(s->controller, d);
    if (controllers().insert(s->controller).second) persist();
    sendState(s->controller, renderState(region, *s));
}

struct TargetRegistration {
    TargetRegistration() {
        registerTarget(kBoardId, Target{
            .id    = "display_board",
            .stats = [](BlockSource& region, GlobalPos const& t) {
                auto s = structureOf(region, t);
                return TargetStats{s ? s->height * 2 : 2, s ? maxChars(s->width) : 4, "display_board"};
            },
            .accept = acceptLines,
            .acceptSections = [](BlockSource& region, GlobalPos const& t, int line, std::vector<Lines> const& sections, Context& ctx) {
                auto s     = structureOf(region, t);
                int  width = s ? maxChars(s->width) : 4;
                Lines composed;
                for (auto const& sec : sections) composed.push_back(composeSections(sec, width));
                acceptLines(region, t, line, composed, ctx);
            },
            .lineName = [](int line, Player const& p) { return trFor(p, "create.display_target.line", "Line {}", line + 1); },
        });
    }
};
TargetRegistration const gTarget;
} // namespace

} // namespace lv::feature::display::board
