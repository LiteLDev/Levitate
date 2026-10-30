// Factory gauge panels drawn by the client mod (U26, U42; Create's FactoryPanelRenderer): the
// count of each panel ("in storage / target", coloured as the server's label) under its item, and
// the arrows from the panels it takes input from (coloured by the panel's state: satisfied,
// promised, missing, no target). The items themselves are display items (server provider). The
// server sends `stock.gauge {dim, reset, gauges: [{pos, face, facing, panels: [{slot, count,
// colour, from: [{pos, slot, arrow}]}]}]}` when a gauge changes.

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"

#include "client/Client.h"
#include "client/display/Font.h"
#include "client/render/Outline.h"
#include "core/stock/Logistics.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::stock {

namespace {

namespace cs = core::stock;
using Json   = nlohmann::json;
using P3     = render::P3;

struct Source {
    std::array<int, 3> pos{};
    int                slot  = 0;
    int                arrow = 0;
};
struct Panel {
    int                 slot = 0;
    std::string         count;
    unsigned            colour = 0x888898;
    std::vector<Source> from;
};
struct Gauge {
    cs::AttachFace     face   = cs::AttachFace::Floor;
    int                facing = 0;
    std::vector<Panel> panels;
};

struct State {
    std::mutex                               mutex;
    int                                      dim = 0;
    std::map<std::array<int, 3>, Gauge>      gauges;
    bool                                     logged = false;
};
State& state() {
    static State s;
    return s;
}

std::array<int, 3> posOf(Json const& j) {
    if (!j.is_array() || j.size() != 3) return {0, 0, 0};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

void onGauge(Json const& msg) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    int             dim = msg.value("dim", 0);
    if (msg.value("reset", false) || dim != s.dim) s.gauges.clear();
    s.dim = dim;
    for (auto const& g : msg.value("gauges", Json::array())) {
        Gauge out;
        out.face   = static_cast<cs::AttachFace>(std::clamp(g.value("face", 0), 0, 2));
        out.facing = g.value("facing", 0);
        for (auto const& p : g.value("panels", Json::array())) {
            Panel panel;
            panel.slot   = std::clamp(p.value("slot", 0), 0, 3);
            panel.count  = p.value("count", std::string{});
            panel.colour = p.value("colour", 0x888898u);
            for (auto const& f : p.value("from", Json::array())) panel.from.push_back({posOf(f.value("pos", Json())), std::clamp(f.value("slot", 0), 0, 3), f.value("arrow", 0)});
            out.panels.push_back(std::move(panel));
        }
        auto pos = posOf(g.value("pos", Json()));
        if (out.panels.empty()) s.gauges.erase(pos);
        else s.gauges[pos] = std::move(out);
    }
}

/// The panel plane of a gauge: normal (out of the panel), right and up as seen from the front.
struct Frame {
    P3 normal, right, up;
};
Frame frameOf(Gauge const& g) {
    auto s = cs::supportOffset(g.face, g.facing);
    P3   n{-double(s[0]), -double(s[1]), -double(s[2])};
    P3   up{0, 1, 0};
    if (n[1] != 0) {
        // floor / ceiling: the top of the panel points away from where the player stood
        auto [fx, fz] = cs::facingVector(g.facing);
        up            = {-double(fx), 0, -double(fz)};
        if (n[1] < 0) up = {double(fx), 0, double(fz)};
    }
    P3 right{up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
    return {n, right, up};
}

P3 add(P3 a, P3 const& b, double k = 1) {
    for (int i = 0; i < 3; ++i) a[i] += b[i] * k;
    return a;
}

P3 panelPoint(std::array<int, 3> const& pos, Gauge const& g, int slot, Frame const& f) {
    auto c = cs::panelCentre(g.face, g.facing, static_cast<cs::PanelSlot>(slot));
    return add({pos[0] + c.x, pos[1] + c.y, pos[2] + c.z}, f.normal, .015);
}

/// Formatting codes of the count label to colours (the codes the server uses).
unsigned codeColour(char c, unsigned fallback) {
    switch (c) {
    case 'a': return 0x55FF55;
    case '6': return 0xFFAA00;
    case 'c': return 0xFF5555;
    case 'f': return 0xFFFFFF;
    case '7': return 0xAAAAAA;
    default: return fallback;
    }
}

/// The font's glyphs plus Create's count symbols: ∞, the stacks mark ▤ and the promised arrow ⏶.
display::Glyph const& glyphOf(std::string const& ch) {
    static constexpr display::Glyph kInfinity{0x18, 0x24, 0x18, 0x24, 0x18};
    static constexpr display::Glyph kStacks{0x7F, 0x49, 0x49, 0x49, 0x7F};
    static constexpr display::Glyph kPromised{0x10, 0x18, 0x1C, 0x18, 0x10};
    if (ch == "\xE2\x88\x9E") return kInfinity;
    if (ch == "\xE2\x96\xA4") return kStacks;
    if (ch == "\xE2\x8F\xB6") return kPromised;
    return display::glyphOf(ch);
}

/// Text in the 5x7 pixel font, centred on `at` in the panel plane (`px` = one font pixel).
void drawText(render::Lines& lines, std::string const& text, P3 const& at, Frame const& f, double px) {
    // glyphs with their colours
    std::vector<std::pair<std::string, unsigned>> glyphs;
    unsigned                                      colour = 0xFFFFFF;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (text.compare(i, 2, "\xC2\xA7") == 0 && i + 2 < text.size()) { // §x
            colour = codeColour(text[i + 2], colour);
            i += 3;
            continue;
        }
        std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        auto        ch  = text.substr(i, len);
        glyphs.emplace_back(ch, colour);
        i += len;
    }
    double width = static_cast<double>(glyphs.size()) * 6 - 1;
    P3     origin = add(add(at, f.right, -width / 2 * px), f.up, 3.5 * px); // top-left of the text
    double depth  = px * .2;
    for (std::size_t g = 0; g < glyphs.size(); ++g) {
        auto const& glyph = glyphOf(glyphs[g].first);
        auto        rgb   = render::Rgb::hex(glyphs[g].second);
        for (int col = 0; col < 5; ++col) {
            // one box per run of lit pixels in the column (fewer quads per frame)
            for (int row = 0; row < 7;) {
                if (!(glyph[static_cast<std::size_t>(col)] >> row & 1)) {
                    ++row;
                    continue;
                }
                int end = row;
                while (end < 7 && (glyph[static_cast<std::size_t>(col)] >> end & 1)) ++end;
                P3 a = add(add(origin, f.right, (static_cast<double>(g) * 6 + col) * px), f.up, -row * px);
                P3 b = add(add(a, f.right, px), f.up, -(end - row) * px);
                row  = end;
                P3 lo{std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2])};
                P3 hi{std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])};
                for (int i = 0; i < 3; ++i)
                    if (f.normal[i] != 0) lo[i] -= depth, hi[i] += depth;
                lines.box(lo, hi, rgb);
            }
        }
    }
}

/// Create's connection path: from the input panel to this one along the panel plane, the bend as
/// the wrench set it (0: across first, 1: up first, 2 / 3: bent in the middle), an arrow head at
/// this panel.
void drawPath(render::Lines& lines, P3 const& from, P3 const& to, Frame const& f, int arrow, render::Rgb rgb) {
    auto   dot = [](P3 const& a, P3 const& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    P3     d{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    double du = dot(d, f.right), dv = dot(d, f.up);
    std::vector<P3> pts{from};
    switch (((arrow % 4) + 4) % 4) {
    case 0: pts.push_back(add(from, f.right, du)); break;
    case 1: pts.push_back(add(from, f.up, dv)); break;
    case 2:
        pts.push_back(add(from, f.right, du / 2));
        pts.push_back(add(add(from, f.right, du / 2), f.up, dv));
        break;
    default:
        pts.push_back(add(from, f.up, dv / 2));
        pts.push_back(add(add(from, f.up, dv / 2), f.right, du));
        break;
    }
    pts.push_back(to);
    double w = 1.0 / 32;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) lines.line(pts[i], pts[i + 1], rgb, w);
    // the arrow head: two short strokes back from the end along the last segment
    P3     last{to[0] - pts[pts.size() - 2][0], to[1] - pts[pts.size() - 2][1], to[2] - pts[pts.size() - 2][2]};
    double len = std::sqrt(dot(last, last));
    if (len < 1e-4) return;
    P3 back{-last[0] / len, -last[1] / len, -last[2] / len};
    P3 side{back[1] * f.normal[2] - back[2] * f.normal[1], back[2] * f.normal[0] - back[0] * f.normal[2], back[0] * f.normal[1] - back[1] * f.normal[0]};
    double h = .08;
    for (int s : {-1, 1}) {
        P3 tip = to;
        for (int k = 1; k <= 4; ++k) {
            P3 p = add(add(to, back, h * k / 4), side, s * h * k / 4 * .8);
            lines.line(tip, p, rgb, w);
            tip = p;
        }
    }
}

void renderGauges(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player || !feature::net::client::connected()) return;
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    if (s.gauges.empty() || static_cast<int>(player->getDimensionId()) != s.dim) return;
    auto const&   eye = player->getPosition();
    render::Lines lines(ctx);
    int           drawn = 0;
    for (auto const& [pos, g] : s.gauges) {
        double dx = pos[0] + .5 - eye.x, dy = pos[1] + .5 - eye.y, dz = pos[2] + .5 - eye.z;
        if (dx * dx + dy * dy + dz * dz > 64.0 * 64.0) continue;
        auto f = frameOf(g);
        for (auto const& panel : g.panels) {
            P3 centre = panelPoint(pos, g, panel.slot, f);
            if (!panel.count.empty()) drawText(lines, panel.count, add(centre, f.up, -.16), f, 1.0 / 96);
            for (auto const& src : panel.from) {
                auto it = s.gauges.find(src.pos);
                if (it == s.gauges.end()) continue;
                P3 from = panelPoint(src.pos, it->second, src.slot, frameOf(it->second));
                drawPath(lines, from, centre, f, src.arrow, render::Rgb::hex(panel.colour));
            }
            ++drawn;
        }
    }
    if (drawn > 0 && !s.logged) {
        s.logged = true;
        log().info("stock client: drawing {} gauge panels", drawn);
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    StockGaugeRenderHook,
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
        renderGauges(renderContext);
    } catch (...) {
    }
}

class StockGaugeRendererModule {
public:
    void install() {
        feature::net::client::on("stock.gauge", onGauge);
        StockGaugeRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto&           s = state();
            std::lock_guard lock(s.mutex);
            s.gauges.clear();
            s.logged = false;
        });
    }
    void uninstall() {
        StockGaugeRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::stock

LV_REGISTER_MODULE(lv::client::stock::StockGaugeRendererModule);

#endif // LV_CLIENT
