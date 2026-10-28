// Client: text on display boards (split-flap animation) and nixie tubes (SPEC §3.6 Display).
//
// The server sends every board / tube state on the "display.board" / "display.nixie" channels
// (src/feature/display/Board.cpp, Nixie.cpp). This file keeps them per dimension and draws the
// characters with a 5x7 pixel font as coloured quads after the block entities of the opaque pass.
// The name tags vanilla clients see instead (entity levitate:display_text) are hidden here.

#ifdef LV_CLIENT

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "client/display/Font.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::display {

namespace {
namespace net = lv::feature::net;
using Json    = nlohmann::json;

struct V3 {
    double x = 0, y = 0, z = 0;
    V3     operator+(V3 const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3     operator-(V3 const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3     operator*(double s) const { return {x * s, y * s, z * s}; }
};

using Key = std::tuple<int, int, int, int>;

std::vector<std::string> glyphs(std::string const& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        auto c   = static_cast<unsigned char>(s[i]);
        int  len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        out.push_back(s.substr(i, static_cast<std::size_t>(len)));
        i += static_cast<std::size_t>(len);
    }
    return out;
}

// ---- split flaps ------------------------------------------------------------------------------
// Create's flap cycles (lang create.flap_display.cycles.*): letters (no W in Create's cycle),
// digits; anything else changes at once.
constexpr std::string_view kAlphabet = " ABCDEFGHIJKLMNOPQRSTUVXYZ";
constexpr std::string_view kNumeric  = " 0123456789";

struct Flap {
    std::string shown = " ";
    std::string target = " ";
};

struct BoardLine {
    std::vector<Flap> flaps;
    unsigned          colour = 0xD3C6BA;
    bool              glow   = false;
};

struct Board {
    int                    facing = 0, w = 1, h = 1;
    bool                   running = false, instant = false;
    std::vector<BoardLine> lines;
};

struct Tube {
    int         facing = 0, face = 0;
    unsigned    colour = 0xFF9A3C;
    std::string chars  = "  ";
};

std::mutex           gMutex;
std::map<Key, Board> gBoards;
std::map<Key, Tube>  gTubes;
bool                 gLogged = false;

/// One flap step towards the target (Create spins through the cycle of the target character).
void stepFlap(Flap& f, bool instant) {
    if (f.shown == f.target) return;
    if (instant) {
        f.shown = f.target;
        return;
    }
    auto inCycle = [](std::string_view cycle, std::string const& c) { return c.size() == 1 && cycle.find(c[0]) != std::string_view::npos; };
    std::string_view cycle = inCycle(kAlphabet, f.target) ? kAlphabet : inCycle(kNumeric, f.target) ? kNumeric : std::string_view{};
    if (cycle.empty() || !inCycle(cycle, f.shown)) {
        f.shown = cycle.empty() ? f.target : std::string(1, cycle[0]);
        return;
    }
    auto i  = cycle.find(f.shown[0]);
    f.shown = std::string(1, cycle[(i + 1) % cycle.size()]);
}

void onBoard(Json const& msg) {
    auto pos = msg.value("pos", Json::array());
    if (!pos.is_array() || pos.size() != 3) return;
    Key key{msg.value("dim", 0), pos[0].get<int>(), pos[1].get<int>(), pos[2].get<int>()};
    std::lock_guard lock(gMutex);
    if (msg.value("op", "") == "remove") {
        gBoards.erase(key);
        return;
    }
    auto& b   = gBoards[key];
    b.facing  = msg.value("facing", 0);
    b.w       = msg.value("w", 1);
    b.h       = msg.value("h", 1);
    b.running = msg.value("running", false);
    b.instant = msg.value("instant", false);
    auto lines = msg.value("lines", Json::array());
    b.lines.resize(lines.size());
    int chars = static_cast<int>((b.w * 16 - 2) / 3.5);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto& l  = b.lines[i];
        l.colour = lines[i].value("c", 0xD3C6BAu);
        l.glow   = lines[i].value("g", false);
        l.flaps.resize(static_cast<std::size_t>(chars));
        auto g = glyphs(lines[i].value("t", std::string{}));
        for (int c = 0; c < chars; ++c) l.flaps[static_cast<std::size_t>(c)].target = c < static_cast<int>(g.size()) ? g[static_cast<std::size_t>(c)] : " ";
    }
}

void onNixie(Json const& msg) {
    auto pos = msg.value("pos", Json::array());
    if (!pos.is_array() || pos.size() != 3) return;
    Key key{msg.value("dim", 0), pos[0].get<int>(), pos[1].get<int>(), pos[2].get<int>()};
    std::lock_guard lock(gMutex);
    if (msg.value("op", "") == "remove") {
        gTubes.erase(key);
        return;
    }
    auto& t  = gTubes[key];
    t.facing = msg.value("facing", 0);
    t.face   = msg.value("face", 0);
    t.colour = msg.value("colour", 0xFF9A3Cu);
    t.chars  = msg.value("chars", std::string("  "));
}

// ---- drawing ----------------------------------------------------------------------------------

struct Quad {
    std::array<V3, 4> v;
    float             r, g, b;
};

float lin(unsigned c) { return static_cast<float>(std::pow((c & 0xFF) / 255.0, 2.2)); }

/// Java 2D facing (0 S, 1 W, 2 N, 3 E) -> unit vector.
V3 dirOf(int facing) {
    static constexpr int dx[4]{0, -1, 0, 1}, dz[4]{1, 0, -1, 0};
    return {double(dx[facing & 3]), 0, double(dz[facing & 3])};
}

/// Adds the lit pixels of a glyph: `topLeft` corner, `right` / `down` unit vectors (blocks),
/// `pw` / `ph` pixel size in blocks.
void glyph(std::vector<Quad>& out, std::string const& ch, V3 topLeft, V3 right, V3 down, double pw, double ph, unsigned rgb, float bright) {
    auto const& g = glyphOf(ch);
    float r = lin(rgb >> 16) * bright, gg = lin(rgb >> 8) * bright, b = lin(rgb) * bright;
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (!(g[static_cast<std::size_t>(col)] >> row & 1)) continue;
            V3 a = topLeft + right * (col * pw) + down * (row * ph);
            out.push_back({{a, a + right * pw, a + right * pw + down * ph, a + down * ph}, r, gg, b});
        }
    }
}

void boardQuads(std::vector<Quad>& out, Key const& key, Board& b) {
    V3   front = dirOf(b.facing), left = dirOf(b.facing + 1); // the controller is at the clockwise (left) end
    V3   right = left * -1, down{0, -1, 0};
    V3   centre{std::get<1>(key) + 0.5, std::get<2>(key) + 0.5, std::get<3>(key) + 0.5};
    V3   corner = centre + front * (6.0 / 16 + 0.003) + left * 0.5 + V3{0, 0.5, 0};
    double px = 1.0 / 16;
    for (std::size_t li = 0; li < b.lines.size(); ++li) {
        auto& l = b.lines[li];
        for (std::size_t c = 0; c < l.flaps.size(); ++c) {
            auto& f = l.flaps[c];
            if (f.shown == " ") continue;
            V3 tl = corner + right * ((1 + 3.5 * double(c) + 0.25) * px) + down * ((8.0 * double(li) + 1.25) * px);
            glyph(out, f.shown, tl, right, down, 0.6 * px, 0.8 * px, l.colour, l.glow ? 1.25f : 1.0f);
        }
    }
}

void tubeQuads(std::vector<Quad>& out, Key const& key, Tube const& t) {
    V3 centre{std::get<1>(key) + 0.5, std::get<2>(key) + 0.5, std::get<3>(key) + 0.5};
    auto g = glyphs(t.chars);
    double px = 1.0 / 16;
    V3     right, down{0, -1, 0}, slot0, slot1;
    if (t.face >= 2) {
        // wall: tubes point out of the wall, the row runs top (left) to bottom
        V3 out_ = dirOf(t.facing);
        right   = dirOf(t.facing + 3);
        slot0   = centre + out_ * 0.25 + V3{0, 4 * px, 0};
        slot1   = centre + out_ * 0.25 + V3{0, -4 * px, 0};
        if (t.face == 3) std::swap(slot0, slot1);
    } else {
        // floor / ceiling: the row runs from left (opposite the facing) to right
        V3 rowDir = dirOf(t.facing);
        right     = rowDir;
        double y  = t.face == 0 ? 7.5 * px - 0.5 : 0.5 - 7.5 * px;
        slot0     = centre + rowDir * (-4 * px) + V3{0, y, 0};
        slot1     = centre + rowDir * (4 * px) + V3{0, y, 0};
    }
    double pw = 1.0 * px, ph = 1.3 * px;
    for (int s = 0; s < 2; ++s) {
        std::string ch = s < static_cast<int>(g.size()) ? g[static_cast<std::size_t>(s)] : " ";
        if (ch == " ") continue;
        V3 c  = s == 0 ? slot0 : slot1;
        V3 tl = c - right * (2.5 * pw) - down * (3.5 * ph);
        glyph(out, ch, tl, right, down, pw, ph, t.colour, 1.3f);
    }
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

using Clock = std::chrono::steady_clock;
Clock::time_point gLastStep = Clock::now();

void renderDisplays(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int dim = static_cast<int>(player->getDimensionId());
    std::unique_ptr<BaseActorRenderContext::Impl> const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 o3 = impl->mCameraTargetPosition;
    V3   o{o3.x, o3.y, o3.z};

    std::vector<Quad> quads;
    {
        std::lock_guard lock(gMutex);
        if (gBoards.empty() && gTubes.empty()) return;
        // the flaps advance once per game tick (50 ms) while the board turns
        bool step = Clock::now() - gLastStep >= std::chrono::milliseconds(50);
        if (step) gLastStep = Clock::now();
        for (auto& [key, b] : gBoards) {
            if (std::get<0>(key) != dim) continue;
            if (step && b.running)
                for (auto& l : b.lines)
                    for (auto& f : l.flaps) stepFlap(f, b.instant);
            V3 c{double(std::get<1>(key)), double(std::get<2>(key)), double(std::get<3>(key))};
            V3 d = c - o;
            if (d.x * d.x + d.y * d.y + d.z * d.z > 128.0 * 128.0) continue;
            boardQuads(quads, key, b);
        }
        for (auto const& [key, t] : gTubes) {
            if (std::get<0>(key) != dim) continue;
            V3 c{double(std::get<1>(key)), double(std::get<2>(key)), double(std::get<3>(key))};
            V3 d = c - o;
            if (d.x * d.x + d.y * d.y + d.z * d.z > 96.0 * 96.0) continue;
            tubeQuads(quads, key, t);
        }
    }
    if (quads.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, static_cast<int>(quads.size() * 4), false);
    for (auto const& q : quads) {
        tess.color(q.r, q.g, q.b, 1.0f);
        for (auto const& v : q.v) tess.vertex(float(v.x - o.x), float(v.y - o.y), float(v.z - o.z));
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLogged) {
        gLogged = true;
        log().info("display: rendering text ({} quads)", quads.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    DisplayRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        renderDisplays(renderContext);
    } catch (...) {
    }
}

class DisplayRendererModule {
public:
    void install() {
        net::client::on("display.board", [](Json const& msg) { onBoard(msg); });
        net::client::on("display.nixie", [](Json const& msg) { onNixie(msg); });
        DisplayRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mExit     = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gBoards.clear();
            gTubes.clear();
            gLogged = false;
        });
        // the client mod draws the text itself: hide the name tags meant for vanilla players
        mTick = bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            auto* p = currentPlayer();
            if (!p || !net::client::connected()) return;
            for (auto* a : p->getLevel().getRuntimeActorList()) {
                if (!a || a->getTypeName() != "levitate:display_text") continue;
                if (!a->getNameTag().empty()) a->setNameTag("");
            }
        });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        bus.removeListener(mExit);
        bus.removeListener(mTick);
        DisplayRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gBoards.clear();
        gTubes.clear();
    }

private:
    ll::event::ListenerPtr mExit, mTick;
};

/// Board / tube counts the client knows (debugging).
std::pair<std::size_t, std::size_t> knownDisplays() {
    std::lock_guard lock(gMutex);
    return {gBoards.size(), gTubes.size()};
}

} // namespace lv::client::display

LV_REGISTER_MODULE(lv::client::display::DisplayRendererModule);

#endif // LV_CLIENT
